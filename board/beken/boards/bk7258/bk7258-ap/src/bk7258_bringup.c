/****************************************************************************
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#include <nuttx/config.h>
#include <stddef.h>
#include <stdio.h>

#include <nuttx/board.h>

#include <arch/board/board.h>

#include "bk7258_psram.h"

int bk7258_pwc_start(void);
int bk7258_motor_setup(void);
int bk7258_power_key_motor_start(void);
int bk7258_gc9d01_test(int argc, char **argv);
int bk7258_camera_initialize(void);

int bk7258_bringup(void)
{
  uint32_t button_count;
  int ret;

  button_count = board_button_initialize();

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

  printf("button GPIOs configured, count=%lu\n",
         (unsigned long)button_count);
  printf("PWM device registered at /dev/pwm0\n");
  printf("mailbox transport, PWC and CPU1 ready handshake complete\n");
  printf("heartbeat worker started, interval=2000 ms\n");
#ifdef CONFIG_BK7258_PSRAM
  if (bk7258_psram_is_online())
    {
      printf("PSRAM online, base=0x60000000 size=0x01000000 "
             "RW/XN/non-cacheable\n");
      printf("PSRAM CP heap 0x60700000..0x6071ffff reserved\n");
      bk7258_psram_dump();
    }
  else
    {
      printf("PSRAM unavailable; media services must remain disabled\n");
    }
#endif

  ret = bk7258_power_key_motor_start();
  if (ret < 0)
    {
      return ret;
    }

  return 0;
}

void board_late_initialize(void)
{
  int ret = bk7258_bringup();

  if (ret < 0)
    {
      printf("board bring-up stopped, error=%d\n", ret);
      return;
    }

  printf("board bring-up initialization completed\n");

  /* GC9D01 QSPI panel bring-up smoke test.  board_app_finalinitialize()
   * (BOARDIOC_FINALINIT) is never invoked in this minimal, apps-less NSH
   * configuration (no CONFIG_BOARDCTL_FINALINIT, no apps/nshlib start-up
   * script caller), so the test entry point is invoked here instead, from
   * the unconditionally-called CONFIG_BOARD_LATE_INITIALIZE hook.  See
   * docs/superpowers/plans/2026-07-29-gc9d01-lcd-bringup.md Task 3 Step 3.
   */
  (void)bk7258_gc9d01_test(0, NULL);

  /* GC2145 camera: registered as a standard V4L2 /dev/video0 node here
   * (imgdata+imgsensor framework, see bk7258_camera_bringup.c), superseding
   * the older bare board_late_initialize() smoke-test call this line used
   * to make directly.  The old bk7258_gc2145_test() bare entry point is
   * kept as an independent NSH command (see bk7258_appinit.c) for
   * side-by-side diagnostic comparison, per design discussion in
   * docs/superpowers/plans/2026-07-30-gc2145-camera-bringup.md -- it is
   * intentionally NOT called from here anymore, so a bug in this new V4L2
   * driver cannot be masked by (or confused with) the old direct-call
   * path still running unconditionally at boot. */
  (void)bk7258_camera_initialize();
}
