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
#include <stdint.h>

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

/* Matches qspi_ll_wait_cmd_done()'s 10000-iteration bound
 * (ap/middleware/soc/bk7258_ap/hal/qspi_ll.h) instead of looping forever,
 * so a pinmux or wiring problem produces a diagnosable timeout rather than
 * an unrecoverable hang. */
#define QSPI_WAIT_DONE_MAX_ITER    10000

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
}

bool bk7258_qspi0_send_cmd(uint8_t cmd, const uint8_t *data, uint8_t data_len)
{
  uint32_t cmd_c_h = (uint32_t)cmd;
  uint32_t cmd_c_l = 0;
  uint8_t i;

  putreg32(0, QSPI_CMD_C_L);
  putreg32(0, QSPI_CMD_C_H);
  putreg32(0, QSPI_CMD_C_CFG1);
  putreg32(0, QSPI_CMD_C_CFG2);

  if (data_len > 0 && data_len <= 4)
    {
      for (i = 0; i < data_len; i++)
        {
          cmd_c_l |= ((uint32_t)data[i]) << (i * 8);
        }

      putreg32(cmd_c_l, QSPI_CMD_C_L);
    }

  putreg32(cmd_c_h, QSPI_CMD_C_H);
  modifyreg32(QSPI_CMD_C_CFG2, 0, QSPI_CMD_START_BIT);
  return bk7258_qspi0_wait_done();
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
