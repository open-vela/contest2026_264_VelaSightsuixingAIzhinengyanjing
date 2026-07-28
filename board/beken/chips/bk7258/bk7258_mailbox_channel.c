/****************************************************************************
 * BK7258 logical mailbox channel.
 ****************************************************************************/

#include <nuttx/config.h>
#include <stdint.h>
#include <string.h>

#include "hardware/bk7258_mbox.h"

#define MB_CHNL_PWC_RX 0x42u
#define MB_CHNL_PWC_TX 0x12u
#define CHNL_CTRL_ACK_BOX 0x01u
#define CHNL_CTRL_SYNC_TX 0x02u
#define CHNL_CTRL_RESET 0x04u
#define CHNL_STATE_COM_FAIL 0x01u

struct mb_header
{
  uint32_t data;
};

struct mb_message
{
  struct mb_header header;
  uint32_t param1;
  uint32_t param2;
  uint32_t param3;
};

static struct mb_message g_tx[4] __attribute__((aligned(32)));
static uint8_t g_tx_seq;
static void (*g_rx)(const struct mb_message *message);

static void mailbox_rx(const bk7258_mbox_message_t *wire)
{
  struct mb_message message;
  uint32_t ack[2];
  uintptr_t address = wire->data[0];

  if (wire->src_cpu != 0 || wire->data[1] != sizeof(message) ||
      (address & 31u) != 0 || address < 0x28010000u ||
      address + sizeof(message) > 0x28064000u)
    {
      return;
    }
  memcpy(&message, (const void *)address, sizeof(message));
  if (((message.header.data >> 24) & 0xffu) != MB_CHNL_PWC_RX)
    {
      return;
    }

  /* A command must be transport-ACKed before its semantic work is queued. */
  if (((message.header.data >> 12) & 0xfu) == 0)
    {
      message.header.data |= (uint32_t)CHNL_CTRL_ACK_BOX << 12;
      memcpy((void *)address, &message, sizeof(message));
      ack[0] = (uint32_t)address;
      ack[1] = sizeof(message);
      __asm__ volatile("dmb sy" ::: "memory");
      (void)bk7258_mbox_send(0, ack);
      message.header.data &= ~((uint32_t)CHNL_CTRL_ACK_BOX << 12);
    }
  else
    {
      return;
    }
  if (g_rx != NULL)
    {
      g_rx(&message);
    }
}

void bk7258_mailbox_set_pwc_rx(void (*callback)(const void *message))
{
  g_rx = (void (*)(const struct mb_message *))callback;
}

int bk7258_mailbox_init(void)
{
  g_rx = NULL;
  return bk7258_mbox_init(), bk7258_mbox_set_callback(mailbox_rx), 0;
}

int bk7258_mailbox_send_pwc(uint8_t command, uint32_t p1, uint32_t p2,
                            uint32_t p3)
{
  uint32_t wire[2];
  struct mb_message *message = &g_tx[g_tx_seq & 3u];
  message->header.data = (uint32_t)command |
                         ((uint32_t)++g_tx_seq << 16) |
                         ((uint32_t)MB_CHNL_PWC_TX << 24);
  message->param1 = p1;
  message->param2 = p2;
  message->param3 = p3;
  wire[0] = (uint32_t)(uintptr_t)message;
  wire[1] = sizeof(*message);
  __asm__ volatile("dmb sy" ::: "memory");
  return bk7258_mbox_send(0, wire);
}
