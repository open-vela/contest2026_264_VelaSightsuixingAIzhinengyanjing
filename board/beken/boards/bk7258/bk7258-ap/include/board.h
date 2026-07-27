/****************************************************************************
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef __VENDOR_BEKEN_BOARDS_BK7258_AP_INCLUDE_BOARD_H
#define __VENDOR_BEKEN_BOARDS_BK7258_AP_INCLUDE_BOARD_H

#include <nuttx/config.h>

#define BOARD_NAME "bk7258-ap"
#define BOARD_AP_RAM_START 0x28010000u
#define BOARD_AP_RAM_END   0x28064000u
#define BOARD_UART1_TX_GPIO 0
#define BOARD_UART1_RX_GPIO 1

int bk7258_bringup(void);

#endif
