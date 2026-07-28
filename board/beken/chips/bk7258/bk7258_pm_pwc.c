/****************************************************************************
 * BK7258 PWC worker and CPU1 ready handshake.
 ****************************************************************************/

#include <nuttx/config.h>
#include <stdint.h>
#include <stdbool.h>

#include <nuttx/semaphore.h>
#include <nuttx/sched.h>
#include <nuttx/signal.h>
#include <nuttx/kthread.h>
#include <nuttx/clock.h>

int bk7258_mailbox_init(void);
int bk7258_mailbox_send_pwc(uint8_t command, uint32_t p1, uint32_t p2,
                            uint32_t p3);
void bk7258_mailbox_set_pwc_rx(void (*callback)(const void *message));
#include "hardware/bk7258_mbox.h"

#define PM_CPU1_BOOT_READY_CMD 0x5u
#define PM_QUEUE_DEPTH 8u

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
static volatile bool g_worker_ready;
static volatile bool g_ready_sent;

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
          case 0x7u: /* PSRAM power request: CP owns the PHY transition. */
          case 0xau: /* AP recovery request. */
          case 0x8u: /* allocator state query. */
          case 0x9u: /* allocator diagnostics query. */
            break;
          default:
            break;
        }
    }
  return 0;
}

int bk7258_pwc_start(void)
{
  int pid;
  nxsem_init(&g_pwc_sem, 0, 0);
  nxsem_init(&g_worker_sem, 0, 0);
  g_head = 0;
  g_tail = 0;
  g_worker_ready = false;
  g_ready_sent = false;
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
      return -ETIMEDOUT;
    }
  if (!g_ready_sent)
    {
      g_ready_sent = bk7258_mailbox_send_pwc(PM_CPU1_BOOT_READY_CMD, 1, 0, 0) == 0;
    }
  return g_ready_sent ? 0 : -1;
}
