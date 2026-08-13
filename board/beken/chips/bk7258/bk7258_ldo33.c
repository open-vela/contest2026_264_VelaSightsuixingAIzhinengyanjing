/****************************************************************************
 * board/beken/chips/bk7258/bk7258_ldo33.c
 *
 * Reference-counted control of the shared LDO33_EN rail (GPIO52).  See
 * include/bk7258_ldo33.h for the schematic evidence that this rail is
 * board-wide rather than motor-specific.
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdbool.h>
#include <stdint.h>

#include <nuttx/irq.h>
#include <nuttx/spinlock.h>

#include "bk7258_gpio.h"
#include "bk7258_ldo33.h"

/****************************************************************************
 * Private Data
 ****************************************************************************/

static unsigned int g_ldo33_refs;
static bool g_ldo33_configured;

/****************************************************************************
 * Public Functions
 ****************************************************************************/

unsigned int bk7258_ldo33_request(void)
{
  irqstate_t flags;
  unsigned int refs;

  flags = enter_critical_section();

  if (!g_ldo33_configured)
    {
      /* First consumer: take the pad away from any second function and
       * drive it high in one step.  Doing this on the first *request*
       * rather than in a separate init hook means the rail can never be
       * observed configured-but-off, which is the state that left the
       * panel dark.
       */

      bk7258_gpio_output(BK7258_LDO33_EN_GPIO, true);
      g_ldo33_configured = true;
    }
  else if (g_ldo33_refs == 0)
    {
      bk7258_gpio_write(BK7258_LDO33_EN_GPIO, true);
    }

  refs = ++g_ldo33_refs;
  leave_critical_section(flags);

  return refs;
}

unsigned int bk7258_ldo33_release(void)
{
  irqstate_t flags;
  unsigned int refs;

  flags = enter_critical_section();

  if (g_ldo33_refs > 0 && --g_ldo33_refs == 0)
    {
      bk7258_gpio_write(BK7258_LDO33_EN_GPIO, false);
    }

  refs = g_ldo33_refs;
  leave_critical_section(flags);

  return refs;
}

unsigned int bk7258_ldo33_refcount(void)
{
  return g_ldo33_refs;
}
