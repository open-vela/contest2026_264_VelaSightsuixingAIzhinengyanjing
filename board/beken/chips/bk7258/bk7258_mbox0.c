/****************************************************************************
 * BK7258 MBOX0 v2 physical transport.
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <nuttx/arch.h>
#include <nuttx/irq.h>

#include "arm_internal.h"
#include "hardware/bk7258_mbox.h"
#include "hardware/bk7258_sysctrl.h"
#include "bk7258_boottrace.h"
#include "irq.h"

#define MBOX_RX_DESC_COUNT      (2u * 3u + 1u)
#define MBOX_IRQ_DRAIN_BUDGET   8u

struct mbox_rx_descriptor
{
  struct bk7258_mb_wire_message message;
  uint32_t order;
  bool used;
};

static bk7258_mbox_callback_t g_callback;
static struct mbox_rx_descriptor g_rx_desc[MBOX_RX_DESC_COUNT];
static uint32_t g_rx_order;
static uint32_t g_processing_order;
static bool g_mbox_ready;
static bool g_mbox_smp_ready;
static struct bk7258_mbox_stats g_stats;

static void mbox_trace_errors(void)
{
  bk7258_boottrace_detail(4,
    (g_stats.bad_source & 0xffu) |
    ((g_stats.bad_length & 0xffu) << 8) |
    ((g_stats.bad_address & 0xffu) << 16) |
    ((g_stats.descriptor_full & 0xffu) << 24));
}

_Static_assert(MBOX_RX_DESC_COUNT > 2u * 3u,
               "RX descriptors must hold a deferred command and two FIFOs");

uint32_t bk7258_mbox_rx_status(void)
{
  return getreg32(up_cpu_index() == 0 ? BK7258_MBOX_CH1_STATUS :
                                       BK7258_MBOX_CH2_STATUS);
}

static void mbox_clear_errors(int cpu)
{
  uintptr_t cfg = cpu == 0 ? BK7258_MBOX_CH1_CFG : BK7258_MBOX_CH2_CFG;
  uint32_t config = getreg32(cfg);
  uint32_t errors = config & BK7258_MBOX_CFG_ERROR_STATUS;

  if (cpu == 0 && (errors & BK7258_MBOX_CFG_WRERR_STATUS) != 0)
    {
      g_stats.write_error++;
    }

  if (cpu == 0 && (errors & BK7258_MBOX_CFG_RDERR_STATUS) != 0)
    {
      g_stats.read_error++;
    }

  if (cpu == 0 && (errors & BK7258_MBOX_CFG_WRFULL_STATUS) != 0)
    {
      g_stats.write_full++;
    }

  if (errors != 0)
    {
      putreg32((config & BK7258_MBOX_CFG_RW_MASK) | errors, cfg);
    }
}

static bool mbox_valid_envelope(uint8_t source, uint32_t address,
                                uint32_t length)
{
  if (source != 0)
    {
      g_stats.bad_source++;
      mbox_trace_errors();
      return false;
    }

  if (length != BK7258_MB_MESSAGE_SIZE)
    {
      g_stats.bad_length++;
      mbox_trace_errors();
      return false;
    }

  if ((address & 3u) != 0 || address < BK7258_CP_RAM_START ||
      address > BK7258_CP_RAM_END - BK7258_MB_MESSAGE_SIZE)
    {
      g_stats.bad_address++;
      mbox_trace_errors();
      return false;
    }

  return true;
}

int bk7258_mbox_send(uint8_t destination, const uint32_t data[2])
{
  irqstate_t flags;
  uintptr_t status;
  int cpu = up_cpu_index();

  if (!g_mbox_ready || destination > 2 || data == NULL)
    {
      return -EINVAL;
    }

#ifdef CONFIG_SMP
  if (cpu == 1 && (destination != 1 || data[1] != 0))
    {
      return -EPERM;
    }
#endif

  /* CPU1 transmits through channel 1.  Full is reported by the target's
   * receive FIFO status register.
   */

  status = destination == 0 ? BK7258_MBOX_CH0_STATUS :
            destination == 1 ? BK7258_MBOX_CH1_STATUS :
                               BK7258_MBOX_CH2_STATUS;
  flags = up_irq_save();
  if ((getreg32(status) & 1u) != 0)
    {
      up_irq_restore(flags);
      return -EAGAIN;
    }

  /* TDATA0/TDATA1 are staging registers and TID commits the descriptor.
   * Keep raw SMP kicks and transport sends from interleaving these writes.
   */

  putreg32(data[0], cpu == 0 ? BK7258_MBOX_CH1_TDATA0 :
                               BK7258_MBOX_CH2_TDATA0);
  putreg32(data[1], cpu == 0 ? BK7258_MBOX_CH1_TDATA1 :
                               BK7258_MBOX_CH2_TDATA1);
  putreg32(destination, cpu == 0 ? BK7258_MBOX_CH1_TID :
                                   BK7258_MBOX_CH2_TID);
  up_irq_restore(flags);
  return OK;
}

static struct mbox_rx_descriptor *mbox_alloc_desc(void)
{
  unsigned int i;

  for (i = 0; i < MBOX_RX_DESC_COUNT; i++)
    {
      if (!g_rx_desc[i].used)
        {
          return &g_rx_desc[i];
        }
    }

  return NULL;
}

static bool mbox_priority_message(
  const struct bk7258_mb_wire_message *message)
{
  uint8_t control = bk7258_mb_header_ctrl(message);

  return (control & (BK7258_MB_CTRL_ACK_BOX | BK7258_MB_CTRL_RESET)) != 0;
}

static struct mbox_rx_descriptor *mbox_oldest_desc(bool priority)
{
  struct mbox_rx_descriptor *oldest = NULL;
  unsigned int i;

  for (i = 0; i < MBOX_RX_DESC_COUNT; i++)
    {
      struct mbox_rx_descriptor *desc = &g_rx_desc[i];

      if (!desc->used || mbox_priority_message(&desc->message) != priority)
        {
          continue;
        }

      if (oldest == NULL || (int32_t)(desc->order - oldest->order) < 0)
        {
          oldest = desc;
        }
    }

  return oldest;
}

static void mbox_process_desc(void)
{
  struct mbox_rx_descriptor *desc;
  int ret;

  /* ACK and RESET never need ACK slots.  Process them ahead of a deferred
   * ordinary command, then retry ordinary commands in original FIFO order.
   */

  while ((desc = mbox_oldest_desc(true)) != NULL)
    {
      g_processing_order = desc->order;
      ret = g_callback == NULL ? -ENOSYS : g_callback(&desc->message);
      g_processing_order = 0;
      if (ret == -EAGAIN)
        {
          g_stats.descriptor_deferred++;
          break;
        }

      desc->used = false;
    }

  while ((desc = mbox_oldest_desc(false)) != NULL)
    {
      g_processing_order = desc->order;
      ret = g_callback == NULL ? -ENOSYS : g_callback(&desc->message);
      g_processing_order = 0;
      if (ret == -EAGAIN)
        {
          break;
        }

      desc->used = false;
    }
}

static int bk7258_mbox_irq(int irq, void *context, void *arg)
{
  uintptr_t rdata0;
  uintptr_t rdata1;
  uintptr_t sidreg;
  int cpu = up_cpu_index();
  unsigned int budget = MBOX_IRQ_DRAIN_BUDGET;

  (void)irq;
  (void)context;
  (void)arg;

  mbox_clear_errors(cpu);
  if (cpu == 1 && bk7258_boottrace_secondary_stage() >=
                  BK7258_BOOT_SECONDARY_ACK &&
                  bk7258_boottrace_secondary_stage() <
                  BK7258_BOOT_SECONDARY_MBOX_ENTER)
    {
      bk7258_boottrace_secondary(BK7258_BOOT_SECONDARY_MBOX_ENTER);
    }
  sidreg = cpu == 0 ? BK7258_MBOX_CH1_SID : BK7258_MBOX_CH2_SID;
  rdata0 = cpu == 0 ? BK7258_MBOX_CH1_RDATA0 : BK7258_MBOX_CH2_RDATA0;
  rdata1 = cpu == 0 ? BK7258_MBOX_CH1_RDATA1 : BK7258_MBOX_CH2_RDATA1;

  while ((bk7258_mbox_rx_status() & 2u) == 0 && budget-- != 0)
    {
      struct mbox_rx_descriptor *desc;
      uint32_t address;
      uint32_t length;
      uint8_t source;

      if (cpu == 0)
        {
          mbox_process_desc();
        }

      desc = cpu == 0 ? mbox_alloc_desc() : NULL;
      if (cpu == 0 && desc == NULL)
        {
          g_stats.descriptor_full++;
          mbox_trace_errors();
          break;
        }

      source = getreg32(sidreg) & 0x0fu;
      address = getreg32(rdata0);
      length = getreg32(rdata1);
#ifdef CONFIG_SMP
      if (length == 0)
        {
          if (source == (uint8_t)(2 - cpu))
            {
              if (cpu == 1 && address == BK7258_MBOX_RAW_KICK)
                {
                  bk7258_boottrace_secondary(BK7258_BOOT_SECONDARY_MBOX_RAW);
                }

              bk7258_smp_ipi_receive(irq, context, address);
            }
          else
            {
              if (cpu == 0)
                {
                  g_stats.bad_source++;
                  mbox_trace_errors();
                }
            }

          continue;
        }
#endif

      if (cpu != 0)
        {
          continue;
        }

      if (!mbox_valid_envelope(source, address, length))
        {
          continue;
        }

      __asm__ volatile("dmb sy" ::: "memory");
      memcpy(&desc->message, (const void *)(uintptr_t)address,
             sizeof(desc->message));
      desc->order = ++g_rx_order;
      desc->used = true;
      g_stats.rx_messages++;
      bk7258_boottrace_detail(0, desc->message.header);
      bk7258_boottrace_detail(1, address);
      bk7258_boottrace_detail(2, g_stats.rx_messages);
      mbox_process_desc();
    }

  if (cpu == 0)
    {
      mbox_process_desc();
    }

  if (cpu == 1 && bk7258_boottrace_secondary_stage() ==
                  BK7258_BOOT_SECONDARY_MBOX_RAW)
    {
      bk7258_boottrace_secondary(BK7258_BOOT_SECONDARY_MBOX_EXIT);
    }

  return OK;
}

void bk7258_mbox_set_callback(bk7258_mbox_callback_t callback)
{
  g_callback = callback;
}

void bk7258_mbox_kick_rx(void)
{
  irqstate_t flags = up_irq_save();

  (void)bk7258_mbox_irq(0, NULL, NULL);
  up_irq_restore(flags);
}

void bk7258_mbox_discard_deferred(void)
{
  unsigned int i;

  for (i = 0; i < MBOX_RX_DESC_COUNT; i++)
    {
      if (g_rx_desc[i].used && g_processing_order != 0 &&
          (int32_t)(g_rx_desc[i].order - g_processing_order) < 0 &&
          !mbox_priority_message(&g_rx_desc[i].message))
        {
          g_rx_desc[i].used = false;
        }
    }
}

void bk7258_mbox_get_stats(struct bk7258_mbox_stats *stats)
{
  irqstate_t flags;

  if (stats == NULL)
    {
      return;
    }

  flags = up_irq_save();
  *stats = g_stats;
  up_irq_restore(flags);
}

int bk7258_mbox_init(void)
{
  int ret;

  if (g_mbox_ready)
    {
      return OK;
    }

  memset(g_rx_desc, 0, sizeof(g_rx_desc));
  g_rx_order = 0;
  g_processing_order = 0;
  memset(&g_stats, 0, sizeof(g_stats));

  /* CPU1 owns channel 1 and FIFO entries 2..4.  Install the callback and
   * descriptors before enabling the route; pre-existing FIFO entries are
   * then processed normally instead of being blindly discarded.
   */

  putreg32(2u | BK7258_MBOX_CFG_INT_EN | BK7258_MBOX_CFG_WRERR_EN |
            BK7258_MBOX_CFG_RDERR_EN | BK7258_MBOX_CFG_WRFULL_EN,
            BK7258_MBOX_CH1_CFG);
  putreg32(1u | (3u << 1), BK7258_MBOX_CH1_FIFO_CFG);
#ifdef CONFIG_SMP
  putreg32(5u | BK7258_MBOX_CFG_INT_EN | BK7258_MBOX_CFG_WRERR_EN |
            BK7258_MBOX_CFG_RDERR_EN | BK7258_MBOX_CFG_WRFULL_EN,
            BK7258_MBOX_CH2_CFG);
  putreg32(1u | (3u << 1), BK7258_MBOX_CH2_FIFO_CFG);
#endif
  modifyreg32(BK7258_MBOX_CTRL, 0, 1u << 2);
  mbox_clear_errors(0);

  ret = irq_attach(BK7258_IRQ_MAILBOX, bk7258_mbox_irq, NULL);
  if (ret < 0)
    {
      return ret;
    }

  g_mbox_ready = true;
#ifdef CONFIG_SMP
  g_mbox_smp_ready = true;
#endif
  modifyreg32(BK7258_CPU1_IRQ_EN1, 0, 1u << 31);
  up_enable_irq(BK7258_IRQ_MAILBOX);
  return OK;
}

bool bk7258_mbox_smp_ready(void)
{
#ifdef CONFIG_SMP
  return g_mbox_ready && g_mbox_smp_ready;
#else
  return false;
#endif
}

#ifdef CONFIG_SMP
int bk7258_mbox_secondary_init(void)
{
  if (!g_mbox_ready || !g_mbox_smp_ready || up_cpu_index() != 1)
    {
      return -EAGAIN;
    }

  mbox_clear_errors(1);
  modifyreg32(BK7258_CPU2_IRQ_EN1, 0, 1u << 31);
  up_enable_irq(BK7258_IRQ_MAILBOX);
  return OK;
}
#endif
