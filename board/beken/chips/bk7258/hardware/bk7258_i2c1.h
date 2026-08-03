/****************************************************************************
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef __VENDOR_BEKEN_CHIPS_BK7258_HARDWARE_BK7258_I2C1_H
#define __VENDOR_BEKEN_CHIPS_BK7258_HARDWARE_BK7258_I2C1_H

#include <stdint.h>

#include "bk7258_memorymap.h"

/* Register layout per bk_avdk_smp release/v3.1.1
 * ap/middleware/soc/bk7258_ap/soc/i2c_struct.h (i2c_typedef_t) and
 * ap/middleware/soc/bk7258_ap/soc/i2c_reg.h.  Each I2C unit (I2C0 at
 * SOC_I2C0_REG_BASE, I2C1 at SOC_I2C1_REG_BASE) has an identical 8-word
 * register block; this board only wires up I2C1 (BK7258_I2C1_BASE), so
 * only that block's offsets are defined here. */

#define BK7258_I2C1_DEV_ID          (BK7258_I2C1_BASE + 0x00u)
#define BK7258_I2C1_DEV_VERSION     (BK7258_I2C1_BASE + 0x04u)
#define BK7258_I2C1_GLOBAL_CTRL     (BK7258_I2C1_BASE + 0x08u)
#define BK7258_I2C1_DEV_STATUS      (BK7258_I2C1_BASE + 0x0cu)
#define BK7258_I2C1_SM_BUS_CFG      (BK7258_I2C1_BASE + 0x10u)
#define BK7258_I2C1_INT_STATUS      (BK7258_I2C1_BASE + 0x14u)
#define BK7258_I2C1_SM_BUS_DATA     (BK7258_I2C1_BASE + 0x18u)
#define BK7258_I2C1_SM_BUS_EX_CFG   (BK7258_I2C1_BASE + 0x1cu)

/* global_ctrl (REG_0x02) */
#define BK7258_I2C1_SOFT_RESET       (1u << 0)
#define BK7258_I2C1_CLK_GATE_BYPASS  (1u << 1)

/* sm_bus_cfg (REG_0x04) */
#define BK7258_I2C1_CFG_IDLE_CR_SHIFT     0
#define BK7258_I2C1_CFG_IDLE_CR_MASK      (0x7u << 0)
#define BK7258_I2C1_CFG_SCL_CR_SHIFT      3
#define BK7258_I2C1_CFG_SCL_CR_MASK       (0x7u << 3)
#define BK7258_I2C1_CFG_FREQ_DIV_SHIFT    6
#define BK7258_I2C1_CFG_FREQ_DIV_MASK     (0x3ffu << 6)
#define BK7258_I2C1_CFG_SLAVE_ADDR_SHIFT  16
#define BK7258_I2C1_CFG_SLAVE_ADDR_MASK   (0x3ffu << 16)
#define BK7258_I2C1_CFG_CLK_SRC_SHIFT     26
#define BK7258_I2C1_CFG_CLK_SRC_MASK      (0x3u << 26)
#define BK7258_I2C1_CFG_TIMEOUT_EN        (1u << 28)
#define BK7258_I2C1_CFG_IDLE_DET_EN       (1u << 29)
#define BK7258_I2C1_CFG_INH               (1u << 30)
#define BK7258_I2C1_CFG_EN                (1u << 31)

/* sm_bus_status (REG_0x05).  This register is both the master interrupt-
 * status register (read) and the command/ack register (write): writing
 * start/stop/ack bits issues the corresponding bus transition, per
 * bk_avdk_smp i2c_ll.h's i2c_ll_enable_start()/i2c_ll_tx_non_ack(), which
 * write these same bit positions in sm_bus_status. */
#define BK7258_I2C1_STATUS_SM_INT         (1u << 0)
#define BK7258_I2C1_STATUS_SCL_TIMEOUT    (1u << 1)
#define BK7258_I2C1_STATUS_ARB_LOST       (1u << 3)
#define BK7258_I2C1_STATUS_RX_FIFO_EMPTY  (1u << 4)
#define BK7258_I2C1_STATUS_TX_FIFO_FULL   (1u << 5)
#define BK7258_I2C1_STATUS_INT_MODE_SHIFT 6
#define BK7258_I2C1_STATUS_INT_MODE_MASK  (0x3u << 6)
#define BK7258_I2C1_STATUS_ACK            (1u << 8)
#define BK7258_I2C1_STATUS_STOP           (1u << 9)
#define BK7258_I2C1_STATUS_START          (1u << 10)
#define BK7258_I2C1_STATUS_ADDR_MATCH     (1u << 11)
#define BK7258_I2C1_STATUS_ACK_REQ        (1u << 12)
#define BK7258_I2C1_STATUS_TX_MODE        (1u << 13)
#define BK7258_I2C1_STATUS_MASTER         (1u << 14)
#define BK7258_I2C1_STATUS_BUSY           (1u << 15)

/* sm_bus_data (REG_0x06) */
#define BK7258_I2C1_DATA_MASK             0xffu

/* sm_bus_ex_cfg (REG_0x07) */
#define BK7258_I2C1_EX_DATA_H_OUTEN       (1u << 0)
#define BK7258_I2C1_EX_ADDR_H_OUTEN       (1u << 1)
#define BK7258_I2C1_EX_BYTE_INTERVAL_SHIFT 8
#define BK7258_I2C1_EX_BYTE_INTERVAL_MASK  (0xffu << 8)

/* FIFO interrupt-level encoding for sm_bus_status.int_mode (write side),
 * per bk_avdk_smp i2c_hal.c i2c_hal_set_write_int_mode()/
 * i2c_hal_set_read_int_mode(): the same 2-bit field means different FIFO
 * depths depending on tx vs rx direction. */
#define BK7258_I2C1_WRITE_INT_MODE_LEVEL_1  0u
#define BK7258_I2C1_WRITE_INT_MODE_LEVEL_4  1u

#endif /* __VENDOR_BEKEN_CHIPS_BK7258_HARDWARE_BK7258_I2C1_H */
