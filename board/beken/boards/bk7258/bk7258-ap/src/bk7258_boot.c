/****************************************************************************
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#include <nuttx/config.h>
#include <stdint.h>

#include <arch/board/board.h>

void board_early_initialize(void)
{
  bk7258_led_initialize();
}

int board_app_initialize(uintptr_t arg)
{
  return 0;
}
