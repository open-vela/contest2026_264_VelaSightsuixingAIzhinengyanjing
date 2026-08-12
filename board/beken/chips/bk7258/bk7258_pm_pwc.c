/****************************************************************************
 * BK7258 PWC worker and CPU1 ready handshake.
 ****************************************************************************/

#include <nuttx/config.h>
#include <errno.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

#include <nuttx/semaphore.h>
#include <nuttx/sched.h>
#include <nuttx/signal.h>
#include <nuttx/kthread.h>
#include <nuttx/clock.h>
#include <nuttx/mutex.h>

#include "bk7258_driver.h"
#include "bk7258_psram.h"
#include "hardware/bk7258_mbox.h"
#include "bk7258_boottrace.h"

#define PM_CPU1_BOOT_READY_CMD 0x5u
#define PM_OPENVELA_READY_CMD 0x11u
#define PM_CTRL_PSRAM_POWER_CMD 0x7u
#define PM_CP1_PSRAM_MALLOC_STATE_CMD 0x8u
#define PM_CP1_DUMP_PSRAM_MALLOC_INFO_CMD 0x9u
#define PM_CP1_RECOVERY_CMD 0xau
#define PM_POWER_PSRAM_MODULE_CPU1 11u
#define PM_POWER_MODULE_STATE_ON 0u
#define PM_POWER_MODULE_STATE_OFF 1u
#define PM_RECOVERY_STATE_INIT 0u
#define PM_RECOVERY_STATE_FINISH 1u
#define PM_OPENVELA_READY_PROTO_V1 1u
#define PM_PSRAM_PROTO_V1 1u
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
static sem_t g_openvela_ready_sem;
static mutex_t g_pwc_tx_lock = NXMUTEX_INITIALIZER;
#ifdef CONFIG_BK7258_PSRAM
static mutex_t g_psram_power_lock = NXMUTEX_INITIALIZER;
#endif
static volatile bool g_worker_ready;
static volatile int g_worker_result;
static volatile bool g_psram_power_waiting;
static volatile uint32_t g_psram_power_expected_state;
static volatile uint32_t g_psram_power_state;
static volatile int32_t g_psram_power_result;
static volatile uint32_t g_psram_power_version;
static volatile bool g_openvela_ready_waiting;
static volatile bool g_openvela_ready_received;
static volatile uint32_t g_openvela_ready_expected;
static volatile uint32_t g_openvela_ready_echo;
static volatile int32_t g_openvela_ready_result;
static volatile uint32_t g_openvela_ready_version;

static int pwc_send_wait(uint8_t command, uint32_t param1, uint32_t param2,
                         uint32_t param3);

#ifdef CONFIG_BK7258_PSRAM
static int psram_power_set(uint32_t state)
{
  irqstate_t flags;
  int response_ret;
  int transport_ret;
  int ret;

  ret = nxmutex_lock(&g_psram_power_lock);
  if (ret < 0)
    {
      return ret;
    }

  ret = nxsem_reset(&g_psram_power_sem, 0);
  if (ret < 0)
    {
      nxmutex_unlock(&g_psram_power_lock);
      return ret;
    }

  flags = rspin_lock_irqsave(&g_bk7258_driver_lock);
  g_psram_power_expected_state = state;
  g_psram_power_state = UINT32_MAX;
  g_psram_power_result = -EINPROGRESS;
  g_psram_power_version = 0;
  g_psram_power_waiting = true;
  rspin_unlock_irqrestore(&g_bk7258_driver_lock, flags);

  ret = pwc_send_wait(PM_CTRL_PSRAM_POWER_CMD,
                      PM_POWER_PSRAM_MODULE_CPU1, state,
                      PM_PSRAM_PROTO_V1);
  if (ret == OK)
    {
      transport_ret = OK;
      response_ret = nxsem_tickwait_uninterruptible(
                       &g_psram_power_sem,
                       MSEC2TICK(PM_RESPONSE_TIMEOUT_MS));
      ret = transport_ret != OK ? transport_ret : response_ret;
      if (ret > 0)
        {
          ret = -EIO;
        }
    }
  else if (ret > 0)
    {
      ret = -EIO;
    }

  flags = rspin_lock_irqsave(&g_bk7258_driver_lock);
  g_psram_power_waiting = false;
  rspin_unlock_irqrestore(&g_bk7258_driver_lock, flags);

  if (ret == OK && (g_psram_power_state != state ||
                    g_psram_power_result != OK ||
                    g_psram_power_version != PM_PSRAM_PROTO_V1))
    {
      ret = g_psram_power_result < 0 ? g_psram_power_result : -EPROTO;
    }

  if (ret < 0)
    {
      printf("PWC: PSRAM power %s failed, error=%d result=%ld "
             "state=%lu version=%lu\n",
             state == PM_POWER_MODULE_STATE_ON ? "ON" : "OFF", ret,
             (long)g_psram_power_result,
             (unsigned long)g_psram_power_state,
             (unsigned long)g_psram_power_version);
    }

  nxmutex_unlock(&g_psram_power_lock);
  return ret;
}
#endif

static int pwc_send_wait(uint8_t command, uint32_t param1, uint32_t param2,
                          uint32_t param3)
{
  int ret;

  ret = nxmutex_lock(&g_pwc_tx_lock);
  if (ret < 0)
    {
      return ret;
    }

  ret = bk7258_mailbox_send_pwc(command, param1, param2, param3);
  if (ret == OK)
    {
      ret = bk7258_mailbox_wait_pwc(PM_TRANSPORT_TIMEOUT_MS);
      if (ret > 0)
        {
          ret = -EIO;
        }
    }
  else if (ret > 0)
    {
      ret = -EIO;
    }

  nxmutex_unlock(&g_pwc_tx_lock);
  return ret;
}

static int openvela_ready_request(uint32_t ready, int32_t cause,
                                  int32_t *semantic_result)
{
  irqstate_t flags;
  bool received;
  uint32_t echo;
  uint32_t version;
  int32_t result;
  int ret;

  ret = nxsem_reset(&g_openvela_ready_sem, 0);
  if (ret < 0)
    {
      return ret;
    }

  flags = rspin_lock_irqsave(&g_bk7258_driver_lock);
  g_openvela_ready_expected = ready;
  g_openvela_ready_echo = UINT32_MAX;
  g_openvela_ready_result = -EINPROGRESS;
  g_openvela_ready_version = 0;
  g_openvela_ready_received = false;
  g_openvela_ready_waiting = true;
  rspin_unlock_irqrestore(&g_bk7258_driver_lock, flags);

  ret = pwc_send_wait(PM_OPENVELA_READY_CMD, ready, (uint32_t)cause,
                      PM_OPENVELA_READY_PROTO_V1);
  if (ret == OK)
    {
      ret = nxsem_tickwait_uninterruptible(&g_openvela_ready_sem,
                                           MSEC2TICK(PM_RESPONSE_TIMEOUT_MS));
    }

  if (ret > 0)
    {
      ret = -EIO;
    }

  flags = rspin_lock_irqsave(&g_bk7258_driver_lock);
  g_openvela_ready_waiting = false;
  received = g_openvela_ready_received;
  echo = g_openvela_ready_echo;
  result = g_openvela_ready_result;
  version = g_openvela_ready_version;
  rspin_unlock_irqrestore(&g_bk7258_driver_lock, flags);

  if (ret == OK && (!received || echo != ready ||
                    version != PM_OPENVELA_READY_PROTO_V1))
    {
      ret = -EPROTO;
    }

  if (ret == OK)
    {
      *semantic_result = result;
    }

  return ret;
}

static int openvela_ready_abort(int error)
{
  int ret;

  if (error >= 0)
    {
      error = -EIO;
    }

  /* The transport ACK proves CP accepted ownership of rollback.  CP may
   * reset/power off this core while handling the request, so an application
   * response cannot be required on the abort path.
   */

  ret = pwc_send_wait(PM_OPENVELA_READY_CMD, 0, (uint32_t)error,
                      PM_OPENVELA_READY_PROTO_V1);

  if (ret < 0)
    {
      printf("PWC: OpenVela ready abort failed, error=%d cause=%d\n",
             ret, error);
      bk7258_mailbox_dump_stats();
    }
  else
    {
      printf("PWC: OpenVela rollback accepted, cause=%d\n", error);
    }

  return ret;
}

static int openvela_ready_commit(void)
{
  int32_t semantic_result = -EINPROGRESS;
  int ret;

  ret = openvela_ready_request(1, 0, &semantic_result);
  if (ret < 0)
    {
      return ret;
    }

  if (semantic_result == OK)
    {
      return OK;
    }

  return semantic_result < 0 ? semantic_result : -EPROTO;
}

static int pwc_rx(const void *raw)
{
  const uint8_t *bytes = raw;
  struct pwc_message message;
  unsigned int next;

  if (bytes == NULL)
    {
      return -EINVAL;
    }

  /* The logical mailbox invokes PWC callbacks while holding the driver lock.
   * Keep the queue update in that critical section instead of recursively
   * acquiring the non-recursive SMP spinlock here. */

  next = (g_head + 1u) % PM_QUEUE_DEPTH;
  if (next == g_tail)
    {
      return -EAGAIN;
    }

  message.header = ((const uint32_t *)bytes)[0];
  message.param1 = ((const uint32_t *)bytes)[1];
  message.param2 = ((const uint32_t *)bytes)[2];
  message.param3 = ((const uint32_t *)bytes)[3];
  g_queue[g_head] = message;
  g_head = next;
  nxsem_post(&g_pwc_sem);
  return OK;
}

static int pwc_worker(int argc, char **argv)
{
  int ret = OK;

  (void)argc;
  (void)argv;
  bk7258_boottrace_primary(BK7258_BOOT_PWC_WORKER_ENTER);

  /* bk7258_mailbox_workers_start() pinned the bring-up thread to CPU0.
   * Kernel threads inherit that affinity before activation, so changing the
   * running PWC worker's affinity here is redundant and can trigger another
   * SMP scheduler transaction during the boot handshake. */

  g_worker_result = OK;
  g_worker_ready = true;
  bk7258_boottrace_primary(BK7258_BOOT_PWC_WORKER_READY);
  nxsem_post(&g_worker_sem);

  for (;;)
    {
      struct pwc_message message;
      irqstate_t flags;

      nxsem_wait_uninterruptible(&g_pwc_sem);
      flags = rspin_lock_irqsave(&g_bk7258_driver_lock);
      message = g_queue[g_tail];
      g_tail = (g_tail + 1u) % PM_QUEUE_DEPTH;
      rspin_unlock_irqrestore(&g_bk7258_driver_lock, flags);
      /* A full software queue leaves the physical descriptor deferred with
       * no transport ACK.  Freeing one entry is the progress event that must
       * retry that descriptor even when no new mailbox IRQ arrives. */

      bk7258_mbox_kick_rx();
      /* Hardware power/clock actions stay in this thread, never in IRQ. */
      switch (message.header & 0xffu)
        {
          case PM_CTRL_PSRAM_POWER_CMD:
            flags = rspin_lock_irqsave(&g_bk7258_driver_lock);
            if (g_psram_power_waiting &&
                message.param1 == g_psram_power_expected_state)
              {
                g_psram_power_state = message.param1;
                g_psram_power_result = (int32_t)message.param2;
                g_psram_power_version = message.param3;
                g_psram_power_waiting = false;
                nxsem_post(&g_psram_power_sem);
              }
            rspin_unlock_irqrestore(&g_bk7258_driver_lock, flags);
            break;

          case PM_OPENVELA_READY_CMD:
            flags = rspin_lock_irqsave(&g_bk7258_driver_lock);
            if (g_openvela_ready_waiting &&
                message.param1 == g_openvela_ready_expected)
              {
                g_openvela_ready_echo = message.param1;
                g_openvela_ready_result = (int32_t)message.param2;
                g_openvela_ready_version = message.param3;
                g_openvela_ready_received = true;
                g_openvela_ready_waiting = false;
                nxsem_post(&g_openvela_ready_sem);
              }
            rspin_unlock_irqrestore(&g_bk7258_driver_lock, flags);
            break;

#ifdef CONFIG_BK7258_PSRAM
          case PM_CP1_PSRAM_MALLOC_STATE_CMD:
            if (message.param1 == 0u)
              {
                ret = pwc_send_wait(PM_CP1_PSRAM_MALLOC_STATE_CMD,
                                    PM_CP1_PSRAM_MALLOC_STATE_CMD,
                                    bk7258_psram_heap_used(), 0);
                if (ret != OK)
                  {
                    printf("PWC: PSRAM malloc state response failed, "
                           "error=%d\n", ret);
                  }
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
#ifdef CONFIG_SMP
              ret = pwc_send_wait(PM_CP1_RECOVERY_CMD,
                                  PM_RECOVERY_STATE_INIT,
                                  (uint32_t)-ENOTSUP, 0);
#else
              int shutdown_ret = bk7258_psram_shutdown();

              ret = pwc_send_wait(
                      PM_CP1_RECOVERY_CMD, PM_POWER_PSRAM_MODULE_CPU1,
                      shutdown_ret == 0 ? PM_RECOVERY_STATE_FINISH :
                                          PM_RECOVERY_STATE_INIT, 0);
#endif
              if (ret != OK)
                {
                  printf("PWC: recovery response failed, error=%d\n", ret);
                }
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

  bk7258_boottrace_primary(BK7258_BOOT_PWC_ENTER);
  bk7258_boottrace_detail(3, 0x50570001u);

  nxsem_init(&g_pwc_sem, 0, 0);
  nxsem_init(&g_worker_sem, 0, 0);
  nxsem_init(&g_psram_power_sem, 0, 0);
  nxsem_init(&g_openvela_ready_sem, 0, 0);
  g_head = 0;
  g_tail = 0;
  g_worker_ready = false;
  g_worker_result = -EINPROGRESS;
  g_psram_power_waiting = false;
  g_psram_power_expected_state = PM_POWER_MODULE_STATE_OFF;
  g_psram_power_state = PM_POWER_MODULE_STATE_OFF;
  g_psram_power_result = -EINPROGRESS;
  g_psram_power_version = 0;
  g_openvela_ready_waiting = false;
  g_openvela_ready_received = false;
  g_openvela_ready_expected = 0;
  g_openvela_ready_echo = UINT32_MAX;
  g_openvela_ready_result = -EINPROGRESS;
  g_openvela_ready_version = 0;
  bk7258_mailbox_set_pwc_rx(pwc_rx);

  bk7258_boottrace_primary(BK7258_BOOT_PWC_CREATE_ENTER);
  pid = kthread_create("pwc", 110, 2048, pwc_worker, NULL);
  if (pid < 0)
    {
      bk7258_boottrace_primary(BK7258_BOOT_PWC_CREATE_FAILED);
      bk7258_boottrace_detail(3, 0x5057f000u |
                             ((uint32_t)-pid & 0xfffu));
      return pid;
    }

  bk7258_boottrace_primary(BK7258_BOOT_PWC_CREATE_RETURN);
  if (nxsem_tickwait_uninterruptible(&g_worker_sem, MSEC2TICK(200)) < 0)
    {
      printf("PWC: worker start timeout\n");
      bk7258_mailbox_dump_stats();
      return -ETIMEDOUT;
    }
  if (g_worker_result < 0 || !g_worker_ready)
    {
      printf("PWC: worker affinity failed, error=%d\n", g_worker_result);
      return g_worker_result < 0 ? g_worker_result : -EIO;
    }

  bk7258_boottrace_primary(BK7258_BOOT_PWC_READY_WAIT_DONE);
  printf("PWC: worker ready\n");

  /* This is the core/SMP milestone.  The original HW_CTRL liveness service
   * is already running and the PWC worker can now service CP requests. */

  bk7258_boottrace_primary(BK7258_BOOT_PWC_BOOT_READY_SEND);
  ret = pwc_send_wait(PM_CPU1_BOOT_READY_CMD, 1, 0, 0);
  if (ret < 0)
    {
      bk7258_boottrace_detail(3, 0x5057e000u | ((uint32_t)-ret & 0xfffu));
      printf("PWC: CPU1 boot-ready failed, error=%d\n", ret);
      bk7258_mailbox_dump_stats();
      return ret;
    }

  bk7258_boottrace_primary(BK7258_BOOT_READY_SENT);
  bk7258_boottrace_detail(3, 0x50570002u);
  bk7258_boottrace_detail(3, 0x50570003u);
  printf("PWC: CPU1 boot-ready acknowledged by CP transport\n");

#ifdef CONFIG_BK7258_PSRAM
  ret = psram_power_set(PM_POWER_MODULE_STATE_ON);
  if (ret < 0)
    {
      bk7258_mailbox_dump_stats();
      if (psram_power_set(PM_POWER_MODULE_STATE_OFF) < 0)
        {
          printf("PWC: PSRAM rollback response failed\n");
        }

      openvela_ready_abort(ret);
      return ret;
    }

  ret = bk7258_psram_initialize();
  if (ret < 0)
    {
      printf("PWC: PSRAM allocator initialization failed, error=%d\n", ret);
      if (psram_power_set(PM_POWER_MODULE_STATE_OFF) < 0)
        {
          printf("PWC: PSRAM rollback response failed\n");
        }

      openvela_ready_abort(ret);
      return ret;
    }
#endif

  ret = openvela_ready_commit();
  if (ret < 0)
    {
      printf("PWC: OpenVela ready commit failed, error=%d\n", ret);
#ifdef CONFIG_BK7258_PSRAM
      if (psram_power_set(PM_POWER_MODULE_STATE_OFF) < 0)
        {
          printf("PWC: PSRAM rollback response failed\n");
        }
#endif
      openvela_ready_abort(ret);
      return ret;
    }

  printf("PWC: OpenVela ready committed\n");
  return OK;
}
