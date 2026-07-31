/****************************************************************************
 * BK7258 mailbox v2 logical channels used by AP bring-up and logging.
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <nuttx/clock.h>
#include <nuttx/irq.h>
#include <nuttx/kthread.h>
#include <nuttx/semaphore.h>

#include "hardware/bk7258_mbox.h"

#define MB_CHNL_HW_CTRL_TX 0x10u
#define MB_CHNL_PWC_TX     0x12u
#define MB_CHNL_UART0_TX   0x19u
#define MB_CHNL_PWC_RX     0x42u
#define MB_CHNL_UART0_RX   0x49u

#define MB_UART_SEND_DATA  0u
#define MB_UART_SEND_STATE 1u

#define CHNL_CTRL_ACK_BOX  0x01u
#define CHNL_CTRL_SYNC_TX  0x02u
#define CHNL_CTRL_RESET    0x04u
#define CHNL_STATE_COM_FAIL 0x01u

#define CP_RAM_START       0x28064000u
#define CP_RAM_END         0x2809f700u
#define UART_XCHG_SIZE     128u
#ifndef CONFIG_BK7258_MBOX_LOG_BUFSIZE
#  define CONFIG_BK7258_MBOX_LOG_BUFSIZE 8192u
#endif
#define UART_TX_SIZE       CONFIG_BK7258_MBOX_LOG_BUFSIZE
#define PHY_BUSY_TIMEOUT   MSEC2TICK(200)
#define TX_RETRY_INTERVAL  MSEC2TICK(10)

struct mb_message
{
  uint32_t header;
  uint32_t param1;
  uint32_t param2;
  uint32_t param3;
};

struct logical_channel
{
  struct mb_message message __attribute__((aligned(32)));
  bool pending;
};

struct mailbox_stats
{
  uint32_t tx_count;
  uint32_t rx_count;
  uint32_t fifo_full;
  uint32_t ack_timeout;
  uint32_t bad_envelope;
  uint32_t bad_ack;
  uint32_t reset_count;
  uint32_t uart_drop;
  uint32_t uart_drop_events;
  uint32_t uart_tx_fail;
  uint32_t uart_write_calls;
  uint32_t uart_write_bytes;
};

static struct logical_channel g_channels[3];
static const uint8_t g_channel_ids[3] =
{
  MB_CHNL_PWC_TX,
  MB_CHNL_HW_CTRL_TX,
  MB_CHNL_UART0_TX
};
static uint8_t g_uart_tx[UART_TX_SIZE];
static uint8_t g_uart_xchg[UART_XCHG_SIZE] __attribute__((aligned(32)));
static struct mb_message g_ack_message __attribute__((aligned(32)));
static uint32_t g_ack_wire[2];
static uint16_t g_uart_read;
static uint16_t g_uart_write;
static uint16_t g_uart_inflight;
static uint8_t g_tx_seq;
static uint8_t g_active_channel;
static uint8_t g_active_seq;
static bool g_phy_busy;
static bool g_initialized;
static bool g_serial_kick;
static clock_t g_busy_since;
static clock_t g_uart_retry_after;
static struct mailbox_stats g_stats;
static sem_t g_tx_sem;
static void (*g_pwc_rx)(const struct mb_message *message);

void bk7258_mbox_uart_early_init(void)
{
  /* The BSS-backed ring is intentionally available before Mailbox init. */
}

extern void bk7258_serial_tx_available(void);

static unsigned int channel_index(uint8_t logical_channel)
{
  unsigned int i;

  for (i = 0; i < sizeof(g_channel_ids); i++)
    {
      if (g_channel_ids[i] == logical_channel)
        {
          return i;
        }
    }

  return sizeof(g_channel_ids);
}

static uint16_t uart_count(void)
{
  if (g_uart_write >= g_uart_read)
    {
      return g_uart_write - g_uart_read;
    }

  return UART_TX_SIZE - g_uart_read + g_uart_write;
}

static void uart_prepare(void)
{
  struct logical_channel *channel = &g_channels[2];
  uint16_t count;
  uint16_t first;

  if (channel->pending || g_uart_inflight != 0)
    {
      return;
    }

  if ((int32_t)(clock_systime_ticks() - g_uart_retry_after) < 0)
    {
      return;
    }

  count = uart_count();
  if (count == 0)
    {
      return;
    }

  if (count > UART_XCHG_SIZE)
    {
      count = UART_XCHG_SIZE;
    }

  first = UART_TX_SIZE - g_uart_read;
  if (first > count)
    {
      first = count;
    }

  memcpy(g_uart_xchg, &g_uart_tx[g_uart_read], first);
  if (first != count)
    {
      memcpy(&g_uart_xchg[first], g_uart_tx, count - first);
    }

  channel->message.header = 0; /* MB_UART_SEND_DATA */
  channel->message.param1 = (uint32_t)(uintptr_t)g_uart_xchg;
  channel->message.param2 = count;
  channel->message.param3 = 0; /* RTS deasserted, CRC disabled by Armino. */
  channel->pending = true;
  g_uart_inflight = count;
}

static int dispatch_locked(void)
{
  struct logical_channel *channel;
  uint32_t wire[2];
  clock_t now;
  unsigned int i;
  int ret;

  now = clock_systime_ticks();
  if (g_phy_busy && now - g_busy_since >= PHY_BUSY_TIMEOUT)
    {
      g_stats.ack_timeout++;
      g_phy_busy = false;
    }

  if (g_phy_busy)
    {
      return OK;
    }

  if (g_ack_wire[0] != 0)
    {
      ret = bk7258_mbox_send(0, g_ack_wire);
      if (ret < 0)
        {
          if (ret == -EAGAIN)
            {
              g_stats.fifo_full++;
            }

          return ret;
        }

      g_ack_wire[0] = 0;
      g_stats.tx_count++;
    }

  uart_prepare();
  for (i = 0; i < sizeof(g_channel_ids); i++)
    {
      channel = &g_channels[i];
      if (!channel->pending)
        {
          continue;
        }

      g_active_channel = g_channel_ids[i];
      g_active_seq = ++g_tx_seq;
      channel->message.header &= 0xffu;
      channel->message.header |= (uint32_t)g_active_seq << 16;
      channel->message.header |= (uint32_t)g_active_channel << 24;
      wire[0] = (uint32_t)(uintptr_t)&channel->message;
      wire[1] = sizeof(channel->message);
      __asm__ volatile("dmb sy" ::: "memory");
      ret = bk7258_mbox_send(0, wire);
      if (ret < 0)
        {
          if (ret == -EAGAIN)
            {
              g_stats.fifo_full++;
            }

          return ret;
        }

      g_phy_busy = true;
      g_busy_since = now;
      g_stats.tx_count++;
      return OK;
    }

  return OK;
}

static int mailbox_tx_worker(int argc, char **argv)
{
  irqstate_t flags;

  (void)argc;
  (void)argv;
  for (;;)
    {
      (void)nxsem_tickwait_uninterruptible(&g_tx_sem, TX_RETRY_INTERVAL);

      flags = up_irq_save();
      if (g_serial_kick)
        {
          g_serial_kick = false;
          up_irq_restore(flags);
          bk7258_serial_tx_available();
          flags = up_irq_save();
        }

      (void)dispatch_locked();
      up_irq_restore(flags);
    }

  return OK;
}

static int queue_message(uint8_t logical_channel, uint8_t command,
                         uint32_t p1, uint32_t p2, uint32_t p3)
{
  struct logical_channel *channel;
  irqstate_t flags;
  unsigned int index;
  int ret;

  if (!g_initialized)
    {
      return -EAGAIN;
    }

  index = channel_index(logical_channel);
  if (index >= sizeof(g_channel_ids))
    {
      return -EINVAL;
    }

  flags = up_irq_save();
  channel = &g_channels[index];
  if (channel->pending)
    {
      up_irq_restore(flags);
      return -EBUSY;
    }

  channel->message.header = command;
  channel->message.param1 = p1;
  channel->message.param2 = p2;
  channel->message.param3 = p3;
  channel->pending = true;
  ret = dispatch_locked();
  up_irq_restore(flags);
  nxsem_post(&g_tx_sem);
  return ret == -EAGAIN ? OK : ret;
}

static bool address_in_cp_ram(uintptr_t address, uint32_t length)
{
  return (address & 3u) == 0 && address >= CP_RAM_START &&
         length > 0 && length <= CP_RAM_END - CP_RAM_START &&
         address <= CP_RAM_END - length;
}

static bool valid_cp_message(const bk7258_mbox_message_t *wire,
                             uintptr_t *address)
{
  *address = wire->data[0];
  /* Armino's mbox0_adapter mailbox_buff is an array of 16-byte messages in
   * CP .bss and is only word-aligned; 32-byte alignment is reserved for the
   * separate exchange payload buffers. */
  return wire->src_cpu == 0 && wire->data[1] == sizeof(struct mb_message) &&
         address_in_cp_ram(*address, sizeof(struct mb_message));
}

static void handle_ack(struct mb_message *message)
{
  uint8_t logical_channel = message->header >> 24;
  uint8_t sequence = message->header >> 16;
  bool uart_failed;
  unsigned int index;

  if ((message->header & ((uint32_t)CHNL_CTRL_RESET << 12)) != 0)
    {
      g_stats.reset_count++;
      g_phy_busy = false;
      (void)dispatch_locked();
      return;
    }

  if (logical_channel != g_active_channel || sequence != g_active_seq)
    {
      /* An ACK from a timed-out transmission may arrive after the same
       * logical channel has already been retried with a new sequence. It is
       * stale and must not release the current in-flight transaction. */
      g_stats.bad_ack++;
      return;
    }

  index = channel_index(g_active_channel);
  if (index >= sizeof(g_channel_ids) || !g_channels[index].pending)
    {
      g_stats.bad_ack++;
      g_phy_busy = false;
      (void)dispatch_locked();
      return;
    }

  g_channels[index].pending = false;
  if (g_active_channel == MB_CHNL_UART0_TX)
    {
      uart_failed =
        (message->header & ((uint32_t)CHNL_STATE_COM_FAIL << 8)) != 0 ||
        (message->param1 & 2u) != 0;
      if (uart_failed)
        {
          g_stats.uart_tx_fail++;
          g_uart_inflight = 0;
          g_uart_retry_after = clock_systime_ticks() + TX_RETRY_INTERVAL;
        }
      else
        {
          g_uart_read = (g_uart_read + g_uart_inflight) % UART_TX_SIZE;
          g_uart_inflight = 0;
          g_uart_retry_after = 0;
          g_serial_kick = true;
        }

      nxsem_post(&g_tx_sem);
    }

  g_phy_busy = false;
  uart_prepare();
  (void)dispatch_locked();
}

static void mailbox_rx(const bk7258_mbox_message_t *wire)
{
  struct mb_message message;
  uintptr_t address;
  uint8_t control;
  uint8_t logical_channel;
  irqstate_t flags;

  if (!valid_cp_message(wire, &address))
    {
      g_stats.bad_envelope++;
      return;
    }

  memcpy(&message, (const void *)address, sizeof(message));
  control = (message.header >> 12) & 0x0fu;
  logical_channel = message.header >> 24;
  g_stats.rx_count++;

  flags = up_irq_save();
  if ((control & CHNL_CTRL_ACK_BOX) != 0)
    {
      handle_ack(&message);
      up_irq_restore(flags);
      return;
    }

  if (logical_channel != MB_CHNL_PWC_RX &&
      logical_channel != MB_CHNL_UART0_RX)
    {
      g_stats.bad_envelope++;
      up_irq_restore(flags);
      return;
    }

  if (logical_channel == MB_CHNL_UART0_RX)
    {
      /* CP forwards raw keypresses from the physical UART0 CLI input path
       * (bk_avdk_smp cp/components/bk_cli/shell_task.c's
       * ap_uart0_rx_forward()) using the same address+length envelope
       * convention as the existing AP->CP UART0 TX direction: param1 is the
       * address of a CP-RAM buffer holding the raw bytes, param2 is the
       * byte count. Armino also initializes MB_UART0 by sending SEND_STATE
       * from CP with no payload; both cases are acknowledged with RTS
       * deasserted so the CP endpoint reaches ready. */
      if ((message.header & 0xffu) == MB_UART_SEND_DATA &&
          message.param1 != 0 && message.param2 != 0 &&
          address_in_cp_ram(message.param1, message.param2) &&
          message.param2 <= UART_XCHG_SIZE)
        {
          extern void bk7258_serial_rx_push(const uint8_t *data,
                                             uint16_t length);
          bk7258_serial_rx_push((const uint8_t *)message.param1,
                                 (uint16_t)message.param2);
        }
      else if ((message.header & 0xffu) != MB_UART_SEND_DATA &&
               (message.header & 0xffu) != MB_UART_SEND_STATE)
        {
          message.header |= (uint32_t)CHNL_STATE_COM_FAIL << 8;
        }

      message.param1 = 0; /* uart_rts=0, uart_tx_fail=0 */
      message.param2 = 0;
      message.param3 = 0;
    }

  if ((control & CHNL_CTRL_SYNC_TX) == 0)
    {
      message.header |= (uint32_t)CHNL_CTRL_ACK_BOX << 12;
      memcpy((void *)address, &message, sizeof(message));
      memcpy(&g_ack_message, &message, sizeof(g_ack_message));
      g_ack_wire[0] = (uint32_t)(uintptr_t)&g_ack_message;
      g_ack_wire[1] = sizeof(g_ack_message);
      __asm__ volatile("dmb sy" ::: "memory");
      if (bk7258_mbox_send(0, g_ack_wire) < 0)
        {
          g_stats.fifo_full++;
          nxsem_post(&g_tx_sem);
        }
      else
        {
          g_ack_wire[0] = 0;
          g_stats.tx_count++;
        }

      message.header &= ~((uint32_t)CHNL_CTRL_ACK_BOX << 12);
    }

  if (logical_channel == MB_CHNL_PWC_RX && g_pwc_rx != NULL)
    {
      g_pwc_rx(&message);
    }

  up_irq_restore(flags);
}

void bk7258_mailbox_set_pwc_rx(void (*callback)(const void *message))
{
  g_pwc_rx = (void (*)(const struct mb_message *))callback;
}

int bk7258_mailbox_init(void)
{
  int ret;

  memset(g_channels, 0, sizeof(g_channels));
  /* Preserve syslog data queued before the Mailbox transport was ready. */
  g_ack_wire[0] = 0;
  g_serial_kick = false;
  g_phy_busy = false;
  g_uart_retry_after = 0;
  g_pwc_rx = NULL;
  g_initialized = false;

  nxsem_init(&g_tx_sem, 0, 0);
  bk7258_mbox_set_callback(mailbox_rx);

  ret = bk7258_mbox_init();
  if (ret < 0)
    {
      return ret;
    }

  ret = kthread_create("mbox-tx", 105, 1536, mailbox_tx_worker, NULL);
  if (ret < 0)
    {
      return ret;
    }

  g_initialized = true;
  g_serial_kick = true;
  nxsem_post(&g_tx_sem);
  return OK;
}

int bk7258_mbox_send_message(uint8_t command, uint8_t logical_channel,
                             uint32_t param1, uint32_t param2,
                             uint32_t param3)
{
  return queue_message(logical_channel, command, param1, param2, param3);
}

int bk7258_mailbox_send_pwc(uint8_t command, uint32_t p1, uint32_t p2,
                            uint32_t p3)
{
  return queue_message(MB_CHNL_PWC_TX, command, p1, p2, p3);
}

int bk7258_mbox_uart_write(const uint8_t *data, uint16_t length)
{
  irqstate_t flags;
  uint16_t written = 0;

  if (data == NULL || length == 0)
    {
      return -EINVAL;
    }

  flags = up_irq_save();
  {
    uint16_t free_space;
    uint16_t first;

    if (g_uart_write >= g_uart_read)
      {
        free_space = UART_TX_SIZE - (g_uart_write - g_uart_read) - 1u;
      }
    else
      {
        free_space = g_uart_read - g_uart_write - 1u;
      }

    if (length < free_space)
      {
        written = length;
      }
    else
      {
        written = free_space;
      }

    first = UART_TX_SIZE - g_uart_write;
    if (first > written)
      {
        first = written;
      }

    memcpy(&g_uart_tx[g_uart_write], data, first);
    if (first < written)
      {
        memcpy(g_uart_tx, data + first, written - first);
      }

    g_uart_write = (g_uart_write + written) % UART_TX_SIZE;
    g_stats.uart_write_calls++;
    g_stats.uart_write_bytes += written;

    if (written < length)
      {
        g_stats.uart_drop += length - written;
        g_stats.uart_drop_events++;
      }
  }

  if (g_initialized)
    {
      uart_prepare();
      (void)dispatch_locked();
    }
  up_irq_restore(flags);
  if (g_initialized)
    {
      nxsem_post(&g_tx_sem);
    }
  return written == length ? OK : -ENOSPC;
}

bool bk7258_mbox_uart_txready(void)
{
  irqstate_t flags;
  bool ready;

  flags = up_irq_save();
  ready = g_initialized && (g_uart_write + 1u) % UART_TX_SIZE != g_uart_read;
  up_irq_restore(flags);
  return ready;
}

bool bk7258_mbox_uart_txempty(void)
{
  irqstate_t flags;
  bool empty;

  flags = up_irq_save();
  empty = g_initialized && g_uart_read == g_uart_write &&
          g_uart_inflight == 0;
  up_irq_restore(flags);
  return empty;
}
