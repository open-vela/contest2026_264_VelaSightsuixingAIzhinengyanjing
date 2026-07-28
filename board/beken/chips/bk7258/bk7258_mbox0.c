/****************************************************************************
 * BK7258 MBOX0 v2 transport.
 ****************************************************************************/

#include <nuttx/config.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include <nuttx/irq.h>
#include <nuttx/arch.h>
#include <sys/param.h>
#include "arm_internal.h"

#include "hardware/bk7258_mbox.h"
#include "hardware/bk7258_sysctrl.h"
#include "irq.h"

#define MBOX_REG(n) (BK7258_MBOX0_BASE + ((n) * 4u))
#define MBOX_CH1_CFG MBOX_REG(0x20)
#define MBOX_CH1_FIFO_CFG MBOX_REG(0x21)
#define MBOX_CH1_TDATA0 MBOX_REG(0x22)
#define MBOX_CH1_TDATA1 MBOX_REG(0x23)
#define MBOX_CH1_TID MBOX_REG(0x24)
#define MBOX_CH1_SID MBOX_REG(0x25)
#define MBOX_CH1_RDATA0 MBOX_REG(0x26)
#define MBOX_CH1_RDATA1 MBOX_REG(0x27)
#define MBOX_CH1_STATUS MBOX_REG(0x28)
#define MBOX_CH0_STATUS MBOX_REG(0x18)
#define MBOX_CTRL MBOX_REG(2)
#define MBOX_INT_STATUS MBOX_REG(3)

static bk7258_mbox_callback_t g_callback;
static bool g_mbox_ready;

uint32_t bk7258_mbox_rx_status(void)
{
  return getreg32(MBOX_CH1_STATUS);
}

int bk7258_mbox_receive(bk7258_mbox_message_t *message)
{
  if (message == NULL || (bk7258_mbox_rx_status() & 2u) != 0)
    {
      return -EINVAL;
    }

  message->src_cpu = getreg32(MBOX_CH1_SID) & 0xfu;
  message->data[0] = getreg32(MBOX_CH1_RDATA0);
  message->data[1] = getreg32(MBOX_CH1_RDATA1);
  return OK;
}

int bk7258_mbox_send(uint8_t destination, const uint32_t data[2])
{
  /* TX uses the CPU1-owned channel 1 registers, while the destination FIFO
   * full state is reported by the destination channel (CPU0 is channel 0). */
  if (destination > 2 || data == NULL ||
      (getreg32(destination == 0 ? MBOX_CH0_STATUS : MBOX_CH1_STATUS) & 1u) != 0)
    {
      return -EINVAL;
    }

  putreg32(data[0], MBOX_CH1_TDATA0);
  putreg32(data[1], MBOX_CH1_TDATA1);
  putreg32(destination, MBOX_CH1_TID);
  return OK;
}

int bk7258_mbox_send_message(uint8_t command, uint8_t logical_channel,
                             uint32_t param1, uint32_t param2,
                             uint32_t param3)
{
  static uint32_t message[4] __attribute__((aligned(32)));
  static uint8_t tx_seq;
  uint32_t wire[2];

  if (!g_mbox_ready)
    {
      return -EAGAIN;
    }

  message[0] = (uint32_t)command | ((uint32_t)++tx_seq << 16) |
               ((uint32_t)logical_channel << 24);
  message[1] = param1;
  message[2] = param2;
  message[3] = param3;
  wire[0] = (uint32_t)(uintptr_t)message;
  wire[1] = sizeof(message);
  __asm__ volatile("dmb sy" ::: "memory");
  return bk7258_mbox_send(0, wire);
}

int bk7258_mbox_uart_write(const uint8_t *data, uint16_t length)
{
  static uint8_t payload[4][40] __attribute__((aligned(32)));
  static uint32_t message[4][4] __attribute__((aligned(32)));
  static uint8_t pending[40];
  static uint8_t pending_length;
  static uint8_t tx_seq;
  uint32_t wire[2];
  irqstate_t flags;
  int ret = OK;
  uint8_t slot;

  if (data == NULL || length == 0 || !g_mbox_ready)
    {
      return -EINVAL;
    }

  flags = up_irq_save();
  while (length != 0)
    {
      pending[pending_length++] = *data++;
      length--;
      if (pending_length != sizeof(pending) &&
          pending[pending_length - 1] != '\n' &&
          pending[pending_length - 1] != '>')
        {
          continue;
        }

      slot = tx_seq++ & 3u;
      memcpy(payload[slot], pending, pending_length);
      message[slot][0] = (uint32_t)tx_seq << 16 | (0x19u << 24);
      message[slot][1] = (uint32_t)(uintptr_t)payload[slot];
      message[slot][2] = pending_length;
      message[slot][3] = 0;
      wire[0] = (uint32_t)(uintptr_t)message[slot];
      wire[1] = sizeof(message[slot]);
      __asm__ volatile("dmb sy" ::: "memory");
      ret = bk7258_mbox_send(0, wire);
      if (ret == OK)
        {
          pending_length = 0;
        }
      else
        {
          pending_length = 0;
          break;
        }
    }

  up_irq_restore(flags);
  return ret;
}

static int bk7258_mbox_irq(int irq, void *context, void *arg)
{
  bk7258_mbox_message_t message;
  (void)irq;
  (void)context;
  (void)arg;

  while ((bk7258_mbox_rx_status() & 2u) == 0)
    {
      if (bk7258_mbox_receive(&message) != OK)
        {
          break;
        }
      if (g_callback != NULL)
        {
          g_callback(&message);
        }
    }
  g_mbox_ready = true;
  return OK;
}

void bk7258_mbox_set_callback(bk7258_mbox_callback_t callback)
{
  g_callback = callback;
}

int bk7258_mbox_init(void)
{
  /* CPU1 owns channel 1: FIFO starts at 2 and has three entries. */
  /* Channel 1 owns FIFO entries 2..4.  The start and length fields live in
   * separate registers on mailbox v2. */
  putreg32(2u | (1u << 8), MBOX_CH1_CFG);
  putreg32(1u | (3u << 1), MBOX_CH1_FIFO_CFG);
  modifyreg32(MBOX_CTRL, 0, 1u << 2);
  irq_attach(BK7258_IRQ_MAILBOX, bk7258_mbox_irq, NULL);
  modifyreg32(BK7258_CPU1_IRQ_EN1, 0, 1u << 31);
  up_enable_irq(BK7258_IRQ_MAILBOX);
  while ((bk7258_mbox_rx_status() & 2u) == 0)
    {
      bk7258_mbox_message_t ignored;
      if (bk7258_mbox_receive(&ignored) != OK)
        {
          break;
        }
    }
  return OK;
}
