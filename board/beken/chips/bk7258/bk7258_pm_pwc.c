/****************************************************************************
 * BK7258 PWC worker and CPU1 ready handshake.
 ****************************************************************************/

#include <nuttx/config.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

#include <nuttx/semaphore.h>
#include <nuttx/sched.h>
#include <nuttx/signal.h>
#include <nuttx/kthread.h>
#include <nuttx/clock.h>

#include "bk7258_psram.h"
#include "hardware/bk7258_mbox.h"

#define PM_CPU1_BOOT_READY_CMD 0x5u
#define PM_CTRL_PSRAM_POWER_CMD 0x7u
#define PM_CP1_PSRAM_MALLOC_STATE_CMD 0x8u
#define PM_CP1_DUMP_PSRAM_MALLOC_INFO_CMD 0x9u
#define PM_CP1_RECOVERY_CMD 0xau
#define PM_POWER_PSRAM_MODULE_CPU1 11u
#define PM_POWER_MODULE_STATE_ON 0u
#define PM_POWER_MODULE_STATE_OFF 1u
#define PM_RECOVERY_STATE_INIT 0u
#define PM_RECOVERY_STATE_FINISH 1u
#define PM_QUEUE_DEPTH 8u
#define PM_TRANSPORT_TIMEOUT_MS 500u
#define PM_RESPONSE_TIMEOUT_MS 1000u

struct pwc_message
{
  uint32_t header;
  uint32_t param1;
  uint32_t param2;
  uint32_t param3;
};

static struct pwc_message g_queue[PM_QUEUE_DEPTH];
static volatile unsigned int g_head;
static volatile unsigned int g_tail;
static sem_t g_pwc_sem;
static sem_t g_worker_sem;
static sem_t g_psram_power_sem;
static volatile bool g_worker_ready;
static volatile bool g_ready_sent;
static volatile bool g_psram_power_waiting;
static volatile uint32_t g_psram_power_state;

static void psram_power_vote_rollback(void)
{
  int ret;

  ret = bk7258_mailbox_send_pwc(PM_CTRL_PSRAM_POWER_CMD,
                                PM_POWER_PSRAM_MODULE_CPU1,
                                PM_POWER_MODULE_STATE_OFF, 0);
  if (ret >= 0)
    {
      ret = bk7258_mailbox_wait_pwc(PM_TRANSPORT_TIMEOUT_MS);
    }

  if (ret < 0)
    {
      printf("PWC: PSRAM power vote rollback failed, error=%d\n", ret);
    }
}

static int ipc_heartbeat_worker(int argc, char **argv)
{
  static uint32_t heartbeat_payload;

  (void)argc;
  (void)argv;
  for (;;)
    {
      nxsig_usleep(2000000);
      (void)bk7258_mbox_send_message(2, 0x10,
                                     (uint32_t)(uintptr_t)&heartbeat_payload,
                                     sizeof(heartbeat_payload), 0);
    }
  return 0;
}

static void pwc_rx(const void *raw)
{
  const uint8_t *bytes = raw;
  struct pwc_message message;
  unsigned int next = (g_head + 1u) % PM_QUEUE_DEPTH;
  if (next == g_tail || bytes == NULL)
    {
      return;
    }
  message.header = ((const uint32_t *)bytes)[0];
  message.param1 = ((const uint32_t *)bytes)[1];
  message.param2 = ((const uint32_t *)bytes)[2];
  message.param3 = ((const uint32_t *)bytes)[3];
  g_queue[g_head] = message;
  g_head = next;
  nxsem_post(&g_pwc_sem);
}

static int pwc_worker(int argc, char **argv)
{
  (void)argc;
  (void)argv;
  g_worker_ready = true;
  nxsem_post(&g_worker_sem);
  for (;;)
    {
      struct pwc_message message;
      nxsem_wait_uninterruptible(&g_pwc_sem);
      message = g_queue[g_tail];
      g_tail = (g_tail + 1u) % PM_QUEUE_DEPTH;
      /* Hardware power/clock actions stay in this thread, never in IRQ. */
      switch (message.header & 0xffu)
        {
          case PM_CTRL_PSRAM_POWER_CMD:
            /* The unmodified AVDK SMP firmware returns the requested ON/OFF
             * state in param1. It does not expose the PSRAM driver result or
             * detected capacity, so AP validates its configured regions with
             * the non-destructive boundary probe before creating allocators.
             */

            g_psram_power_state = message.param1;
            if (g_psram_power_waiting)
              {
                g_psram_power_waiting = false;
                nxsem_post(&g_psram_power_sem);
              }
            break;

#ifdef CONFIG_BK7258_PSRAM
          case PM_CP1_PSRAM_MALLOC_STATE_CMD:
            if (message.param1 == 0u)
              {
                (void)bk7258_mailbox_send_pwc(
                  PM_CP1_PSRAM_MALLOC_STATE_CMD,
                  PM_CP1_PSRAM_MALLOC_STATE_CMD,
                  bk7258_psram_heap_used(), 0);
              }
            else if (message.param1 == PM_POWER_MODULE_STATE_OFF)
              {
                /* This CP notification is sent after physical power-down. */

                bk7258_psram_power_lost();
              }
            break;

          case PM_CP1_DUMP_PSRAM_MALLOC_INFO_CMD:
            bk7258_psram_dump();
            break;

          case PM_CP1_RECOVERY_CMD:
            {
              int ret = bk7258_psram_shutdown();

              (void)bk7258_mailbox_send_pwc(
                PM_CP1_RECOVERY_CMD, PM_POWER_PSRAM_MODULE_CPU1,
                ret == 0 ? PM_RECOVERY_STATE_FINISH :
                           PM_RECOVERY_STATE_INIT, 0);
            }
            break;
#endif

          default:
            break;
        }
    }
  return 0;
}

int bk7258_pwc_start(void)
{
  int pid;
  int ret;

  nxsem_init(&g_pwc_sem, 0, 0);
  nxsem_init(&g_worker_sem, 0, 0);
  nxsem_init(&g_psram_power_sem, 0, 0);
  g_head = 0;
  g_tail = 0;
  g_worker_ready = false;
  g_ready_sent = false;
  g_psram_power_waiting = false;
  g_psram_power_state = PM_POWER_MODULE_STATE_OFF;
  pid = bk7258_mailbox_init();
  if (pid < 0)
    {
      return pid;
    }
  bk7258_mailbox_set_pwc_rx(pwc_rx);
  /* Match Armino mb_ipc_heartbeat: notify CPU0 before PM ready and keep the
   * CPU1 liveness indication independent from the PWC worker. */
  if (bk7258_mbox_send_message(1, 0x10, 0, 0, 0) != 0)
    {
      return -1;
    }

  ret = bk7258_mailbox_wait_hw_control(PM_TRANSPORT_TIMEOUT_MS);
  if (ret < 0)
    {
      printf("PWC: IPC power-up transport timeout, error=%d\n", ret);
      bk7258_mailbox_dump_stats();
      return ret;
    }
  printf("PWC: IPC power-up transport complete\n");

  if (kthread_create("ipc-heartbeat", 100, 1536, ipc_heartbeat_worker,
                     NULL) < 0)
    {
      return -1;
    }
  pid = kthread_create("pwc", 110, 2048, pwc_worker, NULL);
  if (pid < 0)
    {
      return pid;
    }
  if (nxsem_tickwait_uninterruptible(&g_worker_sem, MSEC2TICK(200)) < 0 ||
      !g_worker_ready)
    {
      printf("PWC: worker start timeout\n");
      bk7258_mailbox_dump_stats();
      return -ETIMEDOUT;
    }
  printf("PWC: worker ready\n");

  /* CP boots CPU1 from its low-power worker and waits there for this ready
   * command. Send it before requesting PSRAM, whose vote is handled by that
   * same worker, otherwise the two cores deadlock waiting for each other.
   */

  g_ready_sent = bk7258_mailbox_send_pwc(PM_CPU1_BOOT_READY_CMD,
                                         1, 0, 0) == 0;
  if (!g_ready_sent)
    {
      printf("PWC: CPU1 boot-ready queue failed\n");
      bk7258_mailbox_dump_stats();
      return -EIO;
    }

  ret = bk7258_mailbox_wait_pwc(PM_TRANSPORT_TIMEOUT_MS);
  if (ret < 0)
    {
      printf("PWC: CPU1 boot-ready transport timeout, error=%d\n", ret);
      bk7258_mailbox_dump_stats();
      return ret;
    }

  printf("PWC: CPU1 boot-ready acknowledged by CP transport\n");

#ifdef CONFIG_BK7258_PSRAM
  g_psram_power_waiting = true;
  ret = bk7258_mailbox_send_pwc(PM_CTRL_PSRAM_POWER_CMD,
                                PM_POWER_PSRAM_MODULE_CPU1,
                                PM_POWER_MODULE_STATE_ON, 0);
  if (ret >= 0)
    {
      ret = nxsem_tickwait_uninterruptible(&g_psram_power_sem,
                         MSEC2TICK(PM_RESPONSE_TIMEOUT_MS));
    }

  g_psram_power_waiting = false;
  if (ret < 0 || g_psram_power_state != PM_POWER_MODULE_STATE_ON)
    {
      printf("PWC: PSRAM power response failed, error=%d state=%lu\n",
             ret, (unsigned long)g_psram_power_state);
      bk7258_mailbox_dump_stats();
      psram_power_vote_rollback();
#ifdef CONFIG_BK7258_PSRAM_REQUIRED
      return ret < 0 ? ret : -EIO;
#endif
    }
  else
    {
      /* printf("PWC: PSRAM power vote ON acknowledged\n"); */
      ret = bk7258_psram_initialize();
      if (ret < 0)
        {
          printf("PWC: PSRAM allocator initialization failed, error=%d\n",
                 ret);
          psram_power_vote_rollback();
#ifdef CONFIG_BK7258_PSRAM_REQUIRED
          return ret;
#endif
        }
      else
        {
          /* printf("PWC: PSRAM allocators ONLINE\n"); */
        }
    }
#endif

  if (g_ready_sent)
    {
      return 0;
    }

  return -1;
}
