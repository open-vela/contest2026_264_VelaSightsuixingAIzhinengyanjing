/****************************************************************************
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <stdio.h>

#include <nuttx/irq.h>
#include <nuttx/serial/serial.h>

#include "arm_internal.h"
#include "hardware/bk7258_mbox.h"
#include "hardware/bk7258_uart.h"

#ifndef CONFIG_UART1_RXBUFSIZE
#  define CONFIG_UART1_RXBUFSIZE 256
#endif
#ifndef CONFIG_UART1_TXBUFSIZE
#  define CONFIG_UART1_TXBUFSIZE 256
#endif

/* dev->recv is NuttX's serial framework's own ring buffer, populated by
 * uart_recvchars() itself by repeatedly calling this driver's receive()
 * (once per byte, "give me the next hardware byte") and rxavailable()
 * ("is there another hardware byte ready?") -- see uart_16550.c's
 * u16550_receive() for the reference pattern: it reads directly from a
 * hardware register, it never touches dev->recv.
 *
 * This driver's "hardware" is the mailbox UART0 RX channel
 * (bk7258_mailbox_channel.c's mailbox_rx() -> bk7258_serial_rx_push()),
 * which delivers a whole line at once from an IRQ callback rather than one
 * byte at a time from a real UART RX FIFO register. So this driver needs
 * its own small private FIFO to bridge "IRQ hands us N bytes at once" to
 * "framework asks for bytes one at a time via receive()/rxavailable()" --
 * it must NOT reuse dev->recv for this, since uart_recvchars() owns that
 * buffer's head/tail and writes to it itself. */
#define BK7258_HW_RXFIFO_SIZE 256u

static uint8_t g_bk7258_hw_rxfifo[BK7258_HW_RXFIFO_SIZE];
static uint16_t g_bk7258_hw_rxfifo_head; /* next byte to pop (receive()) */
static uint16_t g_bk7258_hw_rxfifo_tail; /* next free slot (rx_push()) */

extern void bk7258_uart1_configure(void);

static int bk7258_setup(struct uart_dev_s *dev)
{
  return OK;
}

static void bk7258_shutdown(struct uart_dev_s *dev)
{
}

static int bk7258_attach(struct uart_dev_s *dev)
{
  return OK;
}

static void bk7258_detach(struct uart_dev_s *dev)
{
}

static int bk7258_ioctl(struct file *filep, int cmd, unsigned long arg)
{
  return -ENOTTY;
}

static int bk7258_receive(struct uart_dev_s *dev, unsigned int *status)
{
  int ch;

  *status = 0;

  if (g_bk7258_hw_rxfifo_head == g_bk7258_hw_rxfifo_tail)
    {
      return -1;
    }

  ch = (unsigned char)g_bk7258_hw_rxfifo[g_bk7258_hw_rxfifo_head];
  g_bk7258_hw_rxfifo_head =
      (g_bk7258_hw_rxfifo_head + 1) % BK7258_HW_RXFIFO_SIZE;
  return ch;
}

static void bk7258_rxint(struct uart_dev_s *dev, bool enable)
{
}

static bool bk7258_rxavailable(struct uart_dev_s *dev)
{
  return g_bk7258_hw_rxfifo_head != g_bk7258_hw_rxfifo_tail;
}

static void bk7258_send(struct uart_dev_s *dev, int ch)
{
  (void)bk7258_mbox_uart_write((const uint8_t *)&ch, 1);
}

static void bk7258_txint(struct uart_dev_s *dev, bool enable)
{
  if (enable)
    {
      uart_xmitchars(dev);
    }
}

static bool bk7258_txready(struct uart_dev_s *dev)
{
  return bk7258_mbox_uart_txready();
}

static bool bk7258_txempty(struct uart_dev_s *dev)
{
  return bk7258_mbox_uart_txempty();
}

static const struct uart_ops_s g_bk7258_uart_ops =
{
  .setup       = bk7258_setup,
  .shutdown    = bk7258_shutdown,
  .attach      = bk7258_attach,
  .detach      = bk7258_detach,
  .ioctl       = bk7258_ioctl,
  .receive     = bk7258_receive,
  .rxint       = bk7258_rxint,
  .rxavailable = bk7258_rxavailable,
  .send        = bk7258_send,
  .txint       = bk7258_txint,
  .txready     = bk7258_txready,
  .txempty     = bk7258_txempty,
};

static char g_bk7258_rxbuffer[CONFIG_UART1_RXBUFSIZE];
static char g_bk7258_txbuffer[CONFIG_UART1_TXBUFSIZE];

static struct uart_dev_s g_bk7258_uart1 =
{
  .isconsole = true,
  .recv =
  {
    .size = CONFIG_UART1_RXBUFSIZE,
    .buffer = g_bk7258_rxbuffer,
  },
  .xmit =
  {
    .size = CONFIG_UART1_TXBUFSIZE,
    .buffer = g_bk7258_txbuffer,
  },
  .ops = &g_bk7258_uart_ops,
};

void bk7258_serial_tx_available(void)
{
  uart_xmitchars(&g_bk7258_uart1);
}

void bk7258_serial_rx_push(const uint8_t *data, uint16_t length)
{
  uint16_t i;
  uint16_t next_tail;
  uint8_t diag[32];
  int diag_len;

  for (i = 0; i < length; i++)
    {
      next_tail = (g_bk7258_hw_rxfifo_tail + 1) % BK7258_HW_RXFIFO_SIZE;
      if (next_tail == g_bk7258_hw_rxfifo_head)
        {
          /* Private hardware-level FIFO full: drop the remaining bytes
           * rather than overwrite unread data or block the mailbox RX
           * path. This is separate from dev->recv, which is NuttX's own
           * ring buffer that uart_recvchars() fills by calling receive()
           * below -- see this file's BK7258_HW_RXFIFO_SIZE comment. */
          break;
        }

      g_bk7258_hw_rxfifo[g_bk7258_hw_rxfifo_tail] = data[i];
      g_bk7258_hw_rxfifo_tail = next_tail;
    }

  diag_len = snprintf((char *)diag, sizeof(diag),
                       "mbox: rx_push %u/%u\r\n", (unsigned int)i,
                       (unsigned int)length);
  if (diag_len > 0)
    {
      (void)bk7258_mbox_uart_write(diag, (uint16_t)diag_len);
    }

  uart_recvchars(&g_bk7258_uart1);
}

void arm_earlyserialinit(void)
{
  bk7258_uart1_configure();
}

void arm_serialinit(void)
{
  uart_register("/dev/console", &g_bk7258_uart1);
  uart_register("/dev/ttyS1", &g_bk7258_uart1);
}

void up_putc(int ch)
{
  if (ch == '\n')
    {
      arm_lowputc('\r');
    }

  arm_lowputc(ch);
}
