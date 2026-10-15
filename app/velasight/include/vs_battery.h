/****************************************************************************
 * app/velasight/include/vs_battery.h
 *
 * Battery voltage and an estimate of the charge left, from the SARADC's
 * VBAT channel (/dev/adc0, board/beken/chips/bk7258/bk7258_saradc.c).
 *
 * The percentage is an estimate and says so.  It comes from a generic
 * single-cell Li-ion discharge table, not one measured for this cell, and
 * it reads the voltage under whatever load the device is putting on the
 * cell at that moment -- a running session sags it by tens of millivolts,
 * which this table reads as a few percent.  Good for "full", "half" and
 * "charge me soon"; not a fuel gauge.
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef __APP_VELASIGHT_INCLUDE_VS_BATTERY_H
#define __APP_VELASIGHT_INCLUDE_VS_BATTERY_H

#include <stdint.h>

struct vs_battery_s
{
  int32_t  raw;       /* the averaged SARADC code */
  uint16_t mv;        /* VBAT, millivolts */
  uint8_t  percent;   /* 0..100, estimated, see above */
};

/* One fresh conversion, about 5 ms.  Returns 0, or a negative errno:
 * -ENOENT without /dev/adc0, -EBUSY when the CP kept the converter for
 * longer than the driver waits, -ENOSYS when built without an ADC.
 */

int vs_battery_read(struct vs_battery_s *out);

/* The median of VS_BATTERY_SAMPLES conversions, which is what a gauge
 * should report.  A single conversion averages about a millisecond of a
 * supply that moves with the load: measured 2026-10-14, one in 300 caught
 * a dip of about 100 mV lasting half a millisecond, sample by sample, and
 * read 60 mV low.  The median ignores one such conversion.  Errors as
 * vs_battery_read(), when no conversion succeeded.
 */

#define VS_BATTERY_SAMPLES 3

int vs_battery_sample(struct vs_battery_s *out);

#endif /* __APP_VELASIGHT_INCLUDE_VS_BATTERY_H */
