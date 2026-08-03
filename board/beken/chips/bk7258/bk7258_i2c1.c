/****************************************************************************
 * board/beken/chips/bk7258/bk7258_i2c1.c
 *
 * Interrupt-driven hardware I2C1 master-write driver.  See
 * include/bk7258_i2c1.h for the full rationale (why this board needs
 * hardware I2C1 on GPIO42/43 instead of the previous bk7258_i2c_sim.c
 * bit-banged GPIO0/1 driver).
 *
 * State machine and register-access sequencing are a from-scratch
 * reimplementation of bk_avdk_smp release/v3.1.1
 * ap/middleware/driver/i2c/i2c_driver.c's I2C_MASTER_WRITE path
 * (i2c_hardware_memory_write_impl() -> i2c_master_start() ->
 * i2c_master_set_write_dev_addr() -> i2c_master_write_mem_addr_low_8bit()
 * -> i2c1_master_write_data() -> i2c_master_stop()), driven by
 * i2c1_isr_common()'s per-byte-ACK interrupt handling.  Register bit
 * layout: hardware/bk7258_i2c1.h (ported from that release's
 * i2c_struct.h/i2c_reg.h).
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#include <nuttx/config.h>
#include <nuttx/arch.h>
#include <nuttx/irq.h>
#include <nuttx/semaphore.h>
#include <nuttx/spinlock.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "arm_internal.h"
#include "irq.h"
#include "bk7258_gpio.h"
#include "bk7258_i2c1.h"
#include "hardware/bk7258_i2c1.h"
#include "hardware/bk7258_sysctrl.h"

/* GPIO42/GPIO43 pinmux function index for I2C1_SCL/I2C1_SDA.
 *
 * bk_avdk_smp ap/middleware/soc/bk7258_ap/soc/gpio_map.h's per-pin
 * function array for GPIO_42 is:
 *   {GPIO_DEV_I2C1_SCL, GPIO_DEV_I2S2_DIN, GPIO_DEV_LIN_SLEEP,
 *    GPIO_DEV_SCR_RSTN, GPIO_DEV_LCD_G2, GPIO_DEV_DEBUG30,
 *    GPIO_DEV_INVALID, GPIO_DEV_SLCD_SEG1}
 * i.e. GPIO_DEV_I2C1_SCL is array index 0.  GPIO_43's array is the same
 * shape with GPIO_DEV_I2C1_SDA at index 0.  bk7258_gpio_set_function()'s
 * "function" parameter is this array index (see bk7258_gpio.c: it writes
 * `function & 0xf` into the 4-bit-per-pin GPIO_SYS_CFG pinmux-select
 * field). */
#define BK7258_I2C1_SCL_PIN       42u
#define BK7258_I2C1_SDA_PIN       43u
#define BK7258_I2C1_PINMUX_FUNCTION 0u

/* 100kHz standard-mode I2C, matching GC2145's supported bus speed and
 * gsensor_sc7a20.c's i2c_cfg.baud_rate=100000 convention for this SoC
 * family. */
#define BK7258_I2C1_BAUD_RATE     100000u

/* freq_div formula: bk_avdk_smp ap/middleware/soc/common/hal/i2c_hal.c
 * I2C_CLK_DIVID(rate) = ROUND_UP(ROUND_UP(CONFIG_XTAL_FREQ, rate) - 6, 3)
 * - 1.  CONFIG_XTAL_FREQ for this board's 26MHz crystal (schematic sheet
 * 2/6, "26M,12pF,85C" crystal note) is 26000000. */
#define BK7258_I2C1_XTAL_FREQ     26000000u
#define BK7258_I2C1_ROUND_UP(a, b) (((a) / (b)) + (((a) % (b)) ? 1u : 0u))
#define BK7258_I2C1_FREQ_DIV(rate) \
  (BK7258_I2C1_ROUND_UP(BK7258_I2C1_ROUND_UP(BK7258_I2C1_XTAL_FREQ, (rate)) - 6u, 3u) - 1u)

/* Master-write state sequence, mirroring i2c_driver.c's
 * i2c_master_status_t (only the subset this write-only driver needs). */
enum bk7258_i2c1_state_e
{
  BK7258_I2C1_STATE_IDLE = 0,
  BK7258_I2C1_STATE_TX_DEV_ADDR,
  BK7258_I2C1_STATE_TX_REG_ADDR,
  BK7258_I2C1_STATE_TX_DATA,
};

static sem_t g_i2c1_done_sem;
static volatile enum bk7258_i2c1_state_e g_i2c1_state;
static volatile bool g_i2c1_ack_ok;
static uint8_t g_i2c1_dev_addr;
static uint8_t g_i2c1_reg_addr;
static uint8_t g_i2c1_data;

static void bk7258_i2c1_stop(void)
{
  /* Per i2c_ll_enable_stop()'s comment in the reference driver: sm_int
   * (bit[0]) and stop (bit[9]) must be written together in the same
   * write, otherwise the hardware will not actually issue a STOP
   * condition on the bus. */
  putreg32(BK7258_I2C1_STATUS_SM_INT | BK7258_I2C1_STATUS_STOP,
           BK7258_I2C1_INT_STATUS);
  g_i2c1_state = BK7258_I2C1_STATE_IDLE;
}

static int bk7258_i2c1_isr(int irq, void *context, void *arg)
{
  uint32_t int_status = getreg32(BK7258_I2C1_INT_STATUS);

  if ((int_status & BK7258_I2C1_STATUS_SM_INT) == 0)
    {
      /* Not the per-byte state-machine interrupt (e.g. scl_timeout,
       * arb_lost): clear whatever fired and bail out without touching
       * the write sequence, matching i2c1_isr_common()'s early return
       * for the same condition. */
      putreg32(int_status, BK7258_I2C1_INT_STATUS);
      return 0;
    }

  /* ack (bit[8]) reads as 1 when the previous byte was ACKed by the
   * slave, per i2c_ll_is_rx_ack_triggered()/i2c_ll_is_tx_ack_triggered()
   * both testing BIT(8) -- the same status bit serves both roles
   * depending on tx_mode, which master-write transactions always are. */
  bool acked = (int_status & BK7258_I2C1_STATUS_ACK) != 0;

  if (!acked)
    {
      g_i2c1_ack_ok = false;
      bk7258_i2c1_stop();
      nxsem_post(&g_i2c1_done_sem);
      /* Clear the interrupt (sm_int alone; no stop/start bit needed here
       * since bk7258_i2c1_stop() already issued its own combined
       * sm_int|stop write above). */
      putreg32(BK7258_I2C1_STATUS_SM_INT, BK7258_I2C1_INT_STATUS);
      return 0;
    }

  switch (g_i2c1_state)
    {
      case BK7258_I2C1_STATE_TX_DEV_ADDR:
        g_i2c1_state = BK7258_I2C1_STATE_TX_REG_ADDR;
        putreg32(g_i2c1_reg_addr & BK7258_I2C1_DATA_MASK,
                 BK7258_I2C1_SM_BUS_DATA);
        putreg32(BK7258_I2C1_STATUS_SM_INT, BK7258_I2C1_INT_STATUS);
        break;

      case BK7258_I2C1_STATE_TX_REG_ADDR:
        g_i2c1_state = BK7258_I2C1_STATE_TX_DATA;
        putreg32(g_i2c1_data & BK7258_I2C1_DATA_MASK,
                 BK7258_I2C1_SM_BUS_DATA);
        putreg32(BK7258_I2C1_STATUS_SM_INT, BK7258_I2C1_INT_STATUS);
        break;

      case BK7258_I2C1_STATE_TX_DATA:
        g_i2c1_ack_ok = true;
        bk7258_i2c1_stop();
        nxsem_post(&g_i2c1_done_sem);
        putreg32(BK7258_I2C1_STATUS_SM_INT, BK7258_I2C1_INT_STATUS);
        break;

      default:
        putreg32(int_status, BK7258_I2C1_INT_STATUS);
        break;
    }

  return 0;
}

void bk7258_i2c1_init(void)
{
  uint32_t freq_div = BK7258_I2C1_FREQ_DIV(BK7258_I2C1_BAUD_RATE);

  nxsem_init(&g_i2c1_done_sem, 0, 0);
  g_i2c1_state = BK7258_I2C1_STATE_IDLE;

  /* Enable the I2C1 *module* clock gate (BK_avdk_smp's
   * sys_drv_dev_clk_pwr_up(CLK_PWR_ID_I2C2, ...), called from
   * i2c_clock_enable(I2C_ID_1) in the reference i2c_driver.c).  This is
   * a separate, more fundamental gate than sm_bus_cfg.freq_div (the bus
   * baud-rate divider) or global_ctrl.clk_gate_bypass (an internal
   * ack-bit-visibility bypass): without this bit, the I2C1 block's
   * state machine receives no clock at all, so writing the start bit
   * in bk7258_i2c1_write_reg() is accepted by the register interface
   * (which is on a different, always-clocked bus) but never actually
   * advances the transaction -- sm_int never fires, indistinguishable
   * from a dead bus purely from register-level symptoms.  See
   * hardware/bk7258_sysctrl.h's BK7258_I2C1_MODULE_CLK_EN comment for
   * the register-map citation. */
  modifyreg32(BK7258_SYS_DEVCLK_EN, 0, BK7258_I2C1_MODULE_CLK_EN);

  /* Route GPIO42/GPIO43 to the I2C1_SCL/I2C1_SDA hardware function.
   *
   * NOTE: bk7258_gpio_input_pullup() must NOT be called here (as an
   * earlier version of this driver did, attempting to also enable the
   * pins' internal pull-ups): it unconditionally clears
   * BK7258_GPIO_SECOND_FUNCTION (see bk7258_gpio.c), and
   * bk7258_gpio_set_function() unconditionally clears
   * BK7258_GPIO_PULL_ENABLE (see the same file) -- calling both back to
   * back, in either order, leaves one of "second function selected" or
   * "internal pull-up enabled" clobbered by the other's write.  This
   * board's schematic (sheet 4/6) already provides external 4.7K
   * pull-ups to VDDGPIO on both IIC1_SCL/IIC1_SDA nets, so the internal
   * pull-up is not required for correct bus operation; only the pinmux
   * function selection matters here. */
  bk7258_gpio_set_function(BK7258_I2C1_SCL_PIN, BK7258_I2C1_PINMUX_FUNCTION);
  bk7258_gpio_set_function(BK7258_I2C1_SDA_PIN, BK7258_I2C1_PINMUX_FUNCTION);

  /* Soft-reset then configure sm_bus_cfg, matching i2c_hal_init_instance()
   * (soft_reset=1, then zero sm_bus_cfg/sm_bus_status) followed by
   * i2c_hal_configure()'s field-by-field setup. */
  putreg32(BK7258_I2C1_SOFT_RESET, BK7258_I2C1_GLOBAL_CTRL);
  putreg32(0, BK7258_I2C1_SM_BUS_CFG);
  putreg32(0, BK7258_I2C1_INT_STATUS);

  putreg32((freq_div << BK7258_I2C1_CFG_FREQ_DIV_SHIFT) &
           BK7258_I2C1_CFG_FREQ_DIV_MASK,
           BK7258_I2C1_SM_BUS_CFG);

  /* idle_cr=0x3, scl_cr=0x4, clk_src=0x3, timeout_en=1, idle_det_en=1,
   * inh=0 (i2c_ll_enable_slave() clears inh to 0 even though this driver
   * never acts as a slave, matching i2c_hal_configure()'s call order:
   * enable_slave() runs after the cr/clk_src/timeout/idle_det writes). */
  modifyreg32(BK7258_I2C1_SM_BUS_CFG,
              BK7258_I2C1_CFG_IDLE_CR_MASK | BK7258_I2C1_CFG_SCL_CR_MASK |
              BK7258_I2C1_CFG_CLK_SRC_MASK,
              (0x3u << BK7258_I2C1_CFG_IDLE_CR_SHIFT) |
              (0x4u << BK7258_I2C1_CFG_SCL_CR_SHIFT) |
              (0x3u << BK7258_I2C1_CFG_CLK_SRC_SHIFT));
  modifyreg32(BK7258_I2C1_SM_BUS_CFG, BK7258_I2C1_CFG_INH,
              BK7258_I2C1_CFG_TIMEOUT_EN | BK7258_I2C1_CFG_IDLE_DET_EN);

  irq_attach(BK7258_IRQ_I2C1, bk7258_i2c1_isr, NULL);
  up_enable_irq(BK7258_IRQ_I2C1);

  /* Enable the peripheral (sm_bus_cfg.en=1), also bypassing the clock
   * gate so the ack bit is actually readable in the ISR -- per
   * i2c_ll_enable()'s comment: "need open clock bypass, otherwise cannot
   * read ack(BIT8) in i2c_isr". */
  putreg32(BK7258_I2C1_CLK_GATE_BYPASS, BK7258_I2C1_GLOBAL_CTRL);
  modifyreg32(BK7258_I2C1_SM_BUS_CFG, 0, BK7258_I2C1_CFG_EN);
}

bool bk7258_i2c1_write_reg(uint8_t i2c_addr, uint8_t reg, uint8_t value)
{
  irqstate_t flags;
  int ret;
  int busy_wait_us;

  /* Wait for the bus to report idle before issuing a new START,
   * matching i2c_hardware_memory_write_impl()'s
   * i2c_wait_sm_bus_idle() call.  sm_bus_status.busy (bit[15]) lives in
   * the same register as INT_STATUS (both are REG_0x05 per
   * i2c_struct.h's sm_bus_status union), so BK7258_I2C1_INT_STATUS is
   * read here rather than a separate "status" register.  Without this
   * check, if busy is stuck set (e.g. right after
   * bk7258_i2c1_init() enables the peripheral while the external bus
   * lines are still settling), writing the start bit below would be
   * silently ignored by the hardware state machine -- producing
   * exactly the "interrupt never fires, write_reg() times out"
   * symptom, indistinguishable from a dead/unwired bus without this
   * check to rule it out. */
  for (busy_wait_us = 0;
       (getreg32(BK7258_I2C1_INT_STATUS) &
        BK7258_I2C1_STATUS_BUSY) != 0 && busy_wait_us < 10000;
       busy_wait_us += 100)
    {
      up_udelay(100);
    }

  g_i2c1_dev_addr = i2c_addr;
  g_i2c1_reg_addr = reg;
  g_i2c1_data = value;
  g_i2c1_ack_ok = false;

  flags = enter_critical_section();

  /* Write the (7-bit addr << 1 | write-bit=0) byte first, then set
   * start=1, matching i2c_master_set_write_dev_addr()'s comment: "write
   * slave_addr first, then enable start bit, otherwise the slave_addr
   * written will be zero." */
  putreg32((uint32_t)(i2c_addr << 1) & BK7258_I2C1_DATA_MASK,
           BK7258_I2C1_SM_BUS_DATA);
  g_i2c1_state = BK7258_I2C1_STATE_TX_DEV_ADDR;
  modifyreg32(BK7258_I2C1_INT_STATUS, 0, BK7258_I2C1_STATUS_START);

  leave_critical_section(flags);

  /* Bounded wait: 100ms is generous for a 3-byte 100kHz transaction
   * (~270us of bus time) plus scheduling jitter, while still failing
   * fast if the slave never responds (e.g. wrong wiring) rather than
   * hanging board_late_initialize() indefinitely. */
  ret = nxsem_tickwait_uninterruptible(&g_i2c1_done_sem, MSEC2TICK(100));
  if (ret < 0)
    {
      /* Timed out: the ISR never ran to completion (either the I2C1
       * interrupt never fired at all -- e.g. pinmux/clock
       * misconfiguration, or a hardware issue independent of ACK/NACK
       * -- or it fired but got stuck mid-sequence).  This is a
       * distinct failure mode from a clean NACK (which nxsem_post()s
       * promptly with g_i2c1_ack_ok=false) and is logged differently
       * here so board bring-up logs can tell "the bus electrically
       * never responded at all" apart from "the slave was reachable
       * and explicitly rejected". */
      printf("i2c1: write_reg(addr=0x%02x, reg=0x%02x) timed out "
             "waiting for the I2C1 interrupt (int_status=0x%08x, "
             "sm_bus_cfg=0x%08x) -- the ISR never completed the "
             "transaction\n",
             i2c_addr, reg, (unsigned int)getreg32(BK7258_I2C1_INT_STATUS),
             (unsigned int)getreg32(BK7258_I2C1_SM_BUS_CFG));

      /* Timed out: force the state machine back to idle so the next
       * write_reg() call starts clean, and disable+re-enable the
       * peripheral to clear any stuck bus condition, matching
       * i2c1_isr_common()'s SCL-timeout recovery path
       * (i2c_hal_disable() + i2c_hal_enable()). */
      g_i2c1_state = BK7258_I2C1_STATE_IDLE;
      modifyreg32(BK7258_I2C1_SM_BUS_CFG, BK7258_I2C1_CFG_EN, 0);
      modifyreg32(BK7258_I2C1_SM_BUS_CFG, 0, BK7258_I2C1_CFG_EN);
      return false;
    }

  if (!g_i2c1_ack_ok)
    {
      printf("i2c1: write_reg(addr=0x%02x, reg=0x%02x) got a clean "
             "NACK (the I2C1 interrupt fired and completed, but the "
             "slave never pulled ACK) -- the bus is electrically "
             "responsive, the device at this address is not\n",
             i2c_addr, reg);
    }

  return g_i2c1_ack_ok;
}
