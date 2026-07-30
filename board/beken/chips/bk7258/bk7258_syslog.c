/****************************************************************************
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#include <nuttx/config.h>

#include <stddef.h>
#include <sys/types.h>

#include <nuttx/syslog/syslog.h>

#include "hardware/bk7258_mbox.h"

static int bk7258_syslog_putc(syslog_channel_t *channel, int ch)
{
  (void)channel;
  (void)bk7258_mbox_uart_write((const uint8_t *)&ch, 1);
  return ch;
}

static int bk7258_syslog_force(syslog_channel_t *channel, int ch)
{
  return bk7258_syslog_putc(channel, ch);
}

static ssize_t bk7258_syslog_write(syslog_channel_t *channel,
                                   const char *buffer, size_t length)
{
  (void)channel;
  (void)bk7258_mbox_uart_write((const uint8_t *)buffer, length);
  return length;
}

static ssize_t bk7258_syslog_write_force(syslog_channel_t *channel,
                                         const char *buffer, size_t length)
{
  return bk7258_syslog_write(channel, buffer, length);
}

static const struct syslog_channel_ops_s g_bk7258_syslog_ops =
{
  .sc_putc = bk7258_syslog_putc,
  .sc_force = bk7258_syslog_force,
  .sc_flush = NULL,
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
