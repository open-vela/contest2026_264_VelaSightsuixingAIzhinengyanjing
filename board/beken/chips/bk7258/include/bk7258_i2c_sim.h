/****************************************************************************
 * board/beken/chips/bk7258/include/bk7258_i2c_sim.h
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef __VENDOR_BEKEN_CHIPS_BK7258_INCLUDE_BK7258_I2C_SIM_H
#define __VENDOR_BEKEN_CHIPS_BK7258_INCLUDE_BK7258_I2C_SIM_H

#include <stdbool.h>
#include <stdint.h>

/* Software bit-banged I2C master on a fixed pair of GPIO pins (GPIO0=SCL,
 * GPIO1=SDA, matching bk_avdk_smp release/v3.1.1
 * projects/dvp_example/ap/config/bk7258_ap/config
 * CONFIG_SIM_I2C0_SCL_GPIO=0 / CONFIG_SIM_I2C0_SDA_GPIO=1).  Master-only,
 * no multi-master arbitration, no clock stretching support (matching the
 * reference sim_i2c_driver.c, which also does not support clock
 * stretching).
 *
 * dev_addr is the 7-bit I2C address (e.g. 0x3C for GC2145), NOT the
 * pre-shifted 8-bit write/read address (0x78/0x79) sometimes quoted in
 * sensor datasheets -- this driver performs the <<1 shift internally to
 * match sim_i2c_driver.c's I2cMemWrite()/I2cMemRead() convention.
 */

void bk7258_i2c_sim_init(void);
bool bk7258_i2c_sim_write_reg(uint8_t dev_addr, uint8_t reg_addr,
                               uint8_t value);
bool bk7258_i2c_sim_read_reg(uint8_t dev_addr, uint8_t reg_addr,
                              uint8_t *value);

#endif /* __VENDOR_BEKEN_CHIPS_BK7258_INCLUDE_BK7258_I2C_SIM_H */
