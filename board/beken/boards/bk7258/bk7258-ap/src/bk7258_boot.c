/****************************************************************************
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#include <nuttx/config.h>
#include <stdint.h>

#include "board.h"

void board_early_initialize(void)
{
}

int board_app_initialize(uintptr_t arg)
{
  return 0;
}
