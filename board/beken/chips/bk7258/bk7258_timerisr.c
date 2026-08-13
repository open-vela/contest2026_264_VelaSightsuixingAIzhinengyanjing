/****************************************************************************
 * vendor/beken/chips/bk7258/bk7258_timerisr.c
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#include <nuttx/config.h>

#include <nuttx/timers/arch_timer.h>

#include "systick.h"

void up_timer_initialize(void)
{
  up_timer_set_lowerhalf(
    systick_initialize(true, CONFIG_BK7258_CPU_FREQ_HZ, -1));
}
