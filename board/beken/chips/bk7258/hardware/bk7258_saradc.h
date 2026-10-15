/****************************************************************************
 * board/beken/chips/bk7258/hardware/bk7258_saradc.h
 *
 * General-purpose SARADC register map, and the system-control and analog
 * fields that gate it.  Not the audio ADC: that is the AUD block at
 * 0x47800000 (hardware/bk7258_aud.h), an unrelated peripheral.
 *
 * Provenance, all bk_avdk_smp:
 *
 *   base      ap/include/soc/bk7258/reg_base.h SOC_SADC_REG_BASE; this AP
 *             builds the SPE variant, where SOC_ADDR_OFFSET is 0
 *   words     cp/middleware/soc/bk7258/soc/adc_struct.h (adc_hw_t)
 *   sysctrl   cp/middleware/soc/bk7258/soc/sys_struct.h: cksel_sadc is
 *             bit 17 of word 0x8, sadc_cken bit 5 of word 0xc,
 *             cpu0_sadc_int_en bit 8 of word 0x20 (sys_reg.h)
 *   analog    cp/middleware/soc/bk7258/hal/sys_ll.h ana_reg2 (word 0x42)
 *             and ana_reg5 (word 0x45), written through the analog SPI
 *             with the same busy poll hardware/bk7258_aud.h describes
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef __BOARD_BEKEN_CHIPS_BK7258_HARDWARE_BK7258_SARADC_H
#define __BOARD_BEKEN_CHIPS_BK7258_HARDWARE_BK7258_SARADC_H

#include "bk7258_memorymap.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define BK7258_SARADC_BASE             0x45890000u

#define BK7258_SARADC_DEV_ID           (BK7258_SARADC_BASE + 0x00u)
#define BK7258_SARADC_DEV_VERSION      (BK7258_SARADC_BASE + 0x04u)
#define BK7258_SARADC_GLOBAL_CTRL      (BK7258_SARADC_BASE + 0x08u)
#define BK7258_SARADC_CTRL             (BK7258_SARADC_BASE + 0x10u)
#define BK7258_SARADC_STEADY           (BK7258_SARADC_BASE + 0x18u)
#define BK7258_SARADC_SAT              (BK7258_SARADC_BASE + 0x1cu)
#define BK7258_SARADC_DATA             (BK7258_SARADC_BASE + 0x20u)

/* GLOBAL_CTRL */

#define BK7258_SARADC_SOFT_RESET       (1u << 0)

/* CTRL.  adc_clk = source / (2 * (div + 1)); in continuous mode one sample
 * every (16 + samp_rate) adc_clk, averaged over filter + 1 of them.
 */

#define BK7258_SARADC_MODE_MASK        (3u << 0)
#define BK7258_SARADC_MODE_CONTINUOUS  (3u << 0)
#define BK7258_SARADC_EN               (1u << 2)
#define BK7258_SARADC_CHAN_SHIFT       3
#define BK7258_SARADC_CHAN_MASK        (0xfu << 3)
#define BK7258_SARADC_SETTING_8        (1u << 7)   /* 8 cycles, not 4 */
#define BK7258_SARADC_INT_CLEAR        (1u << 8)
#define BK7258_SARADC_DIV_SHIFT        9
#define BK7258_SARADC_DIV_MASK         (0x3fu << 9)
#define BK7258_SARADC_32M              (1u << 15)
#define BK7258_SARADC_SAMP_MASK        (0x3fu << 16)
#define BK7258_SARADC_FILTER_MASK      (0x7fu << 22)
#define BK7258_SARADC_BUSY             (1u << 29)
#define BK7258_SARADC_FIFO_EMPTY       (1u << 30)
#define BK7258_SARADC_FIFO_FULL        (1u << 31)

/* STEADY.  fifo_level only sets when the FIFO interrupt fires; steady is
 * the power-on wait, (steady + 1) * 8 adc_clk.
 */

#define BK7258_SARADC_FIFO_LEVEL_MASK  (0x1fu << 0)
#define BK7258_SARADC_STEADY_SHIFT     5
#define BK7258_SARADC_STEADY_MASK      (7u << 5)
#define BK7258_SARADC_BYPASS_CALIB     (1u << 10)

/* SAT.  With the 12-bit core, sat_ctrl 3 selects result bits [15:0]. */

#define BK7258_SARADC_SAT_CTRL_MASK    (3u << 0)
#define BK7258_SARADC_SAT_ENABLE       (1u << 2)
#define BK7258_SARADC_OVERFLOW         (1u << 3)

#define BK7258_SARADC_DATA_MASK        0xffffu

/* System control.  BK7258_SYS_CLKDIV1 and BK7258_SYS_DEVCLK_EN are the
 * words in hardware/bk7258_sysctrl.h; the CPU0 enable word is the CP's.
 */

#define BK7258_SYS_CKSEL_SADC          (1u << 17)  /* 0: XTAL 26 MHz */
#define BK7258_SYS_SADC_CKEN           (1u << 5)
#define BK7258_SYS_CPU0_IRQ_EN0        (BK7258_SYSCTRL_BASE + 0x80u)
#define BK7258_SYS_CPU0_SADC_INT_EN    (1u << 8)

/* Analog registers.  Same transport as the AUD ones: a write is forwarded
 * over an internal SPI link and is complete only when its busy bit in the
 * state word clears; a read returns the shadow directly.
 */

#define BK7258_SARADC_ANA(n)           (BK7258_SYSCTRL_BASE + 0x100u + \
                                        (n) * 4u)
#define BK7258_SARADC_ANA_STATE        (BK7258_SYSCTRL_BASE + 0xe8u)
#define BK7258_SARADC_ANA_BUSY(n)      (1u << (n))

/* ana_reg2: the general-purpose ADC front end.  Bits [7:0] are the crystal
 * trim and [18:16] reference controls that this driver never touches.
 */

#define BK7258_ANA2                    2u
#define BK7258_ANA2_CMP_ICTRL(v)       ((uint32_t)(v) << 9)
#define BK7258_ANA2_INBUF_ICTRL(v)     ((uint32_t)(v) << 11)
#define BK7258_ANA2_REFBUF_ICTRL(v)    ((uint32_t)(v) << 13)
#define BK7258_ANA2_NOBUF_ENABLE       (1u << 15)
#define BK7258_ANA2_CAPCAL(v)          ((uint32_t)(v) << 19)
#define BK7258_ANA2_SP_NT_CTRL(v)      ((uint32_t)(v) << 25)

#define BK7258_ANA2_GADC_MASK          (BK7258_ANA2_CMP_ICTRL(3) | \
                                        BK7258_ANA2_INBUF_ICTRL(3) | \
                                        BK7258_ANA2_REFBUF_ICTRL(3) | \
                                        BK7258_ANA2_NOBUF_ENABLE | \
                                        BK7258_ANA2_CAPCAL(0x3f) | \
                                        BK7258_ANA2_SP_NT_CTRL(0x7f))

/* ana_reg5: en_temp powers the die temperature sensor (sys_hal_en_tempdet());
 * adc_div is the input divider, channel 0 reading 0 1/7, 1 1/5, 2 1/3,
 * 3 1/2 and the other channels 0 1/4, 1 1/3, 2 1/2, 3 1/1.
 */

#define BK7258_ANA5                    5u
#define BK7258_ANA5_EN_TEMP            (1u << 4)   /* die temp sensor */
#define BK7258_ANA5_ADC_DIV_SHIFT      10
#define BK7258_ANA5_ADC_DIV_MASK       (3u << 10)

#endif /* __BOARD_BEKEN_CHIPS_BK7258_HARDWARE_BK7258_SARADC_H */
