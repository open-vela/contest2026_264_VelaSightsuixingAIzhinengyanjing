/****************************************************************************
 * board/beken/chips/bk7258/bk7258_qspi.c
 *
 * BK7258 QSPI0 command-channel driver (no DMA, cmd_c/cmd_d indirect command
 * path only).  Register layout source: bk_avdk_smp release/v3.1.1
 * ap/middleware/soc/bk7258_ap/soc/qspi_struct.h and hal/qspi_ll.h.
 *
 * Register offsets (byte offset from BK7258_QSPI0_BASE, word index in
 * qspi_hw_t from qspi_struct.h in parentheses):
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
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#include <nuttx/config.h>
#include <stdint.h>

#include "arm_internal.h"
#include "hardware/bk7258_memorymap.h"
#include "bk7258_qspi.h"

#define QSPI_REG(offset)           (BK7258_QSPI0_BASE + (offset))

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

#define QSPI_CMD_START_BIT         (1u << 0)

static void bk7258_qspi0_wait_done(void)
{
  while ((getreg32(QSPI_STATUS) & QSPI_STATUS_CMD_START_DONE) == 0)
    {
    }
}

void bk7258_qspi0_init(void)
{
  putreg32(QSPI_GLB_CTRL_SOFT_RESET, QSPI_GLB_CTRL);
  up_udelay(10);
  putreg32(0, QSPI_GLB_CTRL);
}

void bk7258_qspi0_send_cmd(uint8_t cmd, const uint8_t *data, uint8_t data_len)
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
  bk7258_qspi0_wait_done();
}

uint32_t bk7258_qspi0_read_id(void)
{
  putreg32(0, QSPI_CMD_D_L);
  putreg32(0, QSPI_CMD_D_H);
  putreg32(0, QSPI_CMD_D_CFG1);
  putreg32(0, QSPI_CMD_D_CFG2);

  putreg32(0x04u, QSPI_CMD_D_H); /* GC9D01 read-ID opcode, single byte cmd */
  modifyreg32(QSPI_CMD_D_CFG2, 0, QSPI_CMD_START_BIT);
  bk7258_qspi0_wait_done();

  return getreg32(QSPI_CMD_D_L);
}
