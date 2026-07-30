/****************************************************************************
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#include <nuttx/config.h>
#include <stddef.h>

#include <nuttx/board.h>

#include <arch/board/board.h>

int bk7258_pwc_start(void);
int bk7258_motor_setup(void);
int bk7258_power_key_motor_start(void);
int bk7258_gc9d01_test(int argc, char **argv);

int bk7258_bringup(void)
{
  int ret;

  board_button_initialize();
  ret = bk7258_motor_setup();
  if (ret < 0)
    {
      return ret;
    }

  ret = bk7258_pwc_start();
  if (ret < 0)
    {
      return ret;
    }

  ret = bk7258_power_key_motor_start();
  if (ret < 0)
    {
      return ret;
    }

  return 0;
}

void board_late_initialize(void)
{
  (void)bk7258_bringup();

  /* GC9D01 QSPI panel bring-up smoke test.  board_app_finalinitialize()
   * (BOARDIOC_FINALINIT) is never invoked in this minimal, apps-less NSH
   * configuration (no CONFIG_BOARDCTL_FINALINIT, no apps/nshlib start-up
   * script caller), so the test entry point is invoked here instead, from
   * the unconditionally-called CONFIG_BOARD_LATE_INITIALIZE hook.  See
   * docs/superpowers/plans/2026-07-29-gc9d01-lcd-bringup.md Task 3 Step 3.
   */
  (void)bk7258_gc9d01_test(0, NULL);
}
