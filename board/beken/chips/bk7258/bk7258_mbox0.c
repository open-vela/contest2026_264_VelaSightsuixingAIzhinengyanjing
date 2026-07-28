/****************************************************************************
 * BK7258 MBOX0 v2 transport.
 ****************************************************************************/

#include <nuttx/config.h>
#include <stdint.h>

#include <nuttx/irq.h>
#include <nuttx/arch.h>
#include <sys/param.h>
#include "arm_internal.h"

#include "hardware/bk7258_mbox.h"
#include "hardware/bk7258_sysctrl.h"
#include "irq.h"

#define MBOX_REG(n) (BK7258_MBOX0_BASE + ((n) * 4u))
#define MBOX_CH1_CFG MBOX_REG(0x20)
#define MBOX_CH1_TDATA0 MBOX_REG(0x22)
#define MBOX_CH1_TDATA1 MBOX_REG(0x23)
#define MBOX_CH1_TID MBOX_REG(0x24)
#define MBOX_CH1_SID MBOX_REG(0x25)
#define MBOX_CH1_RDATA0 MBOX_REG(0x26)
#define MBOX_CH1_RDATA1 MBOX_REG(0x27)
#define MBOX_CH1_STATUS MBOX_REG(0x28)
#define MBOX_CTRL MBOX_REG(2)
#define MBOX_INT_STATUS MBOX_REG(3)

static bk7258_mbox_callback_t g_callback;

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
  if (destination > 2 || data == NULL || (bk7258_mbox_rx_status() & 1u) != 0)
    {
      return -EINVAL;
    }

  putreg32(data[0], MBOX_CH1_TDATA0);
  putreg32(data[1], MBOX_CH1_TDATA1);
  putreg32(destination, MBOX_CH1_TID);
  return OK;
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
  return OK;
}

void bk7258_mbox_set_callback(bk7258_mbox_callback_t callback)
{
  g_callback = callback;
}

int bk7258_mbox_init(void)
{
  /* CPU1 owns channel 1: FIFO starts at 2 and has three entries. */
  putreg32(1u | (3u << 1), MBOX_CH1_CFG);
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
