/****************************************************************************
 * board/beken/chips/bk7258/bk7258_qspi.c
 *
 * BK7258 QSPI1 command-channel driver (no DMA, cmd_c/cmd_d indirect command
 * path only).  Register layout source: bk_avdk_smp release/v3.1.1
 * ap/middleware/soc/bk7258_ap/soc/qspi_struct.h and hal/qspi_ll.h.  QSPI0
 * and QSPI1 are two identical controller instances differing only in base
 * address and which fixed GPIO group is wired to their CLK/CSN/IO0-3
 * signal lines.
 *
 * Corrected from QSPI0 to QSPI1 (2026-07-31): this driver originally
 * targeted QSPI0 (base 0x46040000, GPIO22/23/24/25) because that is the
 * pin group used by bk_avdk_smp's generic
 * projects/spi_lcd_example/qspi_lcd_example reference configs. Those
 * reference projects' own hardware wires their LCD to that pin group, but
 * this board's actual schematic
 * (AIDK_AI玩具开发板_原理图.pdf sheet 2/6 main chip pin table + sheet 5/6
 * CN5 single-screen connector) shows the panel's LCD_QSPI_CLK/CS/D0/D1 net
 * labels are wired to chip pins 35/34/33/32 = P2/P3/P4/P5 =
 * GPIO2/GPIO3/GPIO4/GPIO5, which per the chip's pinmux table
 * (bk_avdk_smp/ap/middleware/soc/bk7258_ap/soc/gpio_map.h GPIO_DEV_MAP)
 * carry QSPI1_CLK/QSPI1_CSN/QSPI1_IO0/QSPI1_IO1 (function-select index 6),
 * not QSPI0.  Symptom that led to this fix: with the QSPI0 assumption,
 * bk7258_qspi0_send_cmd() always timed out on the very first init command
 * (0xFE) -- GPIO22-25 were pinmuxed to QSPI0, but the panel is not
 * physically wired to those pins, so cmd_start_done never asserts and the
 * 10000-iteration wait always exhausts.  See
 * hardware/bk7258_memorymap.h's BK7258_QSPI1_BASE comment for the full
 * schematic evidence chain (chip pin numbers, net names, gpio_map.h
 * excerpt).
 *
 * Note the schematic's literal net names ("LCD_QSPI_D0" etc.) do NOT
 * match the SoC's real hardware role for these pins (CLK/CSN, not data
 * lines) -- PCB net labels are the board designer's arbitrary
 * connector-pin numbering convention, not a statement about which SoC
 * peripheral function is actually multiplexed onto that pad.  The real
 * role is only knowable from gpio_map.h's per-pin function array, which is
 * what this driver's pin/function assignments below are built from.
 *
 * Register offsets (byte offset from BK7258_QSPI1_BASE, word index in
 * qspi_hw_t from qspi_struct.h in parentheses) -- identical layout to
 * QSPI0, only the base address differs:
 *   glb_ctrl    0x08 (REG_0x02) - bit[0] soft_reset, bit[1] bps_clkgate
 *   cmd_c_l     0x40 (REG_0x10)
 *   cmd_c_h     0x44 (REG_0x11)
 *   cmd_c_cfg1  0x48 (REG_0x12)
 *   cmd_c_cfg2  0x4c (REG_0x13) - bit[0] cmd_start
 *   cmd_d_l     0x50 (REG_0x14)
 *   cmd_d_h     0x54 (REG_0x15)
 *   cmd_d_cfg1  0x58 (REG_0x16)
 *   cmd_d_cfg2  0x5c (REG_0x17) - bit[0] cmd_start
 *   status      0x70 (REG_0x1c) - bit[2] cmd_start_done
 *   status_clr  0x6c (REG_0x1b) - bit[2] clr_cmd_start_done
 *
 * The "command done" flag is carried by the status register's
 * cmd_start_done bit, not by core_status (REG_0x03, byte offset 0x0c),
 * which is an unused, un-bitfielded raw word in the vendor headers and is
 * never read by qspi_ll_is_cmd_start_done()/qspi_ll_wait_cmd_done().  The
 * start trigger is not a separate register either: it is the cmd_start bit
 * inside each channel's own cfg2 register (cmd_c_cfg2 for the send path,
 * cmd_d_cfg2 for the read path), matching qspi_ll_cmd_c_start() and
 * qspi_ll_cmd_d_start().
 *
 * Pinmux: the QSPI1 controller's four signal lines (CLK/CSN/IO0/IO1) are
 * shared, multiplexed GPIO pads (GPIO_2/3/4/5 on this SoC), not dedicated
 * pins.  Each pad defaults to plain GPIO after reset and must be switched
 * to QSPI1 mode via the "second function" pinmux mechanism before the
 * controller's TX/RX lines are actually connected to the pad -- without
 * this step cmd_start_done never asserts because the panel never receives
 * the command.  Source of the function-select index (6) for QSPI1 on
 * GPIO_2/3/4/5: bk_avdk_smp release/v3.1.1
 * ap/middleware/soc/bk7258_ap/soc/gpio_map.h gpio_map_table (QSPI1_CLK/
 * CSN/IO0/IO1 are the 7th entry, index 6, in each pin's per-function
 * array).  The pinmux register plumbing itself (BK7258_GPIO_SYS_BASE etc.)
 * is shared with bk7258_gpio.c; see bk7258_gpio_set_function().
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#include <nuttx/config.h>
#include <nuttx/arch.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "arm_internal.h"
#include "hardware/bk7258_memorymap.h"
#include "hardware/bk7258_sysctrl.h"
#include "bk7258_gpio.h"
#include "bk7258_qspi.h"

#define QSPI1_CLK_PIN               2u
#define QSPI1_CSN_PIN               3u
#define QSPI1_IO0_PIN               4u
#define QSPI1_IO1_PIN               5u
#define QSPI1_PINMUX_FUNCTION       6u

#define QSPI_REG(offset)           (BK7258_QSPI1_BASE + (offset))

#define QSPI_GLB_CTRL              QSPI_REG(0x08u)
#define QSPI_GLB_CTRL_SOFT_RESET   (1u << 0)

#define QSPI_CMD_C_L               QSPI_REG(0x40u)
#define QSPI_CMD_C_H               QSPI_REG(0x44u)
#define QSPI_CMD_C_CFG1            QSPI_REG(0x48u)
#define QSPI_CMD_C_CFG2            QSPI_REG(0x4cu)

#define QSPI_CMD_D_L               QSPI_REG(0x50u)
#define QSPI_CMD_D_H               QSPI_REG(0x54u)
#define QSPI_CMD_D_CFG1            QSPI_REG(0x58u)
#define QSPI_CMD_D_CFG2            QSPI_REG(0x5cu)

#define QSPI_STATUS                QSPI_REG(0x70u)
#define QSPI_STATUS_CMD_START_DONE (1u << 2)

#define QSPI_STATUS_CLR                 QSPI_REG(0x6cu)
#define QSPI_STATUS_CLR_CMD_START_DONE  (1u << 2)

#define QSPI_CMD_START_BIT         (1u << 0)

/* config, REG_0x18 (qspi_struct.h) */

#define QSPI_CONFIG                     QSPI_REG(0x60u)
#define QSPI_CONFIG_QSPI_EN             (1u << 0)
#define QSPI_CONFIG_FORCE_SPI_CS_LOW    (1u << 6)
#define QSPI_CONFIG_CLK_RATE_SHIFT      8u
#define QSPI_CONFIG_CLK_RATE_MASK       (0xffu << QSPI_CONFIG_CLK_RATE_SHIFT)
#define QSPI_CONFIG_DISABLE_CMD_SCK     (1u << 16)
#define QSPI_CONFIG_IO_CPU_MEM_SEL      (1u << 22)

/* Memory-mapped data window of the QSPI1 controller
 * (LCD_QSPI1_DATA_ADDR, ap/include/driver/lcd_qspi_types.h).  With
 * config.io_cpu_mem_sel set, every write into this window is streamed out
 * of the QSPI data lines, which is how a whole frame is pushed to the
 * panel without touching the command FIFO for each byte.
 */

#define QSPI1_DATA_WINDOW               0x68000000u

/* GC9-series QSPI command framing: a transfer always starts with a 4-byte
 * header {write_cmd, 0x00, register, 0x00} placed in cmd_c_h's
 * cmd1..cmd4 (bk_lcd_qspi_send_cmd(), lcd_qspi_driver.c), where write_cmd
 * is 0x02 for a register write and 0x32 for a quad pixel write
 * (gc9c01_cmd[] = {0x32, 0x00, 0x2c, 0x00}).  cmd_c_cfg1 marks where the
 * header ends by writing the wire-mode value 3 into the cmdN_line field
 * of the first byte position that is NOT part of the header.
 */

#define GC9_REG_WRITE_CMD               0x02u
#define GC9_PIXEL_WRITE_CMD             0x02u
#define GC9_RAMWR                       0x2Cu
#define QSPI_CMD_LINE_MARK              0x3u

/* Panel clock.  GC9D01's reference config asks for 60MHz
 * (lcd_spi_gc9d01_config.clk = LCD_QSPI_60M); clk_rate is a divider of the
 * 480MHz QSPI source clock, so 8 gives 60MHz.
 */

#define QSPI_CLK_RATE_60M               8u

/* Matches qspi_ll_wait_cmd_done()'s 10000-iteration bound
 * (ap/middleware/soc/bk7258_ap/hal/qspi_ll.h) instead of looping forever,
 * so a pinmux or wiring problem produces a diagnosable timeout rather than
 * an unrecoverable hang. */
#define QSPI_WAIT_DONE_MAX_ITER    10000

static bool g_qspi_cmd_traced;
static uint32_t g_qspi_frames;

static bool bk7258_qspi0_wait_done(void)
{
  int i;

  for (i = 0; i < QSPI_WAIT_DONE_MAX_ITER; i++)
    {
      if ((getreg32(QSPI_STATUS) & QSPI_STATUS_CMD_START_DONE) != 0)
        {
          break;
        }

      up_udelay(1);
    }

  /* Clear cmd_start_done (write 1 then 0), matching qspi_ll_wait_cmd_done()
   * -- required so a stale "done" flag from this command does not make
   * the next command's wait loop return immediately without having
   * actually completed. */
  modifyreg32(QSPI_STATUS_CLR, 0, QSPI_STATUS_CLR_CMD_START_DONE);
  modifyreg32(QSPI_STATUS_CLR, QSPI_STATUS_CLR_CMD_START_DONE, 0);

  return i < QSPI_WAIT_DONE_MAX_ITER;
}

void bk7258_qspi0_init(void)
{
  /* Enable the QSPI1 *module* clock gate first, matching
   * qspi_id_init_common()'s documented call order ("1. set clock, 2. set
   * gpio as qspi, 3. enable interrupt") in bk_avdk_smp release/v3.1.1
   * ap/middleware/driver/qspi/qspi_driver.c.  Without this bit, register
   * writes to the QSPI1 block are accepted (no bus fault) but the
   * controller's internal command state machine has no clock to run on,
   * so cmd_start_done never asserts -- see
   * hardware/bk7258_sysctrl.h's BK7258_QSPI1_MODULE_CLK_EN comment for
   * the register-map citation and the I2C1 precedent this is modeled
   * after. */
  modifyreg32(BK7258_SYS_DEVCLK_EN, 0, BK7258_QSPI1_MODULE_CLK_EN);

  /* Switch the 4 shared GPIO pads to QSPI1 mode before touching the
   * controller; see file header comment for why this is required and why
   * it is QSPI1 (GPIO2-5) rather than QSPI0 (GPIO22-25) on this board. */
  bk7258_gpio_set_function(QSPI1_CLK_PIN, QSPI1_PINMUX_FUNCTION);
  bk7258_gpio_set_function(QSPI1_CSN_PIN, QSPI1_PINMUX_FUNCTION);
  bk7258_gpio_set_function(QSPI1_IO0_PIN, QSPI1_PINMUX_FUNCTION);
  bk7258_gpio_set_function(QSPI1_IO1_PIN, QSPI1_PINMUX_FUNCTION);

  putreg32(QSPI_GLB_CTRL_SOFT_RESET, QSPI_GLB_CTRL);
  up_udelay(10);
  putreg32(0, QSPI_GLB_CTRL);

  /* Clock rate and controller enable.  These were never programmed before:
   * the controller ran at whatever clk_rate reset left behind, and
   * qspi_en was never set, which is one reason the panel never showed
   * anything even though every command "completed".
   */

  modifyreg32(QSPI_CONFIG, QSPI_CONFIG_CLK_RATE_MASK,
              (QSPI_CLK_RATE_60M << QSPI_CONFIG_CLK_RATE_SHIFT) |
              QSPI_CONFIG_QSPI_EN);

  printf("bk7258_qspi1: init: devclk_en=0x%08x config=0x%08x "
         "glb_ctrl=0x%08x (clk_rate=%u qspi_en=%u)\n",
         (unsigned int)getreg32(BK7258_SYS_DEVCLK_EN),
         (unsigned int)getreg32(QSPI_CONFIG),
         (unsigned int)getreg32(QSPI_GLB_CTRL),
         (unsigned int)((getreg32(QSPI_CONFIG) &
                         QSPI_CONFIG_CLK_RATE_MASK) >>
                        QSPI_CONFIG_CLK_RATE_SHIFT),
         (unsigned int)((getreg32(QSPI_CONFIG) &
                         QSPI_CONFIG_QSPI_EN) != 0));
}

bool bk7258_qspi0_send_cmd(uint8_t cmd, const uint8_t *data, uint8_t data_len)
{
  uint32_t cmd_c_l = 0;
  uint8_t i;

  /* CORRECTION (2026-08-10): this used to put the bare register number in
   * cmd_c_h and leave cmd_c_cfg1/cfg2 at zero.  That is not the framing
   * GC9-series panels expect -- bk_lcd_qspi_send_cmd() builds a 4-byte
   * header {0x02, 0x00, reg, 0x00} and then marks the end of the header in
   * cmd_c_cfg1.  With the old code every command "completed" (the
   * controller happily clocked out a malformed frame and asserted
   * cmd_start_done), which is why bk7258_gc9d01_test() reported success
   * while the panel stayed dark: the transfers were well-formed QSPI
   * bursts carrying the wrong bytes.
   */

  putreg32(0, QSPI_CMD_C_L);
  putreg32(0, QSPI_CMD_C_H);
  putreg32(0, QSPI_CMD_C_CFG1);
  putreg32(0, QSPI_CMD_C_CFG2);

  if (data_len > 4)
    {
      return false;    /* long payloads would need the FIFO path */
    }

  for (i = 0; i < data_len; i++)
    {
      cmd_c_l |= ((uint32_t)data[i]) << (i * 8);
    }

  putreg32(cmd_c_l, QSPI_CMD_C_L);
  putreg32((((uint32_t)cmd) << 16) | GC9_REG_WRITE_CMD, QSPI_CMD_C_H);

  /* Header is 4 bytes (write_cmd, 0x00, reg, 0x00) plus data_len payload
   * bytes; mark the first byte slot after them, exactly as
   * "0x3 << ((data_len + 4) * 2)" does in the reference.
   */

  putreg32(QSPI_CMD_LINE_MARK << ((data_len + 4u) * 2u), QSPI_CMD_C_CFG1);

  if (!g_qspi_cmd_traced)
    {
      /* One-shot trace of the very first command: the whole GC9 framing fix
       * lives in these four registers, so their real read-back values are
       * the evidence that the header is being built as intended.
       */

      g_qspi_cmd_traced = true;
      printf("bk7258_qspi1: first cmd 0x%02x: cmd_c_h=0x%08x "
             "cmd_c_l=0x%08x cfg1=0x%08x cfg2=0x%08x data_len=%u\n",
             cmd, (unsigned int)getreg32(QSPI_CMD_C_H),
             (unsigned int)getreg32(QSPI_CMD_C_L),
             (unsigned int)getreg32(QSPI_CMD_C_CFG1),
             (unsigned int)getreg32(QSPI_CMD_C_CFG2),
             (unsigned int)data_len);
    }

  modifyreg32(QSPI_CMD_C_CFG2, 0, QSPI_CMD_START_BIT);

  if (!bk7258_qspi0_wait_done())
    {
      printf("bk7258_qspi1: cmd 0x%02x TIMED OUT, status=0x%08x "
             "cfg2=0x%08x\n", cmd,
             (unsigned int)getreg32(QSPI_STATUS),
             (unsigned int)getreg32(QSPI_CMD_C_CFG2));
      return false;
    }

  return true;
}

bool bk7258_qspi1_lcd_write_frame(FAR const void *frame, size_t len)
{
  FAR const uint32_t *src = frame;
  FAR volatile uint32_t *win = (FAR volatile uint32_t *)QSPI1_DATA_WINDOW;
  size_t words = len / 4u;
  size_t i;

  if (frame == NULL || len == 0 || (len & 3u) != 0)
    {
      return false;
    }

  /* Open a pixel-write burst: hold CS low across the whole frame, send the
   * {0x02, 0x00, RAMWR, 0x00} header through the command channel, then hand
   * the data lines over to the memory-mapped window
   * (bk_lcd_qspi_quad_write_start(), lcd_qspi_driver.c).
   */

  modifyreg32(QSPI_CONFIG, 0, QSPI_CONFIG_FORCE_SPI_CS_LOW);

  putreg32(0, QSPI_CMD_C_L);
  putreg32((((uint32_t)GC9_RAMWR) << 16) | GC9_PIXEL_WRITE_CMD,
           QSPI_CMD_C_H);
  putreg32(QSPI_CMD_LINE_MARK << (4u * 2u), QSPI_CMD_C_CFG1);
  putreg32(0, QSPI_CMD_C_CFG2);
  modifyreg32(QSPI_CMD_C_CFG2, 0, QSPI_CMD_START_BIT);

  if (!bk7258_qspi0_wait_done())
    {
      modifyreg32(QSPI_CONFIG, QSPI_CONFIG_FORCE_SPI_CS_LOW, 0);
      return false;
    }

  modifyreg32(QSPI_CONFIG, 0,
              QSPI_CONFIG_IO_CPU_MEM_SEL | QSPI_CONFIG_DISABLE_CMD_SCK);

  if (g_qspi_frames < 2)
    {
      printf("bk7258_qspi1: frame %u: config=0x%08x words=%u src=%p "
             "first_px=0x%04x\n", (unsigned int)g_qspi_frames,
             (unsigned int)getreg32(QSPI_CONFIG), (unsigned int)words,
             frame, (unsigned int)(src[0] & 0xffff));
    }

  /* CPU-driven copy.  160x160 RGB565 is 51200 bytes, which at 60MHz is
   * about 7ms even single-wire, so DMA is not needed to reach the frame
   * rates this panel is used at; it can be added later without changing
   * this function's contract.
   */

  for (i = 0; i < words; i++)
    {
      *win = src[i];
    }

  modifyreg32(QSPI_CONFIG,
              QSPI_CONFIG_IO_CPU_MEM_SEL | QSPI_CONFIG_DISABLE_CMD_SCK |
              QSPI_CONFIG_FORCE_SPI_CS_LOW, 0);

  g_qspi_frames++;
  return true;
}

uint32_t bk7258_qspi1_lcd_frame_count(void)
{
  return g_qspi_frames;
}

uint32_t bk7258_qspi0_read_id(void)
{
  putreg32(0, QSPI_CMD_D_L);
  putreg32(0, QSPI_CMD_D_H);
  putreg32(0, QSPI_CMD_D_CFG1);
  putreg32(0, QSPI_CMD_D_CFG2);

  putreg32(0x04u, QSPI_CMD_D_H); /* GC9D01 read-ID opcode, single byte cmd */
  modifyreg32(QSPI_CMD_D_CFG2, 0, QSPI_CMD_START_BIT);
  (void)bk7258_qspi0_wait_done();

  return getreg32(QSPI_CMD_D_L);
}
