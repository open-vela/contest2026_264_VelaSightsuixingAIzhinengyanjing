/****************************************************************************
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#include <nuttx/config.h>

#include "arm_internal.h"
#include "hardware/bk7258_memorymap.h"
#include "hardware/bk7258_sysctrl.h"
#include "hardware/bk7258_uart.h"

#ifndef CONFIG_UART1_BAUD
#  define CONFIG_UART1_BAUD 115200
#endif

void bk7258_uart1_configure(void)
{
  uint32_t regval;

  /* GPIO0/1 mode zero is UART1 TX/RX. Enable their peripheral functions. */

  modifyreg32(BK7258_GPIO_FUNC0, 0xffu, 0);
  modifyreg32(BK7258_AON_GPIO_BASE, 0, 1u << 6);
  modifyreg32(BK7258_AON_GPIO_BASE + 4, 0, 1u << 6);

  modifyreg32(BK7258_SYS_DEVCLK_EN, 0, BK7258_UART1_CLK_EN);
  modifyreg32(BK7258_SYS_CLKDIV1,
              BK7258_UART1_CLKDIV_MASK | BK7258_UART1_CLKSEL, 0);

  putreg32(0, BK7258_UART1_INT_ENABLE);
  putreg32(BK7258_UART_INT_ALL, BK7258_UART1_INT_STATUS);
  putreg32(0, BK7258_UART1_FIFO_CONFIG);
  regval = BK7258_UART_TX_ENABLE | BK7258_UART_RX_ENABLE |
           BK7258_UART_DATA_8BIT |
           (((BK7258_UART_CLOCK / CONFIG_UART1_BAUD) - 1u) <<
            BK7258_UART_CLKDIV_SHIFT);
  putreg32(regval, BK7258_UART1_CONFIG);
}

void arm_lowputc(char ch)
{
  while ((getreg32(BK7258_UART1_FIFO_STATUS) &
          BK7258_UART_FIFO_WR_READY) == 0)
    {
    }

  putreg32((uint8_t)ch, BK7258_UART1_FIFO_PORT);
}
