/****************************************************************************
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#include <nuttx/config.h>

#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include <nuttx/syslog/syslog.h>
#include <nuttx/irq.h>

#include "hardware/bk7258_mbox.h"

#define BK7258_SYSLOG_FORCE_TIMEOUT_MS 20u

static int bk7258_syslog_putc(syslog_channel_t *channel, int ch)
{
  uint8_t byte = ch;

  (void)channel;
  return bk7258_mbox_uart_write(&byte, 1) == 1 ? ch : EOF;
}

static int bk7258_syslog_force(syslog_channel_t *channel, int ch)
{
  int ret = bk7258_syslog_putc(channel, ch);

  if (ret != EOF && !up_interrupt_context())
    {
      (void)bk7258_mbox_uart_flush(BK7258_SYSLOG_FORCE_TIMEOUT_MS);
    }

  return ret;
}

static ssize_t bk7258_syslog_write(syslog_channel_t *channel,
                                   const char *buffer, size_t length)
{
  size_t total = 0;

  (void)channel;
  while (total < length)
    {
      size_t chunk = length - total;
      ssize_t ret;

      if (chunk > UINT16_MAX)
        {
          chunk = UINT16_MAX;
        }

      ret = bk7258_mbox_uart_write((const uint8_t *)buffer + total, chunk);
      if (ret <= 0)
        {
          return total != 0 ? (ssize_t)total : ret;
        }

      total += ret;
      if ((size_t)ret < chunk)
        {
          break;
        }
    }

  return total;
}

static ssize_t bk7258_syslog_write_force(syslog_channel_t *channel,
                                         const char *buffer, size_t length)
{
  ssize_t ret = bk7258_syslog_write(channel, buffer, length);

  if (ret > 0 && !up_interrupt_context())
    {
      (void)bk7258_mbox_uart_flush(BK7258_SYSLOG_FORCE_TIMEOUT_MS);
    }

  return ret;
}

static int bk7258_syslog_flush(syslog_channel_t *channel)
{
  (void)channel;
  if (up_interrupt_context())
    {
      return -EAGAIN;
    }

  return bk7258_mbox_uart_flush(BK7258_SYSLOG_FORCE_TIMEOUT_MS);
}

static const struct syslog_channel_ops_s g_bk7258_syslog_ops =
{
  .sc_putc = bk7258_syslog_putc,
  .sc_force = bk7258_syslog_force,
  .sc_flush = bk7258_syslog_flush,
  .sc_write = bk7258_syslog_write,
  .sc_write_force = bk7258_syslog_write_force,
  .sc_close = NULL,
};

static syslog_channel_t g_bk7258_syslog_channel =
{
  .sc_ops = &g_bk7258_syslog_ops,
};

int bk7258_syslog_initialize(void)
{
  bk7258_mbox_uart_early_init();
  return syslog_channel_register(&g_bk7258_syslog_channel);
}
