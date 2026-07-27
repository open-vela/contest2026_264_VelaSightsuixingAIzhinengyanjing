/****************************************************************************
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#include <nuttx/config.h>

#include "board.h"

int bk7258_bringup(void)
{
  /* Do not send a partial AP-ready message before mailbox v2 is complete. */
  return 0;
}

void board_late_initialize(void)
{
  (void)bk7258_bringup();
}
