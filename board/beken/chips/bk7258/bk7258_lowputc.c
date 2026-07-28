/****************************************************************************
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#include <nuttx/config.h>

#include "arm_internal.h"
#include "hardware/bk7258_memorymap.h"
#include "hardware/bk7258_sysctrl.h"
#include "hardware/bk7258_uart.h"

int bk7258_mbox_uart_write(const uint8_t *data, uint16_t length);

#ifndef CONFIG_UART1_BAUD
#  define CONFIG_UART1_BAUD 115200
#endif

void bk7258_uart1_configure(void)
{
  /* UART1 is intentionally not pinmuxed; the console uses Mailbox UART0. */
}

void arm_lowputc(char ch)
{
  (void)bk7258_mbox_uart_write((const uint8_t *)&ch, 1);
}
