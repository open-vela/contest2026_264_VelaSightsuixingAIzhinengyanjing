/****************************************************************************
 * board/beken/chips/bk7258/bk7258_gc9d01_gpio.c
 *
 * Minimal BK7258 GPIO output driver dedicated to the GC9D01 QSPI panel
 * bring-up.  Each pin has its own 32-bit config register at
 * BK7258_AON_GPIO_BASE + pin * 4.  bit[1] is the output level, bit[3] is
 * the output-enable bit.  Source of these bit positions:
 * bk_avdk_smp/ap/middleware/soc/bk7258_ap/soc/gpio_struct.h (release/v3.1.1).
 *
 * This is not a general GPIO framework: it only supports enabling a pin
 * as an output and driving it high/low.  No pinmux, pull, or interrupt
 * support.  See bk7258_gpio.c for the general-purpose GPIO driver used by
 * buttons/PWM.
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#include <nuttx/config.h>
#include <stdint.h>

#include "arm_internal.h"
#include "hardware/bk7258_memorymap.h"
#include "bk7258_gc9d01_gpio.h"

#define BK7258_GPIO_CFG(pin)   (BK7258_AON_GPIO_BASE + ((pin) * 4u))
#define BK7258_GPIO_OUTPUT_BIT (1u << 1)
#define BK7258_GPIO_OUTEN_BIT  (1u << 3)

void bk7258_gpio_output_enable(uint32_t pin)
{
  modifyreg32(BK7258_GPIO_CFG(pin), 0, BK7258_GPIO_OUTEN_BIT);
}

void bk7258_gpio_set_high(uint32_t pin)
{
  modifyreg32(BK7258_GPIO_CFG(pin), 0, BK7258_GPIO_OUTPUT_BIT);
}

void bk7258_gpio_set_low(uint32_t pin)
{
  modifyreg32(BK7258_GPIO_CFG(pin), BK7258_GPIO_OUTPUT_BIT, 0);
}
