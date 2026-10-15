/****************************************************************************
 * app/velasight/vs_battery.c
 *
 * See include/vs_battery.h.  The conversion from code to millivolts is the
 * chip's (BK7258_SARADC_VBAT_MV(): its divider, its reference); the curve
 * from millivolts to percent is the cell's, and lives here.
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <unistd.h>

#ifdef CONFIG_BK7258_SARADC
#  include <nuttx/analog/adc.h>
#  include <nuttx/analog/ioctl.h>
#  include <arch/chip/bk7258_saradc.h>
#endif

#include "include/vs_battery.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define VS_BATTERY_DEV "/dev/adc0"

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct vs_battery_point_s
{
  uint16_t mv;
  uint8_t  percent;
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

#ifdef CONFIG_BK7258_SARADC

/* Resting voltage against charge left for a generic LiCoO2/NMC cell, the
 * shape of the commonly published curves rather than a measurement of this
 * one.  Flat between 3.75 and 3.85 V, which is where an estimate is least
 * certain; steep below 3.7 V, which is where it matters most.  100% at
 * 4.15 V because chargers commonly terminate a little short of 4.20.
 */

static const struct vs_battery_point_s g_battery_curve[] =
{
  { 3300,   0 },
  { 3450,   5 },
  { 3680,  10 },
  { 3740,  20 },
  { 3770,  30 },
  { 3790,  40 },
  { 3820,  50 },
  { 3870,  60 },
  { 3920,  70 },
  { 3980,  80 },
  { 4060,  90 },
  { 4150, 100 },
};

#endif

/****************************************************************************
 * Private Functions
 ****************************************************************************/

#ifdef CONFIG_BK7258_SARADC

/* Linear between the points of the curve, clamped at both ends. */

static uint8_t vs_battery_percent(int32_t mv)
{
  const size_t count = sizeof(g_battery_curve) / sizeof(g_battery_curve[0]);
  size_t i;

  if (mv <= g_battery_curve[0].mv)
    {
      return g_battery_curve[0].percent;
    }

  for (i = 1; i < count; i++)
    {
      const struct vs_battery_point_s *lo = &g_battery_curve[i - 1];
      const struct vs_battery_point_s *hi = &g_battery_curve[i];

      if (mv < hi->mv)
        {
          return (uint8_t)(lo->percent +
                           ((mv - lo->mv) * (hi->percent - lo->percent)) /
                           (hi->mv - lo->mv));
        }
    }

  return g_battery_curve[count - 1].percent;
}

#endif

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int vs_battery_read(struct vs_battery_s *out)
{
#ifdef CONFIG_BK7258_SARADC
  struct adc_msg_s msg;
  ssize_t n;
  int32_t mv;
  int ret = 0;
  int fd;

  if (out == NULL)
    {
      return -EINVAL;
    }

  /* Non-blocking, so a read can never wait for a conversion that the
   * trigger did not queue: the driver queues one only when it succeeds.
   */

  fd = open(VS_BATTERY_DEV, O_RDONLY | O_NONBLOCK);
  if (fd < 0)
    {
      return -errno;
    }

  if (ioctl(fd, ANIOC_TRIGGER, 0) < 0)
    {
      ret = -errno;
    }
  else
    {
      n = read(fd, &msg, sizeof(msg));
      if (n < 0)
        {
          ret = -errno;
        }
      else if (n != (ssize_t)sizeof(msg) ||
               msg.am_channel != BK7258_SARADC_CH_VBAT)
        {
          ret = -EIO;
        }
    }

  close(fd);

  if (ret < 0)
    {
      return ret;
    }

  mv = BK7258_SARADC_VBAT_MV(msg.am_data);
  out->raw     = msg.am_data;
  out->mv      = (uint16_t)(mv < 0 ? 0 : mv > UINT16_MAX ? UINT16_MAX : mv);
  out->percent = vs_battery_percent(mv);
  return 0;
#else
  (void)out;
  return -ENOSYS;
#endif
}

int vs_battery_sample(struct vs_battery_s *out)
{
  struct vs_battery_s got[VS_BATTERY_SAMPLES];
  struct vs_battery_s swap;
  unsigned int n = 0;
  unsigned int i;
  unsigned int j;
  int ret = -EIO;

  if (out == NULL)
    {
      return -EINVAL;
    }

  for (i = 0; i < VS_BATTERY_SAMPLES; i++)
    {
      int r = vs_battery_read(&got[n]);

      if (r == 0)
        {
          n++;
        }
      else
        {
          ret = r;
        }
    }

  if (n == 0)
    {
      return ret;
    }

  /* Sorted by voltage; with an even count the lower middle is reported. */

  for (i = 1; i < n; i++)
    {
      for (j = i; j > 0 && got[j].mv < got[j - 1].mv; j--)
        {
          swap       = got[j];
          got[j]     = got[j - 1];
          got[j - 1] = swap;
        }
    }

  *out = got[(n - 1) / 2];
  return 0;
}
