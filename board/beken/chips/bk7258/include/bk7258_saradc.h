/****************************************************************************
 * board/beken/chips/bk7258/include/bk7258_saradc.h
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef __BOARD_BEKEN_CHIPS_BK7258_INCLUDE_BK7258_SARADC_H
#define __BOARD_BEKEN_CHIPS_BK7258_INCLUDE_BK7258_SARADC_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdint.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* The two internal channels, the only ones this driver accepts: neither
 * needs a pin, and every external channel on this board lands on a pad
 * that the keys, a panel, the camera or the software I2C already uses.
 */

#define BK7258_SARADC_CH_VBAT   0   /* the VBAT pin, through a 1/5 divider */
#define BK7258_SARADC_CH_VTEMP  7   /* die temperature, uncalibrated here */

/* A channel-0 code in millivolts at the VBAT pin.
 *
 * The vendor's own battery monitor's conversion, for the configuration this
 * driver samples with (bk_avdk_smp ap/components/bk_batt_monitor/
 * bat_monitor.c, iot_battery_voltage()).  Not trimmed per chip.  The CP's
 * default two-point calibration (saradc_val in its adc_driver.c, 1 V at
 * 0x9c7 and 2 V at 0x1358) gives a slope about 2% steeper -- 15 mV apart
 * at 3.8 V, well inside what a gauge built on either can claim.
 */

#define BK7258_SARADC_VBAT_MV(code) \
  ((int32_t)(((uint32_t)(code) * 667u) / 1000u) + 40)

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

struct adc_dev_s;

/****************************************************************************
 * Name: bk7258_saradc_initialize
 *
 * Description:
 *   Allocate the SARADC lower half for the listed channels, ready for
 *   adc_register().  Touches no hardware: the block belongs to the CP
 *   between conversions, see bk7258_saradc.c.
 *
 *   Each ANIOC_TRIGGER converts every listed channel once, in order, and
 *   delivers one averaged code per channel.
 *
 * Input Parameters:
 *   chanlist  - channels to convert, BK7258_SARADC_CH_*
 *   nchannels - entries in chanlist, 1 or 2
 *
 * Returned Value:
 *   The device, or NULL for an unsupported channel list or no memory.
 *
 ****************************************************************************/

struct adc_dev_s *bk7258_saradc_initialize(const uint8_t *chanlist,
                                           unsigned int nchannels);

#endif /* __BOARD_BEKEN_CHIPS_BK7258_INCLUDE_BK7258_SARADC_H */
