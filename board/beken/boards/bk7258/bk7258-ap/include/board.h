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

#define BOARD_KEY_VOLUME_UP_GPIO   13
#define BOARD_KEY_POWER_GPIO       12
#define BOARD_KEY_VOLUME_DOWN_GPIO 8

#define BUTTON_VOLUME_UP           0
#define BUTTON_POWER               1
#define BUTTON_VOLUME_DOWN         2
#define NUM_BUTTONS                3

#define BUTTON_VOLUME_UP_BIT       (1u << BUTTON_VOLUME_UP)
#define BUTTON_POWER_BIT           (1u << BUTTON_POWER)
#define BUTTON_VOLUME_DOWN_BIT     (1u << BUTTON_VOLUME_DOWN)

int bk7258_bringup(void);

#endif
