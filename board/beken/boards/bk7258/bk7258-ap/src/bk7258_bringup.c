/****************************************************************************
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#include <nuttx/config.h>

#include "board.h"

int bk7258_pwc_start(void);

int bk7258_bringup(void)
{
  return bk7258_pwc_start();
}

void board_late_initialize(void)
{
  (void)bk7258_bringup();
}
