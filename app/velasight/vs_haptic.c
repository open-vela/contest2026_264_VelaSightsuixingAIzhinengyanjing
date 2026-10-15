/****************************************************************************
 * app/velasight/vs_haptic.c
 *
 * See include/vs_haptic.h.  Drives /dev/pwm0 the way app/social_cue's
 * sc_buzz() does -- 1 kHz at 70% duty, start, wait, stop -- the pattern the
 * 8.16 bring-up record verified on this board as "提示 2 短".
 *
 * Each step of a pattern is one work item: open and start, stop, start
 * again, and finally stop and close.  The device is open only while a
 * pattern runs, and from the worker thread only, so the descriptor is never
 * shared between tasks; closing it lets the PWM driver gate its clock and
 * hand back its reference on the rail the panels share.
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include <nuttx/clock.h>
#include <nuttx/wqueue.h>

#ifdef CONFIG_PWM
#  include <nuttx/timers/pwm.h>
#endif

#include "include/vs_haptic.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#if defined(CONFIG_PWM) && defined(CONFIG_SCHED_HPWORK)
#  define HAPTIC_SUPPORTED 1
#endif

#define HAPTIC_DEV         "/dev/pwm0"
#define HAPTIC_FREQ_HZ     1000
#define HAPTIC_DUTY        ((uint32_t)((7u * 65536u) / 10u))

#define HAPTIC_MAX_PULSES  5u
#define HAPTIC_MIN_MS      20u
#define HAPTIC_MAX_MS      1000u

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct vs_haptic_s
{
  struct work_s work;
  int      fd;          /* open only while a pattern runs */
  uint16_t on_ms;
  uint16_t off_ms;
  uint8_t  remaining;   /* pulses not yet finished */
  bool     on;          /* the motor is being driven */
  bool     busy;        /* a pattern owns the fields above */
  bool     warned;      /* the missing-device line was printed */
  uint32_t alert_ms;    /* start of the last alert pattern, 0 never */
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct vs_haptic_s g_haptic =
{
  .fd = -1
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

#ifdef HAPTIC_SUPPORTED

/* End the pattern from the worker: motor off, device closed, idle again. */

static void haptic_finish(struct vs_haptic_s *h)
{
  if (h->fd >= 0)
    {
      if (h->on)
        {
          (void)ioctl(h->fd, PWMIOC_STOP, 0);
        }

      close(h->fd);
      h->fd = -1;
    }

  h->on = false;
  __atomic_store_n(&h->busy, false, __ATOMIC_RELEASE);
}

static void haptic_step(void *arg)
{
  struct vs_haptic_s *h = arg;
  unsigned int wait_ms;

  if (!h->on)
    {
      if (h->fd < 0)
        {
          struct pwm_info_s info;

          h->fd = open(HAPTIC_DEV, O_RDONLY);
          if (h->fd < 0)
            {
              if (!h->warned)
                {
                  h->warned = true;
                  printf("velasight: no %s (%d), no vibration\n",
                         HAPTIC_DEV, errno);
                }

              haptic_finish(h);
              return;
            }

          memset(&info, 0, sizeof(info));
          info.frequency = HAPTIC_FREQ_HZ;
          info.duty      = (ub16_t)HAPTIC_DUTY;
          if (ioctl(h->fd, PWMIOC_SETCHARACTERISTICS,
                    (unsigned long)(uintptr_t)&info) < 0)
            {
              printf("velasight: vibration setup failed (%d)\n", errno);
              haptic_finish(h);
              return;
            }
        }

      if (ioctl(h->fd, PWMIOC_START, 0) < 0)
        {
          printf("velasight: vibration start failed (%d)\n", errno);
          haptic_finish(h);
          return;
        }

      h->on   = true;
      wait_ms = h->on_ms;
    }
  else
    {
      (void)ioctl(h->fd, PWMIOC_STOP, 0);
      h->on = false;

      if (--h->remaining == 0)
        {
          haptic_finish(h);
          return;
        }

      wait_ms = h->off_ms;
    }

  /* A pattern that cannot schedule its next step must not leave the motor
   * running until the next one happens to come along.
   */

  if (work_queue(HPWORK, &h->work, haptic_step, h, MSEC2TICK(wait_ms)) < 0)
    {
      haptic_finish(h);
    }
}

#endif /* HAPTIC_SUPPORTED */

#ifdef CONFIG_VS_HAPTIC_ALERT
static uint32_t haptic_now_ms(void)
{
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint32_t)(ts.tv_sec * 1000u + ts.tv_nsec / 1000000u);
}
#endif

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int vs_haptic_pulse(unsigned int count, unsigned int on_ms,
                    unsigned int off_ms)
{
#ifdef HAPTIC_SUPPORTED
  struct vs_haptic_s *h = &g_haptic;
  bool idle = false;
  int ret;

  if (count == 0 || count > HAPTIC_MAX_PULSES ||
      on_ms < HAPTIC_MIN_MS || on_ms > HAPTIC_MAX_MS ||
      (count > 1 && (off_ms < HAPTIC_MIN_MS || off_ms > HAPTIC_MAX_MS)))
    {
      return -EINVAL;
    }

  if (!__atomic_compare_exchange_n(&h->busy, &idle, true, false,
                                   __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
    {
      return -EBUSY;
    }

  h->remaining = (uint8_t)count;
  h->on_ms     = (uint16_t)on_ms;
  h->off_ms    = (uint16_t)off_ms;
  h->on        = false;

  ret = work_queue(HPWORK, &h->work, haptic_step, h, 0);
  if (ret < 0)
    {
      __atomic_store_n(&h->busy, false, __ATOMIC_RELEASE);
      return ret;
    }

  return 0;
#else
  (void)count;
  (void)on_ms;
  (void)off_ms;
  return -ENOSYS;
#endif
}

int vs_haptic_alert(void)
{
#ifdef CONFIG_VS_HAPTIC_ALERT
  uint32_t now = haptic_now_ms();
  int ret;

  if (g_haptic.alert_ms != 0 &&
      now - g_haptic.alert_ms <
      (uint32_t)CONFIG_VS_HAPTIC_ALERT_COOLDOWN_MS)
    {
      return -EALREADY;
    }

  ret = vs_haptic_pulse(CONFIG_VS_HAPTIC_ALERT_PULSES,
                        CONFIG_VS_HAPTIC_ALERT_ON_MS,
                        CONFIG_VS_HAPTIC_ALERT_OFF_MS);
  if (ret == 0)
    {
      g_haptic.alert_ms = now != 0 ? now : 1;
    }

  return ret;
#else
  return -ENOSYS;
#endif
}

bool vs_haptic_busy(void)
{
  return __atomic_load_n(&g_haptic.busy, __ATOMIC_ACQUIRE);
}
