/****************************************************************************
 * BK7258 P0 idle implementation.
 ****************************************************************************/

#include <nuttx/config.h>

#include "arm_internal.h"
#include "nvic.h"
#include "bk7258_boottrace.h"

void up_idle(void)
{
#ifdef CONFIG_SMP
  if (up_cpu_index() == 1 &&
      bk7258_boottrace_secondary_stage() == BK7258_BOOT_SECONDARY_WAIT)
    {
      bk7258_boottrace_secondary(BK7258_BOOT_SECONDARY_RUNNING);
    }
#endif

  modifyreg32(NVIC_SYSCON, NVIC_SYSCON_SLEEPDEEP, 0);
  __asm__ volatile("dsb sy\n"
                   "wfi\n"
                   "isb sy\n" ::: "memory");
}
