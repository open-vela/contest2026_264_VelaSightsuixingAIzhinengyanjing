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
#include "irq.h"

#define MBOX_REG(n)             (BK7258_MBOX0_BASE + ((n) * 4u))
#define MBOX_CH1_CFG            MBOX_REG(0x20)
#define MBOX_CH1_FIFO_CFG       MBOX_REG(0x21)
#define MBOX_CH1_TDATA0         MBOX_REG(0x22)
#define MBOX_CH1_TDATA1         MBOX_REG(0x23)
#define MBOX_CH1_TID            MBOX_REG(0x24)
#define MBOX_CH1_SID            MBOX_REG(0x25)
#define MBOX_CH1_RDATA0         MBOX_REG(0x26)
#define MBOX_CH1_RDATA1         MBOX_REG(0x27)
#define MBOX_CH1_STATUS         MBOX_REG(0x28)
#define MBOX_CH0_STATUS         MBOX_REG(0x18)
#define MBOX_CH2_STATUS         MBOX_REG(0x38)
#define MBOX_CTRL               MBOX_REG(2)

#define MBOX_CFG_INT_EN         (1u << 8)
#define MBOX_CFG_WRERR_EN       (1u << 9)
#define MBOX_CFG_RDERR_EN       (1u << 10)
#define MBOX_CFG_WRFULL_EN      (1u << 11)
#define MBOX_CFG_WRERR_STATUS   (1u << 16)
#define MBOX_CFG_RDERR_STATUS   (1u << 17)
#define MBOX_CFG_WRFULL_STATUS  (1u << 18)
#define MBOX_CFG_ERROR_STATUS   (MBOX_CFG_WRERR_STATUS | \
                                 MBOX_CFG_RDERR_STATUS | \
                                 MBOX_CFG_WRFULL_STATUS)
#define MBOX_CFG_RW_MASK        0x00000fffu

#define MBOX_RX_DESC_COUNT      (2u * 3u + 1u)

struct mbox_rx_descriptor
{
  struct bk7258_mb_wire_message message;
  uint32_t order;
  bool used;
};

static bk7258_mbox_callback_t g_callback;
static struct mbox_rx_descriptor g_rx_desc[MBOX_RX_DESC_COUNT];
static uint32_t g_rx_order;
static bool g_mbox_ready;
static struct bk7258_mbox_stats g_stats;

_Static_assert(MBOX_RX_DESC_COUNT > 2u * 3u,
               "RX descriptors must hold a deferred command and two FIFOs");

uint32_t bk7258_mbox_rx_status(void)
{
  return getreg32(MBOX_CH1_STATUS);
}

static void mbox_clear_errors(void)
{
  uint32_t config = getreg32(MBOX_CH1_CFG);
  uint32_t errors = config & MBOX_CFG_ERROR_STATUS;

  if ((errors & MBOX_CFG_WRERR_STATUS) != 0)
    {
      g_stats.write_error++;
    }

  if ((errors & MBOX_CFG_RDERR_STATUS) != 0)
    {
      g_stats.read_error++;
    }

  if ((errors & MBOX_CFG_WRFULL_STATUS) != 0)
    {
      g_stats.write_full++;
    }

  if (errors != 0)
    {
      putreg32((config & MBOX_CFG_RW_MASK) | errors, MBOX_CH1_CFG);
    }
}

static bool mbox_valid_envelope(uint8_t source, uint32_t address,
                                uint32_t length)
{
  if (source != 0)
    {
      g_stats.bad_source++;
      return false;
    }

  if (length != BK7258_MB_MESSAGE_SIZE)
    {
      g_stats.bad_length++;
      return false;
    }

  if ((address & 3u) != 0 || address < BK7258_CP_RAM_START ||
      address > BK7258_CP_RAM_END - BK7258_MB_MESSAGE_SIZE)
    {
      g_stats.bad_address++;
      return false;
    }

  return true;
}

int bk7258_mbox_send(uint8_t destination, const uint32_t data[2])
{
  uintptr_t status;

  if (!g_mbox_ready || destination > 2 || data == NULL)
    {
      return -EINVAL;
    }

  /* CPU1 transmits through channel 1.  Full is reported by the target's
   * receive FIFO status register.
   */

  status = destination == 0 ? MBOX_CH0_STATUS :
           destination == 1 ? MBOX_CH1_STATUS : MBOX_CH2_STATUS;
  if ((getreg32(status) & 1u) != 0)
    {
      return -EAGAIN;
    }

  putreg32(data[0], MBOX_CH1_TDATA0);
  putreg32(data[1], MBOX_CH1_TDATA1);
  putreg32(destination, MBOX_CH1_TID);
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
      ret = g_callback == NULL ? -ENOSYS : g_callback(&desc->message);
      if (ret == -EAGAIN)
        {
          g_stats.descriptor_deferred++;
          break;
        }

      desc->used = false;
    }

  while ((desc = mbox_oldest_desc(false)) != NULL)
    {
      ret = g_callback == NULL ? -ENOSYS : g_callback(&desc->message);
      if (ret == -EAGAIN)
        {
          break;
        }

      desc->used = false;
    }
}

static int bk7258_mbox_irq(int irq, void *context, void *arg)
{
  (void)irq;
  (void)context;
  (void)arg;

  mbox_clear_errors();

  while ((bk7258_mbox_rx_status() & 2u) == 0)
    {
      struct mbox_rx_descriptor *desc;
      uint32_t address;
      uint32_t length;
      uint8_t source;

      mbox_process_desc();
      desc = mbox_alloc_desc();
      if (desc == NULL)
        {
          g_stats.descriptor_full++;
          break;
        }

      source = getreg32(MBOX_CH1_SID) & 0x0fu;
      address = getreg32(MBOX_CH1_RDATA0);
      length = getreg32(MBOX_CH1_RDATA1);
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
      mbox_process_desc();
    }

  mbox_process_desc();

  return OK;
}

void bk7258_mbox_set_callback(bk7258_mbox_callback_t callback)
{
  g_callback = callback;
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
  memset(&g_stats, 0, sizeof(g_stats));

  /* CPU1 owns channel 1 and FIFO entries 2..4.  Install the callback and
   * descriptors before enabling the route; pre-existing FIFO entries are
   * then processed normally instead of being blindly discarded.
   */

  putreg32(2u | MBOX_CFG_INT_EN | MBOX_CFG_WRERR_EN |
           MBOX_CFG_RDERR_EN | MBOX_CFG_WRFULL_EN, MBOX_CH1_CFG);
  putreg32(1u | (3u << 1), MBOX_CH1_FIFO_CFG);
  modifyreg32(MBOX_CTRL, 0, 1u << 2);
  mbox_clear_errors();

  ret = irq_attach(BK7258_IRQ_MAILBOX, bk7258_mbox_irq, NULL);
  if (ret < 0)
    {
      return ret;
    }

  g_mbox_ready = true;
  modifyreg32(BK7258_CPU1_IRQ_EN1, 0, 1u << 31);
  up_enable_irq(BK7258_IRQ_MAILBOX);
  return OK;
}
