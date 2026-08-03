/****************************************************************************
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>

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
  *status = 0;
  return -1;
}

static void bk7258_rxint(struct uart_dev_s *dev, bool enable)
{
}

static bool bk7258_rxavailable(struct uart_dev_s *dev)
{
  return false;
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
