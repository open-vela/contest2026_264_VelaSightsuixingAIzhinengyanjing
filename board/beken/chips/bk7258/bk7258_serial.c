/****************************************************************************
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>

#include <nuttx/irq.h>
#include <nuttx/serial/serial.h>

#include "arm_internal.h"
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
  bk7258_uart1_configure();
  return OK;
}

static void bk7258_shutdown(struct uart_dev_s *dev)
{
  putreg32(0, BK7258_UART1_INT_ENABLE);
}

static int bk7258_interrupt(int irq, void *context, void *arg)
{
  struct uart_dev_s *dev = arg;
  uint32_t status = getreg32(BK7258_UART1_INT_STATUS);

  putreg32(status, BK7258_UART1_INT_STATUS);
  if ((status & BK7258_UART_INT_RX) != 0)
    {
      uart_recvchars(dev);
    }

  if ((status & BK7258_UART_INT_TX) != 0)
    {
      uart_xmitchars(dev);
    }

  return OK;
}

static int bk7258_attach(struct uart_dev_s *dev)
{
  int ret = irq_attach(BK7258_IRQ_UART1, bk7258_interrupt, dev);
  if (ret == OK)
    {
      up_enable_irq(BK7258_IRQ_UART1);
    }

  return ret;
}

static void bk7258_detach(struct uart_dev_s *dev)
{
  up_disable_irq(BK7258_IRQ_UART1);
  irq_detach(BK7258_IRQ_UART1);
}

static int bk7258_ioctl(struct file *filep, int cmd, unsigned long arg)
{
  return -ENOTTY;
}

static int bk7258_receive(struct uart_dev_s *dev, unsigned int *status)
{
  *status = 0;
  return (getreg32(BK7258_UART1_FIFO_PORT) >> 8) & 0xff;
}

static void bk7258_rxint(struct uart_dev_s *dev, bool enable)
{
  modifyreg32(BK7258_UART1_INT_ENABLE,
              BK7258_UART_INT_RX,
              enable ? BK7258_UART_INT_RX : 0);
}

static bool bk7258_rxavailable(struct uart_dev_s *dev)
{
  return (getreg32(BK7258_UART1_FIFO_STATUS) &
          BK7258_UART_FIFO_RD_READY) != 0;
}

static void bk7258_send(struct uart_dev_s *dev, int ch)
{
  putreg32((uint8_t)ch, BK7258_UART1_FIFO_PORT);
}

static void bk7258_txint(struct uart_dev_s *dev, bool enable)
{
  modifyreg32(BK7258_UART1_INT_ENABLE,
              BK7258_UART_INT_TX,
              enable ? BK7258_UART_INT_TX : 0);
  if (enable)
    {
      uart_xmitchars(dev);
    }
}

static bool bk7258_txready(struct uart_dev_s *dev)
{
  return (getreg32(BK7258_UART1_FIFO_STATUS) &
          BK7258_UART_FIFO_WR_READY) != 0;
}

static bool bk7258_txempty(struct uart_dev_s *dev)
{
  return (getreg32(BK7258_UART1_FIFO_STATUS) & BK7258_UART_TX_EMPTY) != 0;
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
