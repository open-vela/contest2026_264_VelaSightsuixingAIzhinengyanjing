/****************************************************************************
 * board/beken/chips/bk7258/include/bk7258_gc9d01_gpio.h
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef __VENDOR_BEKEN_CHIPS_BK7258_INCLUDE_BK7258_GC9D01_GPIO_H
#define __VENDOR_BEKEN_CHIPS_BK7258_INCLUDE_BK7258_GC9D01_GPIO_H

#include <stdint.h>

/* Minimal push-pull output control for a small, fixed set of BK7258 pins,
 * dedicated to the GC9D01 QSPI panel bring-up.  This is not a general
 * GPIO framework: it only supports enabling a pin as an output and
 * driving it high/low.  No pinmux, pull, or interrupt support.
 */

void bk7258_gpio_output_enable(uint32_t pin);
void bk7258_gpio_set_high(uint32_t pin);
void bk7258_gpio_set_low(uint32_t pin);

#endif /* __VENDOR_BEKEN_CHIPS_BK7258_INCLUDE_BK7258_GC9D01_GPIO_H */
