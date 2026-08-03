/****************************************************************************
 * board/beken/chips/bk7258/include/bk7258_gpio.h
 *
 * Function prototypes for bk7258_gpio.c (general-purpose GPIO driver).
 * Register-level macros (BK7258_GPIO_CFG etc.) live in the separate
 * hardware/bk7258_gpio.h; this header only declares the functions that
 * operate on those registers, so callers outside bk7258_gpio.c do not
 * need to hand-write extern declarations (as bk7258_pwm.c previously did
 * for bk7258_gpio_set_function()).
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef __VENDOR_BEKEN_CHIPS_BK7258_INCLUDE_BK7258_GPIO_H
#define __VENDOR_BEKEN_CHIPS_BK7258_INCLUDE_BK7258_GPIO_H

#include <stdbool.h>

void bk7258_gpio_input_pullup(unsigned int pin);
void bk7258_gpio_output(unsigned int pin, bool value);
void bk7258_gpio_write(unsigned int pin, bool value);
bool bk7258_gpio_read(unsigned int pin);
void bk7258_gpio_set_function(unsigned int pin, unsigned int function);

#endif /* __VENDOR_BEKEN_CHIPS_BK7258_INCLUDE_BK7258_GPIO_H */
