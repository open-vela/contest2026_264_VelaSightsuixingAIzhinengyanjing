/****************************************************************************
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdbool.h>
#include <stdint.h>

#include <nuttx/board.h>
#include <nuttx/ioexpander/gpio.h>

#include "board.h"

#ifdef CONFIG_DEV_GPIO
struct bk7258_button_gpio_s
{
  struct gpio_dev_s gpio;
  uint8_t id;
};
#endif

void bk7258_gpio_input_pullup(unsigned int pin);
bool bk7258_gpio_read(unsigned int pin);

static const uint8_t g_button_pins[NUM_BUTTONS] =
{
  BOARD_KEY_VOLUME_UP_GPIO,
  BOARD_KEY_POWER_GPIO,
  BOARD_KEY_VOLUME_DOWN_GPIO,
};

#ifdef CONFIG_DEV_GPIO
static int bk7258_button_read(struct gpio_dev_s *dev, bool *value);

static const struct gpio_operations_s g_button_ops =
{
  .go_read = bk7258_button_read,
  .go_write = NULL,
  .go_attach = NULL,
  .go_enable = NULL,
};

static struct bk7258_button_gpio_s g_button_gpio[NUM_BUTTONS];

static int bk7258_button_read(struct gpio_dev_s *dev, bool *value)
{
  struct bk7258_button_gpio_s *button =
    (struct bk7258_button_gpio_s *)dev;

  *value = !bk7258_gpio_read(g_button_pins[button->id]);
  return OK;
}
#endif

uint32_t board_button_initialize(void)
{
  unsigned int i;

  for (i = 0; i < NUM_BUTTONS; i++)
    {
      bk7258_gpio_input_pullup(g_button_pins[i]);
#ifdef CONFIG_DEV_GPIO
      g_button_gpio[i].gpio.gp_pintype = GPIO_INPUT_PIN;
      g_button_gpio[i].gpio.gp_ops = &g_button_ops;
      g_button_gpio[i].id = i;
      gpio_pin_register(&g_button_gpio[i].gpio, i);
#endif
    }

  return NUM_BUTTONS;
}

uint32_t board_buttons(void)
{
  uint32_t pressed = 0;
  unsigned int i;

  for (i = 0; i < NUM_BUTTONS; i++)
    {
      if (!bk7258_gpio_read(g_button_pins[i]))
        {
          pressed |= 1u << i;
        }
    }

  return pressed;
}
