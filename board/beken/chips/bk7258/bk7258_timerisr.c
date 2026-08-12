/****************************************************************************
 * vendor/beken/chips/bk7258/bk7258_timerisr.c
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

#include <nuttx/arch.h>
#include <nuttx/clock.h>
#include <nuttx/irq.h>

#include <arch/arm_m/nvicpri.h>

#include "arm_internal.h"
#include "nvic.h"
#include "irq.h"
#include "bk7258_boottrace.h"

#define BK7258_SYSTICK_RELOAD \
  ((CONFIG_BK7258_CPU_FREQ_HZ / TICK_PER_SEC) - 1u)

#if CONFIG_BK7258_CPU_FREQ_HZ % TICK_PER_SEC != 0
#  error "BK7258 CPU frequency must divide evenly into the system tick rate"
#endif

#if BK7258_SYSTICK_RELOAD > NVIC_MAX_SYSTICK_CNT
#  error "BK7258 SysTick reload exceeds the 24-bit counter"
#endif

static bool g_first_tick_complete;
static uint32_t g_tick_count;

static int bk7258_timerisr(int irq, void *context, void *arg)
{
  uint32_t tick = ++g_tick_count;
  bool first_tick = !g_first_tick_complete;

  (void)irq;
  (void)context;
  (void)arg;

  bk7258_boottrace_detail(3, (tick << 8) | 1u);

  bk7258_boottrace_detail(3, (tick << 8) | 2u);
  nxsched_process_timer();
  bk7258_boottrace_detail(3, (tick << 8) | 3u);

  if (first_tick)
    {
      g_first_tick_complete = true;
    }

  return OK;
}

int bk7258_timer_start(void)
{
  if (up_cpu_index() != 0)
    {
      return -EXDEV;
    }

  putreg32(0, NVIC_SYSTICK_CURRENT);
  bk7258_boottrace_primary(BK7258_BOOT_SYSTICK_STARTED);
  putreg32(NVIC_SYSTICK_CTRL_CLKSOURCE | NVIC_SYSTICK_CTRL_TICKINT |
           NVIC_SYSTICK_CTRL_ENABLE, NVIC_SYSTICK_CTRL);
  up_enable_irq(BK7258_IRQ_SYSTICK);
  return OK;
}

void up_timer_initialize(void)
{
  uint32_t priority;

  /* Match the vendor SMP port's configTICK_CORE contract: only logical CPU0
   * owns the global tick.  CPU1 leaves its private SysTick disabled.
   */

  DEBUGASSERT(up_cpu_index() == 0);

  priority = getreg32(NVIC_SYSH12_15_PRIORITY);
  priority &= ~NVIC_SYSH_PRIORITY_PR15_MASK;
  priority |= NVIC_SYSH_PRIORITY_DEFAULT <<
              NVIC_SYSH_PRIORITY_PR15_SHIFT;
  putreg32(priority, NVIC_SYSH12_15_PRIORITY);

  putreg32(0, NVIC_SYSTICK_CTRL);
  putreg32(BK7258_SYSTICK_RELOAD, NVIC_SYSTICK_RELOAD);
  putreg32(0, NVIC_SYSTICK_CURRENT);

  if (irq_attach(BK7258_IRQ_SYSTICK, bk7258_timerisr, NULL) < 0)
    {
      PANIC();
    }

  /* nx_start() initializes the clock before nx_smp_start().  Keep SysTick
   * stopped until up_cpu_start() has completed the secondary boot transaction.
   */
}
