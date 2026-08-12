/****************************************************************************
 * BK7258 raw Mailbox SMP IPI.
 ****************************************************************************/

#include <nuttx/config.h>

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <strings.h>

#include <nuttx/arch.h>
#include <nuttx/atomic.h>
#include <nuttx/sched.h>

#include "arm_internal.h"
#include "sched/sched.h"
#include "hardware/bk7258_mbox.h"
#include "bk7258_boottrace.h"
#include "bk7258_smp.h"

#define BK7258_IPI_SCHED        (1u << 0)
#define BK7258_IPI_CALL         (1u << 1)
#define BK7258_BOOT_RETRY_LIMIT 1024

#define BK7258_MBOX_BOOT_NOTIFY (BK7258_MBOX_RAW_KICK + 1u)
#define BK7258_MBOX_BOOT_PING   (BK7258_MBOX_RAW_KICK + 2u)
#define BK7258_MBOX_BOOT_ACK    (BK7258_MBOX_RAW_KICK + 3u)

#define BK7258_BOOT_SEEN_NOTIFY (1u << 0)
#define BK7258_BOOT_SEEN_ACK    (1u << 1)

#define BK7258_IPI_TRACE_ENTRY  0xb1000000u
#define BK7258_IPI_TRACE_CLAIM  0xb2000000u
#define BK7258_IPI_TRACE_SENT   0xb3000000u
#define BK7258_IPI_TRACE_FAIL   0xb4000000u

static atomic_t g_ipi_pending[CONFIG_SMP_NCPUS];
static atomic_t g_ipi_kicked[CONFIG_SMP_NCPUS];
static atomic_t g_boot_ping_received;
static atomic_t g_secondary_ready;
static uint32_t g_boot_seen;

static void bk7258_smp_trace(uint32_t value)
{
  if (bk7258_boottrace_primary_stage() == BK7258_BOOT_CPU2_FIRST_IRQ)
    {
      bk7258_boottrace_detail(4, value);
    }
}

static int bk7258_smp_boot_send(int cpu, uint32_t command)
{
  uint32_t data[2] = {command, 0};
  int retry;

  UP_DMB();
  for (retry = 0; retry < BK7258_BOOT_RETRY_LIMIT; retry++)
    {
      int ret = bk7258_mbox_send((uint8_t)(cpu + 1), data);

      if (ret == OK)
        {
          return OK;
        }

      if (ret != -EAGAIN)
        {
          return ret;
        }

      __asm__ volatile("nop");
    }

  return -ETIMEDOUT;
}

static int bk7258_smp_kick(int cpu, uint32_t pending)
{
  uint32_t data[2] = {BK7258_MBOX_RAW_KICK, 0};
  int expected = 0;
  uint32_t retry = 0;

  if ((unsigned int)cpu >= CONFIG_SMP_NCPUS || cpu == this_cpu())
    {
      return -EINVAL;
    }

  bk7258_smp_trace(BK7258_IPI_TRACE_ENTRY | (pending & 0xffu));
  atomic_or_release(&g_ipi_pending[cpu], pending);
  if (!atomic_cmpxchg_acquire(&g_ipi_kicked[cpu], &expected, 1))
    {
      bk7258_smp_trace(BK7258_IPI_TRACE_CLAIM | 0x100u);
      return OK;
    }

  bk7258_smp_trace(BK7258_IPI_TRACE_CLAIM | (cpu & 0xffu));
  UP_DMB();
  for (;;)
    {
      int ret = bk7258_mbox_send((uint8_t)(cpu + 1), data);

      if (ret == OK)
        {
          bk7258_smp_trace(BK7258_IPI_TRACE_SENT | (retry & 0xffffu));
          return OK;
        }

      if (ret != -EAGAIN)
        {
          atomic_set_release(&g_ipi_kicked[cpu], 0);
          bk7258_smp_trace(BK7258_IPI_TRACE_FAIL |
                           ((uint32_t)(-ret) & 0xffffu));
          return ret;
        }

      retry++;
      __asm__ volatile("nop");
    }
}

void bk7258_smp_ipi_receive(int irq, void *context, uint32_t command)
{
  int cpu = this_cpu();

  if (command == BK7258_MBOX_BOOT_NOTIFY)
    {
      if (cpu == 0)
        {
          g_boot_seen |= BK7258_BOOT_SEEN_NOTIFY;
        }

      return;
    }

  if (command == BK7258_MBOX_BOOT_PING)
    {
      if (cpu == 1)
        {
          atomic_set_release(&g_boot_ping_received, 1);
          if (bk7258_boottrace_secondary_stage() <
              BK7258_BOOT_SECONDARY_ACK)
            {
              bk7258_boottrace_secondary(BK7258_BOOT_SECONDARY_ACK);
            }
        }

      return;
    }

  if (command == BK7258_MBOX_BOOT_ACK)
    {
      if (cpu == 0)
        {
          g_boot_seen |= BK7258_BOOT_SEEN_ACK;
        }

      return;
    }

  if (command != BK7258_MBOX_RAW_KICK)
    {
      return;
    }

  for (;;)
    {
      uint32_t pending = atomic_xchg_acquire(&g_ipi_pending[cpu], 0);

      if ((pending & BK7258_IPI_CALL) != 0)
        {
          nxsched_smp_call_handler(irq, context, NULL);
        }

      if ((pending & BK7258_IPI_SCHED) != 0)
        {
          nxsched_process_delivered(cpu);
        }

      atomic_set_release(&g_ipi_kicked[cpu], 0);
      UP_DMB();
      if (atomic_read_acquire(&g_ipi_pending[cpu]) == 0)
        {
          break;
        }

      atomic_set_release(&g_ipi_kicked[cpu], 1);
    }
}

int up_send_smp_sched(int cpu)
{
  int ret = bk7258_smp_kick(cpu, BK7258_IPI_SCHED);

  if (ret < 0)
    {
      return ret;
    }

  return ret;
}

void up_send_smp_call(cpu_set_t cpuset)
{
  int cpu;

  for (; cpuset != 0; cpuset &= ~(1u << cpu))
    {
      cpu = ffs((int)cpuset) - 1;
      if (cpu != this_cpu() && bk7258_smp_kick(cpu, BK7258_IPI_CALL) < 0)
        {
          PANIC();
        }
    }
}

void bk7258_smp_prepare_boot(void)
{
  int cpu;

  for (cpu = 0; cpu < CONFIG_SMP_NCPUS; cpu++)
    {
      atomic_set(&g_ipi_pending[cpu], 0);
      atomic_set(&g_ipi_kicked[cpu], 0);
    }

  g_boot_seen = 0;
  atomic_set(&g_boot_ping_received, 0);
  atomic_set(&g_secondary_ready, 0);
  UP_DMB();
}

void bk7258_smp_secondary_ready(void)
{
  atomic_set_release(&g_secondary_ready, 1);
}

bool bk7258_smp_secondary_is_ready(void)
{
  return atomic_read_acquire(&g_secondary_ready) != 0;
}

void bk7258_smp_boot_notify(void)
{
  if (bk7258_smp_boot_send(0, BK7258_MBOX_BOOT_NOTIFY) < 0)
    {
      PANIC();
    }
}

void bk7258_smp_boot_ack(void)
{
  if (bk7258_smp_boot_send(0, BK7258_MBOX_BOOT_ACK) < 0)
    {
      PANIC();
    }
}

bool bk7258_smp_boot_notified(void)
{
  return (g_boot_seen & BK7258_BOOT_SEEN_NOTIFY) != 0;
}

bool bk7258_smp_boot_ping_received(void)
{
  return atomic_read_acquire(&g_boot_ping_received) != 0;
}

bool bk7258_smp_boot_test_complete(void)
{
  return g_boot_seen == (BK7258_BOOT_SEEN_NOTIFY |
                         BK7258_BOOT_SEEN_ACK);
}

int bk7258_smp_boot_ping(int cpu)
{
  return bk7258_smp_boot_send(cpu, BK7258_MBOX_BOOT_PING);
}
