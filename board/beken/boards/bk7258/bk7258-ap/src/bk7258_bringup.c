/****************************************************************************
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#include <nuttx/config.h>
#include <pthread.h>
#include <sched.h>
#include <stddef.h>
#include <stdio.h>

#include <nuttx/board.h>
#include <nuttx/fs/fs.h>
#include <nuttx/video/fb.h>

#include <arch/board/board.h>

#include "bk7258_psram.h"
#include "bk7258_ramdisk.h"
#include "bk7258_gc9d01_fb.h"
#include "bk7258_boottrace.h"
#include "bk7258_smp.h"
#include "hardware/bk7258_mbox.h"
#include "bk7258_wifi.h"

#ifdef CONFIG_BK7258_AUDIO
#  include "bk7258_audio_bringup.h"
#endif

#ifdef CONFIG_BK7258_JPEG_ENC
#  include "bk7258_jpeg_enc.h"
#endif

#define BK7258_LINK_WAIT_MS 8000u
#define BK7258_BRINGUP_PRIORITY 100
#define BK7258_BRINGUP_STACKSIZE 4096

int bk7258_pwc_start(void);
int bk7258_motor_setup(void);
int bk7258_power_key_motor_start(void);
int bk7258_gc9d01_test(int argc, char **argv);
int bk7258_camera_initialize(void);

int bk7258_bringup(void)
{
  uint32_t button_count;
  int ret;

  bk7258_boottrace_primary(BK7258_BOOT_BRINGUP_ENTER);
#ifdef CONFIG_FS_PROCFS
  /* Mount procfs first so that ps, free and the other informational NSH
   * commands work even if a later bring-up step returns early. A mount
   * failure is reported but never blocks bring-up: procfs is not required
   * by the console data path.
   */

  ret = nx_mount(NULL, "/proc", "procfs", 0, NULL);
  if (ret < 0)
    {
      printf("failed to mount procfs at /proc, error=%d\n", ret);
    }
#endif

  button_count = board_button_initialize();

  ret = bk7258_motor_setup();
  if (ret < 0)
    {
      return ret;
    }

  ret = bk7258_mailbox_workers_start();
  if (ret < 0)
    {
      printf("mailbox worker start failed, error=%d\n", ret);
      return ret;
    }

  bk7258_boottrace_primary(BK7258_BOOT_BRINGUP_WORKERS);

  ret = bk7258_mb_uart_worker_start();
  if (ret < 0)
    {
      printf("mailbox UART0 worker start failed, error=%d\n", ret);
      return ret;
    }

  bk7258_boottrace_primary(BK7258_BOOT_BRINGUP_UART_WORKER);

  /* Create every CPU0-only transport worker before enabling SysTick. */

  bk7258_boottrace_primary(BK7258_BOOT_BRINGUP_TIMER_START);
  ret = bk7258_timer_start();
  if (ret < 0)
    {
      printf("system tick start failed, error=%d\n", ret);
      return ret;
    }

  bk7258_boottrace_primary(BK7258_BOOT_BRINGUP_TIMER_RETURN);

  /* Workers were created at their final priorities.  Avoid reprioritizing
   * live SMP tasks during boot; the workers preempt this priority-100 thread
   * only long enough to drain transport state and then block. */

  bk7258_boottrace_primary(BK7258_BOOT_BRINGUP_ACTIVATE);

  bk7258_boottrace_primary(BK7258_BOOT_WORKERS_READY);

  bk7258_mb_uart_start();

  bk7258_boottrace_primary(BK7258_BOOT_BRINGUP_UART_START);

  bk7258_boottrace_primary(BK7258_BOOT_BRINGUP_LINK_WAIT);
  ret = bk7258_mailbox_wait_link_ready(BK7258_LINK_WAIT_MS);
  if (ret < 0)
    {
      printf("mailbox UART0 STATE probe failed, error=%d\n", ret);
      bk7258_mailbox_dump_stats();
      bk7258_mbox_uart_dump_stats();
      return ret;
    }

  bk7258_boottrace_primary(BK7258_BOOT_BRINGUP_LINK_READY);

  printf("mailbox UART0 link ready\n");
  bk7258_boottrace_primary(BK7258_BOOT_LINK_READY);

  ret = bk7258_ipc_heartbeat_start();
  if (ret < 0)
    {
      printf("HW_CTRL power-up service failed, error=%d\n", ret);
      bk7258_mailbox_dump_stats();
      return ret;
    }

  ret = bk7258_pwc_start();
  if (ret < 0)
    {
      return ret;
    }

#ifdef CONFIG_BK7258_WIFI
  ret = bk7258_wifi_initialize();
  if (ret < 0)
    {
      return ret;
    }
#endif

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

      /* PSRAM-backed /dev/ram0: the only storage on this board big enough
       * to hold a captured frame as a file (tmpfs lives in the ~300KB
       * kernel heap, one 640x480 YUYV frame is 614400 bytes).
       */

      (void)bk7258_ramdisk_initialize();
      /* printf("PSRAM CP heap 0x60700000..0x6071ffff reserved\n"); */
      /* bk7258_psram_dump(); */
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

static void *bk7258_bringup_worker(void *arg)
{
  int ret = bk7258_bringup();

  (void)arg;
  if (ret < 0)
    {
      printf("board bring-up stopped, error=%d\n", ret);
      return NULL;
    }

  printf("board bring-up initialization completed\n");

  /* GC9D01 QSPI panel bring-up smoke test.  board_app_finalinitialize()
   * (BOARDIOC_FINALINIT) is never invoked in this minimal, apps-less NSH
   * configuration (no CONFIG_BOARDCTL_FINALINIT, no apps/nshlib start-up
   * script caller), so the test entry point is invoked here instead, from
   * the unconditionally-called CONFIG_BOARD_LATE_INITIALIZE hook.  See
   * docs/superpowers/plans/2026-07-29-gc9d01-lcd-bringup.md Task 3 Step 3.
   */
#ifdef CONFIG_BK7258_GC9D01_FB
  /* GC9D01 framebuffer: fb_register() calls up_fbinitialize(), which does
   * the panel reset / QSPI bring-up / init sequence and allocates the
   * 51200-byte RGB565 framebuffer, then publishes /dev/fb0.
   */

  ret = fb_register(0, 0);
  if (ret < 0)
    {
      printf("fb_register() failed: %d\n", ret);
    }
  else
    {
      /* The panel has no readable ID register, so "the init sequence was
       * sent" has never been evidence that it worked.  Draw a pattern once
       * at boot: four quadrants with a black border makes both a byte-order
       * mistake (wrong colours) and a stride mistake (sheared boundaries)
       * visible at a glance.
       */

      (void)bk7258_gc9d01_fb_test_pattern();
      printf("fb: /dev/fb0 registered, boot test pattern pushed\n");
    }
#endif

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

#ifdef CONFIG_BK7258_AUDIO
  /* Internal-DAC audio.  This has to come after bk7258_bringup() above,
   * not from inside it, for the same reason the camera does: the AUD block
   * needs the AUDP power domain and the AUD module clock, and both are
   * requested from CP1 over the PWC mailbox channel that bk7258_pwc_start()
   * brings up.  Registering the audio devices is best-effort -- a failure
   * here must not take the console down with it.
   */

  (void)bk7258_audio_initialize();
#endif

#ifdef CONFIG_BK7258_JPEG_ENC
  /* After the camera: both want PSRAM, and registering the encoder second
   * keeps /dev/video0 belonging to capture and /dev/video1 to this codec no
   * matter how the framework numbers them internally.  Best-effort, like the
   * others -- a codec that fails to register must not take the console down.
   */

  (void)bk7258_jpeg_enc_initialize();
#endif
  return NULL;
}

void board_late_initialize(void)
{
  struct sched_param param;
  pthread_attr_t attr;
  pthread_t thread;
  cpu_set_t cpuset;
  int ret;

  pthread_attr_init(&attr);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
  pthread_attr_setstacksize(&attr, BK7258_BRINGUP_STACKSIZE);
  pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
  param.sched_priority = BK7258_BRINGUP_PRIORITY;
  pthread_attr_setschedparam(&attr, &param);
#ifdef CONFIG_SMP
  CPU_ZERO(&cpuset);
  CPU_SET(0, &cpuset);
  pthread_attr_setaffinity_np(&attr, sizeof(cpuset), &cpuset);
#endif

  ret = pthread_create(&thread, &attr, bk7258_bringup_worker, NULL);
  pthread_attr_destroy(&attr);
  if (ret != 0)
    {
      printf("board bring-up worker create failed, error=%d\n", ret);
    }
}
