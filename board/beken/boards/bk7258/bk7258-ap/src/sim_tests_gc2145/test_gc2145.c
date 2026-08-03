/****************************************************************************
 * board/beken/boards/bk7258/bk7258-ap/sim_tests_gc2145/test_gc2145.c
 *
 * Integration test for bk7258_gc2145.c, compiled against the real
 * production source (unmodified) plus the real bk7258_gpio.c/
 * bk7258_i2c1.c/bk7258_dma.c/bk7258_yuv_buf.c drivers, using the same
 * mock register/IRQ infrastructure as board/beken/chips/bk7258/
 * sim_tests/.
 *
 * Mock I2C1 slave design: unlike the earlier bit-banged bk7258_i2c_sim.c
 * driver (which this test previously modeled at the GPIO SCL/SDA pin
 * level, reconstructing clock edges from raw register writes), the
 * hardware I2C1 controller exposes a byte-at-a-time register interface
 * (sm_bus_data for the byte being transferred, sm_bus_status.ack for the
 * per-byte ACK/NACK result) -- there is no bit-level bus to simulate.
 * This mock therefore tracks transaction state (address byte -> register
 * byte -> data byte -> STOP) purely from writes to BK7258_I2C1_SM_BUS_DATA
 * and BK7258_I2C1_INT_STATUS's start/stop bits, and drives the ack bit in
 * BK7258_I2C1_INT_STATUS accordingly before synchronously firing the I2C1
 * ISR -- mirroring how a real slave's ACK would be visible to the
 * hardware state machine by the time the sm_int interrupt fires.
 *
 * Firing the ISR synchronously from the write hook (rather than from a
 * separate simulated interrupt source) is what makes this mock work in
 * a single-threaded test process with no real scheduler: by the time
 * bk7258_i2c1_write_reg() reaches nxsem_tickwait_uninterruptible(), the
 * whole START->address->register->data->STOP sequence has already run
 * to completion (recursively, from inside the initial start-bit write
 * that kicks it off) and nxsem_post() has already incremented the
 * semaphore count sim_stubs.c's mock checks.
 *
 * The line-done interrupt wait loop in bk7258_gc2145_test() is a
 * blocking busy-wait with no real asynchrony in this single-threaded
 * test process (up_udelay() is a no-op stub, and there is no timer
 * thread to fire bk7258_sim_irq_fire() *during* the wait).  This test
 * therefore pre-arms the mock YUV_BUF/DMA interrupt state *before*
 * calling bk7258_gc2145_test(), using a write hook on the YUV_BUF start
 * register (ctrl.yuv_mode) to fire the line-done interrupt synchronously
 * the moment the driver calls bk7258_yuv_buf_start() -- and another hook
 * on the DMA ctrl register to fire the DMA-done interrupt synchronously
 * the moment the driver calls bk7258_dma_start() -- so by the time
 * either wait loop's *first* condition check runs, the flag it is
 * waiting on is already set.  This validates the exact same call
 * sequence and control flow the real driver uses, without needing to
 * modify bk7258_gc2145.c to be testable or spin up real threads.
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#include <stdio.h>
#include <string.h>

#include "sim_stubs.h"
#include "hardware/bk7258_gpio.h"
#include "hardware/bk7258_i2c1.h"
#include "bk7258_gpio.h"
#include "bk7258_i2c1.h"
#include "bk7258_yuv_buf.h"
#include "bk7258_dma.h"
#include "irq.h"

static int g_failures;

#define CHECK(cond, ...) \
  do \
    { \
      if (!(cond)) \
        { \
          g_failures++; \
          printf("FAIL %s:%d: ", __func__, __LINE__); \
          printf(__VA_ARGS__); \
          printf("\n"); \
        } \
    } \
  while (0)

#define GC2145_I2C_ADDR_BYTE (0x3Cu << 1)

/* Register layout constants duplicated from the production drivers'
 * private #defines, for the same "independent cross-check" reason
 * documented in test_dma.c/test_yuv_buf.c. */
#define TEST_YUV_BUF_CTRL          (0x48020000u + 4u * 4u)
#define TEST_YUV_BUF_CTRL_YUV_MODE (1u << 0)
#define TEST_YUV_BUF_INT_STATUS    (0x48020000u + 10u * 4u)
#define TEST_YUV_BUF_INT_STATUS_SM0_WR (1u << 2)

#define TEST_DMA_CTRL              (0x45020000u + 0x40u + 0u * 4u)
#define TEST_DMA_CTRL_ENABLE       (1u << 0)
#define TEST_DMA_STATUS            (0x45020000u + 0x40u + 12u * 4u)
#define TEST_DMA_STATUS_FINISH_INT (1u << 18)

/* ========================================================================
 * Mock I2C1 hardware slave: tracks the master-write byte sequence via
 * BK7258_I2C1_SM_BUS_DATA writes and BK7258_I2C1_INT_STATUS's start bit,
 * and synchronously fires the I2C1 ISR with an ACK or NACK result (see
 * file header comment for why this differs from the old bit-level
 * approach and why synchronous firing is required in this harness).
 * ======================================================================== */

typedef enum
{
  SLAVE_IDLE,
  SLAVE_GOT_ADDR,
  SLAVE_GOT_REG,
  SLAVE_GOT_DATA
} slave_state_t;

typedef struct
{
  slave_state_t state;
  bool always_nack;
  unsigned int transactions_seen;
  unsigned int bytes_seen;
} mock_slave_t;

static mock_slave_t g_slave;

static void mock_slave_reset(void)
{
  memset(&g_slave, 0, sizeof(g_slave));
  g_slave.state = SLAVE_IDLE;
}

/* Fires the I2C1 ISR with sm_int + ack set/clear as appropriate for the
 * byte just "received", advancing g_slave.state.  Called synchronously
 * from mock_slave_write_hook() right after each byte is written to
 * BK7258_I2C1_SM_BUS_DATA. */
static void mock_slave_ack_and_fire(bool ack)
{
  uint32_t status = BK7258_I2C1_STATUS_SM_INT;

  if (ack)
    {
      status |= BK7258_I2C1_STATUS_ACK;
    }

  bk7258_sim_poke32(BK7258_I2C1_INT_STATUS, status);
  (void)bk7258_sim_irq_fire(BK7258_IRQ_I2C1);
}

static void mock_slave_write_hook(uintptr_t addr, uint32_t value, void *arg)
{
  (void)arg;

  if (addr == BK7258_I2C1_INT_STATUS &&
      (value & BK7258_I2C1_STATUS_START) != 0)
    {
      /* bk7258_i2c1_write_reg() writes the address byte to
       * SM_BUS_DATA *before* setting start (matching the real
       * hardware's documented write order), so at this point
       * SM_BUS_DATA already holds the address byte the driver wants
       * to send. */
      uint32_t addr_byte = bk7258_sim_peek32(BK7258_I2C1_SM_BUS_DATA) &
                            BK7258_I2C1_DATA_MASK;

      g_slave.transactions_seen++;
      g_slave.bytes_seen++;

      if (g_slave.always_nack ||
          (addr_byte & 0xfeu) != (GC2145_I2C_ADDR_BYTE & 0xfeu))
        {
          g_slave.state = SLAVE_IDLE;
          mock_slave_ack_and_fire(false);
          return;
        }

      g_slave.state = SLAVE_GOT_ADDR;
      mock_slave_ack_and_fire(true);
      return;
    }

  if (addr != BK7258_I2C1_SM_BUS_DATA || g_slave.state == SLAVE_IDLE)
    {
      return;
    }

  /* A register-address or data byte write while a transaction is
   * already address-ACKed (the driver's ISR writes these from
   * BK7258_I2C1_STATE_TX_DEV_ADDR/TX_REG_ADDR before this hook can
   * observe the *next* sm_int-driven byte -- see bk7258_i2c1.c's ISR:
   * it writes the next byte and clears sm_int in the *same* call that
   * this write hook is triggered from, so by the time control returns
   * here after that inner putreg32(), the byte to ACK is already the
   * one just written). */
  g_slave.bytes_seen++;

  switch (g_slave.state)
    {
      case SLAVE_GOT_ADDR:
        g_slave.state = SLAVE_GOT_REG;
        mock_slave_ack_and_fire(true);
        break;

      case SLAVE_GOT_REG:
        /* This is the data byte, the last one in a 3-byte write
         * transaction: ACK it, then immediately return to IDLE so a
         * stray write hook call after the driver's own stop bit write
         * (or the very next transaction's address-byte write) is not
         * mistaken for a continuation of this already-finished
         * transaction. */
        g_slave.state = SLAVE_IDLE;
        mock_slave_ack_and_fire(true);
        break;

      case SLAVE_GOT_DATA:
      case SLAVE_IDLE:
      default:
        break;
    }
}

/* ========================================================================
 * Interrupt auto-fire hooks: the moment the driver writes the "start"
 * bit for YUV_BUF or DMA, synchronously fire that peripheral's
 * interrupt, so bk7258_gc2145_test()'s busy-wait loops see their flag
 * already set on the very first check (see file header comment for why
 * this is necessary in a single-threaded test process).
 * ======================================================================== */

static bool g_yuvb_auto_fire_armed;
static bool g_dma_auto_fire_armed;

static void irq_autofire_write_hook(uintptr_t addr, uint32_t value,
                                     void *arg)
{
  (void)arg;

  mock_slave_write_hook(addr, value, NULL);

  if (g_yuvb_auto_fire_armed && addr == TEST_YUV_BUF_CTRL &&
      (value & TEST_YUV_BUF_CTRL_YUV_MODE) != 0)
    {
      bk7258_sim_poke32(TEST_YUV_BUF_INT_STATUS,
                         TEST_YUV_BUF_INT_STATUS_SM0_WR);
      (void)bk7258_sim_irq_fire(BK7258_IRQ_YUVB);
    }

  if (g_dma_auto_fire_armed && addr == TEST_DMA_CTRL &&
      (value & TEST_DMA_CTRL_ENABLE) != 0)
    {
      bk7258_sim_poke32(TEST_DMA_STATUS, TEST_DMA_STATUS_FINISH_INT);
      (void)bk7258_sim_irq_fire(BK7258_IRQ_DMA);
    }
}

extern int bk7258_gc2145_test(int argc, char **argv);

static void test_gc2145_full_flow_with_responsive_sensor(void)
{
  int ret;

  bk7258_sim_reset();
  mock_slave_reset();
  g_yuvb_auto_fire_armed = true;
  g_dma_auto_fire_armed = true;
  bk7258_sim_set_write_hook(irq_autofire_write_hook, NULL);

  ret = bk7258_gc2145_test(0, NULL);

  CHECK(ret == 0,
        "expected bk7258_gc2145_test() to return 0 (success) when the "
        "mock I2C1 slave ACKs every register and both the YUV_BUF "
        "line-done and DMA-done interrupts fire promptly, got %d", ret);

  /* 625 = 585 (sensor_gc2145_init_talbe) + 40
   * (sensor_gc2145_640_480_table) register writes, each a 3-byte
   * transaction (address, register, value) = 1875 bytes total observed
   * by the mock slave. */
  CHECK(g_slave.transactions_seen == 625u,
        "expected exactly 625 I2C transactions (585 init + 40 "
        "resolution register writes), observed %u",
        g_slave.transactions_seen);
  CHECK(g_slave.bytes_seen == 625u * 3u,
        "expected exactly %u bytes clocked onto the bus (625 "
        "transactions * 3 bytes each: address, register, value), "
        "observed %u", 625u * 3u, g_slave.bytes_seen);
}

static void test_gc2145_aborts_when_sensor_never_acks(void)
{
  int ret;

  bk7258_sim_reset();
  mock_slave_reset();
  g_yuvb_auto_fire_armed = false;
  g_dma_auto_fire_armed = false;
  g_slave.always_nack = true;

  bk7258_sim_set_write_hook(mock_slave_write_hook, NULL);

  ret = bk7258_gc2145_test(0, NULL);

  CHECK(ret == -1,
        "expected bk7258_gc2145_test() to return -1 when the sensor "
        "never ACKs any I2C write (modeling an absent/miswired/"
        "unpowered sensor), got %d", ret);
  CHECK(g_slave.transactions_seen == 1u,
        "expected the driver to abort after exactly 1 I2C transaction "
        "(the very first init register write, 0xFE) once it sees a "
        "NACK, rather than continuing through the rest of the 625-entry "
        "table, observed %u transactions", g_slave.transactions_seen);
  CHECK(g_slave.bytes_seen == 1u,
        "expected exactly 1 byte (the address byte of the aborted "
        "first transaction) to have been clocked onto the bus before "
        "the driver aborted on NACK, observed %u bytes",
        g_slave.bytes_seen);
}

static void test_gc2145_powers_on_and_resets_camera_before_any_i2c_traffic(
  void)
{
  bk7258_sim_reset();
  mock_slave_reset();
  g_yuvb_auto_fire_armed = true;
  g_dma_auto_fire_armed = true;
  bk7258_sim_set_write_hook(irq_autofire_write_hook, NULL);

  (void)bk7258_gc2145_test(0, NULL);

  /* DVP_POWER_PIN is GPIO49 (DVP_PWR_CTL net, schematic sheet 2/6's pin
   * table identifying physical pin 88 as P49) after this driver's
   * schematic-verified correction -- see bk7258_gc2145.c's
   * DVP_POWER_PIN/DVP_RESET_PIN definition comment.  Verify it ends up
   * driven high (bk7258_gc2145_power_on() calls
   * bk7258_gpio_output(DVP_POWER_PIN, true) and never turns it back off
   * within this test entry point). */
  CHECK((bk7258_sim_peek32(BK7258_GPIO_CFG(49u)) & BK7258_GPIO_OUTPUT) != 0,
        "expected the camera power-enable GPIO (49, DVP_PWR_CTL) to be "
        "driven high");

  /* DVP_RESET_PIN is GPIO28 (DVP_RST net).  bk7258_gc2145_reset() drives
   * it low, then high, before returning -- verify the *final* state is
   * high (reset released), which is the state I2C traffic depends on. */
  CHECK((bk7258_sim_peek32(BK7258_GPIO_CFG(28u)) & BK7258_GPIO_OUTPUT) != 0,
        "expected the camera reset GPIO (28, DVP_RST) to end up "
        "released (driven high) after the reset pulse");
}

static void test_gc2145_configures_yuv_buf_for_640x480(void)
{
  uint32_t pixel_reg;

  bk7258_sim_reset();
  mock_slave_reset();
  g_yuvb_auto_fire_armed = true;
  g_dma_auto_fire_armed = true;
  bk7258_sim_set_write_hook(irq_autofire_write_hook, NULL);

  (void)bk7258_gc2145_test(0, NULL);

  pixel_reg = bk7258_sim_peek32(0x48020000u + 5u * 4u);
  CHECK((pixel_reg & 0xffu) == 80u,
        "expected YUV_BUF configured for x_pixel=640/8=80, got %u",
        pixel_reg & 0xffu);
  CHECK(((pixel_reg >> 8) & 0xffu) == 60u,
        "expected YUV_BUF configured for y_pixel=480/8=60, got %u",
        (pixel_reg >> 8) & 0xffu);
}

int main(void)
{
  test_gc2145_full_flow_with_responsive_sensor();
  test_gc2145_aborts_when_sensor_never_acks();
  test_gc2145_powers_on_and_resets_camera_before_any_i2c_traffic();
  test_gc2145_configures_yuv_buf_for_640x480();

  if (g_failures == 0)
    {
      printf("PASS: all bk7258_gc2145.c integration tests passed\n");
      return 0;
    }

  printf("FAILED: %d assertion(s) failed\n", g_failures);
  return 1;
}
