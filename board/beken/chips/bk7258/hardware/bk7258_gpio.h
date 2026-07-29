/****************************************************************************
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef __VENDOR_BEKEN_CHIPS_BK7258_HARDWARE_BK7258_GPIO_H
#define __VENDOR_BEKEN_CHIPS_BK7258_HARDWARE_BK7258_GPIO_H

#include <stdint.h>

#define BK7258_GPIO_BASE              0x44000400u
#define BK7258_GPIO_CFG(pin)          (BK7258_GPIO_BASE + ((pin) << 2))

#define BK7258_GPIO_INPUT             (1u << 0)
#define BK7258_GPIO_OUTPUT            (1u << 1)
#define BK7258_GPIO_INPUT_ENABLE      (1u << 2)
#define BK7258_GPIO_OUTPUT_DISABLE    (1u << 3)
#define BK7258_GPIO_PULL_UP           (1u << 4)
#define BK7258_GPIO_PULL_ENABLE       (1u << 5)
#define BK7258_GPIO_SECOND_FUNCTION  (1u << 6)

#define BK7258_GPIO_SYS_BASE          0x440100c0u
#define BK7258_GPIO_SYS_CFG(pin)      (BK7258_GPIO_SYS_BASE + \
                                      (((pin) >> 3) << 2))
#define BK7258_GPIO_SYS_SHIFT(pin)    (((pin) & 7u) << 2)
#define BK7258_GPIO_SYS_MASK(pin)     (0xfu << BK7258_GPIO_SYS_SHIFT(pin))

#endif
