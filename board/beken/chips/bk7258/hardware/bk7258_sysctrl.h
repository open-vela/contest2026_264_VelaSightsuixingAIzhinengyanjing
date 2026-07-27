/****************************************************************************
 * vendor/beken/chips/bk7258/hardware/bk7258_sysctrl.h
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef __VENDOR_BEKEN_CHIPS_BK7258_HARDWARE_BK7258_SYSCTRL_H
#define __VENDOR_BEKEN_CHIPS_BK7258_HARDWARE_BK7258_SYSCTRL_H

#include "bk7258_memorymap.h"

#define BK7258_SYS_CLKDIV1        (BK7258_SYSCTRL_BASE + 0x20u)
#define BK7258_SYS_DEVCLK_EN      (BK7258_SYSCTRL_BASE + 0x30u)
#define BK7258_CPU1_IRQ_EN0       (BK7258_SYSCTRL_BASE + 0x88u)
#define BK7258_CPU1_IRQ_EN1       (BK7258_SYSCTRL_BASE + 0x8cu)
#define BK7258_GPIO_FUNC0         (BK7258_SYSCTRL_BASE + 0xc0u)

#define BK7258_UART1_CLKDIV_SHIFT 11
#define BK7258_UART1_CLKDIV_MASK  (3u << BK7258_UART1_CLKDIV_SHIFT)
#define BK7258_UART1_CLKSEL       (1u << 13)
#define BK7258_UART1_CLK_EN       (1u << 10)
#define BK7258_UART1_IRQ_ROUTE    (1u << 15)

#endif
