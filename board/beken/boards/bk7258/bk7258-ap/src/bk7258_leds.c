/****************************************************************************
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdbool.h>
#include <stdint.h>

#include <arch/board/board.h>

void bk7258_gpio_output(unsigned int pin, bool value);
void bk7258_gpio_write(unsigned int pin, bool value);

static bool g_led_initialized;
static volatile uint32_t g_green_states;
static volatile uint32_t g_red_states;

void bk7258_led_initialize(void)
{
  if (!g_led_initialized)
    {
      bk7258_gpio_output(BOARD_LED_RED_GPIO, false);
      bk7258_gpio_output(BOARD_LED_GREEN_GPIO, false);
      g_led_initialized = true;
    }
}

void board_autoled_on(int led)
{
  switch (led)
    {
      case LED_ASSERTION:
      case LED_PANIC:
        g_red_states |= 1u << led;
        bk7258_gpio_write(BOARD_LED_RED_GPIO, true);
        break;

      case LED_INIRQ:
      case LED_SIGNAL:
      case LED_IDLE:
      case LED_HEAPALLOCATE:
      case LED_IRQSENABLED:
      case LED_STACKCREATED:
        g_green_states |= 1u << led;
        bk7258_gpio_write(BOARD_LED_GREEN_GPIO, true);
        break;

      default:
        break;
    }
}

void board_autoled_off(int led)
{
  switch (led)
    {
      case LED_ASSERTION:
        g_red_states &= ~(1u << led);
        if (g_red_states == 0)
          {
            bk7258_gpio_write(BOARD_LED_RED_GPIO, false);
          }
        break;

      case LED_INIRQ:
      case LED_SIGNAL:
      case LED_IDLE:
      case LED_HEAPALLOCATE:
      case LED_IRQSENABLED:
      case LED_STACKCREATED:
        g_green_states &= ~(1u << led);
        if (g_green_states == 0)
          {
            bk7258_gpio_write(BOARD_LED_GREEN_GPIO, false);
          }
        break;

      case LED_PANIC:
        /* Keep the red LED latched until reset after a panic. */
      default:
        break;
    }
}
