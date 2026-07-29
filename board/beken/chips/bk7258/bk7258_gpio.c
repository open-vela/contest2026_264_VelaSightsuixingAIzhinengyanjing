/****************************************************************************
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdbool.h>
#include <stdint.h>

#include <nuttx/irq.h>
#include <nuttx/spinlock.h>

#include "arm_internal.h"
#include "hardware/bk7258_gpio.h"

void bk7258_gpio_input_pullup(unsigned int pin)
{
  irqstate_t flags;

  flags = enter_critical_section();
  modifyreg32(BK7258_GPIO_CFG(pin),
              BK7258_GPIO_OUTPUT | BK7258_GPIO_SECOND_FUNCTION,
              BK7258_GPIO_INPUT_ENABLE | BK7258_GPIO_OUTPUT_DISABLE |
              BK7258_GPIO_PULL_UP | BK7258_GPIO_PULL_ENABLE);
  leave_critical_section(flags);
}

void bk7258_gpio_output(unsigned int pin, bool value)
{
  irqstate_t flags;
  uint32_t setbits = value ? BK7258_GPIO_OUTPUT : 0;

  flags = enter_critical_section();
  modifyreg32(BK7258_GPIO_CFG(pin),
              BK7258_GPIO_INPUT_ENABLE | BK7258_GPIO_OUTPUT |
              BK7258_GPIO_OUTPUT_DISABLE | BK7258_GPIO_PULL_ENABLE |
              BK7258_GPIO_SECOND_FUNCTION,
              setbits);
  leave_critical_section(flags);
}

void bk7258_gpio_write(unsigned int pin, bool value)
{
  if (value)
    {
      modifyreg32(BK7258_GPIO_CFG(pin), 0, BK7258_GPIO_OUTPUT);
    }
  else
    {
      modifyreg32(BK7258_GPIO_CFG(pin), BK7258_GPIO_OUTPUT, 0);
    }
}

bool bk7258_gpio_read(unsigned int pin)
{
  return (getreg32(BK7258_GPIO_CFG(pin)) & BK7258_GPIO_INPUT) != 0;
}

void bk7258_gpio_set_function(unsigned int pin, unsigned int function)
{
  irqstate_t flags;
  uintptr_t regaddr = BK7258_GPIO_SYS_CFG(pin);
  uint32_t shift = BK7258_GPIO_SYS_SHIFT(pin);

  flags = enter_critical_section();
  modifyreg32(regaddr, BK7258_GPIO_SYS_MASK(pin), (function & 0xfu) << shift);
  modifyreg32(BK7258_GPIO_CFG(pin),
              BK7258_GPIO_INPUT_ENABLE | BK7258_GPIO_OUTPUT |
              BK7258_GPIO_PULL_ENABLE,
              BK7258_GPIO_OUTPUT_DISABLE | BK7258_GPIO_SECOND_FUNCTION);
  leave_critical_section(flags);
}
