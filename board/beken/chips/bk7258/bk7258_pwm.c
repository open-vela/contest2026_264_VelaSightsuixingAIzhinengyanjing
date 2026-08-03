/****************************************************************************
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

#include <nuttx/irq.h>
#include <nuttx/spinlock.h>
#include <nuttx/timers/pwm.h>

#include "arm_internal.h"
#include "bk7258_gpio.h"

#define BK7258_SYS_CLKSEL             0x44010020u
#define BK7258_SYS_CLK_ENABLE         0x44010030u
#define BK7258_SYS_PWM0_CLKSEL        (1u << 18)
#define BK7258_SYS_PWM0_CLK_ENABLE    (1u << 3)

#define BK7258_PWM0_BASE              0x458a0000u
#define BK7258_PWM_CG_RESET           (BK7258_PWM0_BASE + 0x08u)
#define BK7258_PWM_CR1                (BK7258_PWM0_BASE + 0x10u)
#define BK7258_PWM_EDTR               (BK7258_PWM0_BASE + 0x24u)
#define BK7258_PWM_CCMR               (BK7258_PWM0_BASE + 0x28u)
#define BK7258_PWM_PRESCALER          (BK7258_PWM0_BASE + 0x38u)
#define BK7258_PWM_TIM2_ARR           (BK7258_PWM0_BASE + 0x40u)
#define BK7258_PWM_CCR4               (BK7258_PWM0_BASE + 0x60u)
#define BK7258_PWM_CCR5               (BK7258_PWM0_BASE + 0x64u)

#define BK7258_PWM_CR1_CEN2           (1u << 1)
#define BK7258_PWM_CR1_ARPE2          (1u << 4)
#define BK7258_PWM_CR1_OC2PE          (1u << 7)
#define BK7258_PWM_EDTR_UG2           (1u << 10)
#define BK7258_PWM_CCMR_CH4E          (1u << 15)
#define BK7258_PWM_CCMR_TIM2CCM       (1u << 19)
#define BK7258_PWM_CCMR_OC2M_MASK     (7u << 24)
#define BK7258_PWM_CCMR_OC2M_TOGGLE   (1u << 24)
#define BK7258_PWM_CCMR_OC2M_HIGH     (2u << 24)
#define BK7258_PWM_CCMR_OC2M_LOW      (4u << 24)
#define BK7258_PWM_PSC2_MASK          (0xffu << 8)
#define BK7258_PWM_CLOCK_HZ           26000000u

#define BK7258_MOTOR_PWM_GPIO         9u
#define BK7258_MOTOR_LDO_GPIO         52u
#define BK7258_GPIO_FUNC_PWM3         1u

struct bk7258_pwm_lowerhalf_s
{
  const struct pwm_ops_s *ops;
  bool setup;
};

void bk7258_gpio_output(unsigned int pin, bool value);
void bk7258_gpio_write(unsigned int pin, bool value);
void bk7258_gpio_set_function(unsigned int pin, unsigned int function);

static int bk7258_pwm_setup(struct pwm_lowerhalf_s *dev);
static int bk7258_pwm_shutdown(struct pwm_lowerhalf_s *dev);
static int bk7258_pwm_start(struct pwm_lowerhalf_s *dev,
                            const struct pwm_info_s *info);
static int bk7258_pwm_stop(struct pwm_lowerhalf_s *dev);
static int bk7258_pwm_ioctl(struct pwm_lowerhalf_s *dev, int cmd,
                            unsigned long arg);

static const struct pwm_ops_s g_pwm_ops =
{
  .setup = bk7258_pwm_setup,
  .shutdown = bk7258_pwm_shutdown,
  .start = bk7258_pwm_start,
  .stop = bk7258_pwm_stop,
  .ioctl = bk7258_pwm_ioctl,
};

static struct bk7258_pwm_lowerhalf_s g_motor_pwm =
{
  .ops = &g_pwm_ops,
};

static int bk7258_pwm_setup(struct pwm_lowerhalf_s *dev)
{
  struct bk7258_pwm_lowerhalf_s *priv =
    (struct bk7258_pwm_lowerhalf_s *)dev;
  irqstate_t flags;

  flags = enter_critical_section();
  modifyreg32(BK7258_SYS_CLKSEL, 0, BK7258_SYS_PWM0_CLKSEL);
  modifyreg32(BK7258_SYS_CLK_ENABLE, 0, BK7258_SYS_PWM0_CLK_ENABLE);
  putreg32(1, BK7258_PWM_CG_RESET);
  bk7258_gpio_set_function(BK7258_MOTOR_PWM_GPIO, BK7258_GPIO_FUNC_PWM3);
  bk7258_gpio_output(BK7258_MOTOR_LDO_GPIO, false);
  priv->setup = true;
  leave_critical_section(flags);

  return OK;
}

static int bk7258_pwm_shutdown(struct pwm_lowerhalf_s *dev)
{
  struct bk7258_pwm_lowerhalf_s *priv =
    (struct bk7258_pwm_lowerhalf_s *)dev;

  bk7258_pwm_stop(dev);
  modifyreg32(BK7258_SYS_CLK_ENABLE, BK7258_SYS_PWM0_CLK_ENABLE, 0);
  priv->setup = false;
  return OK;
}

static int bk7258_pwm_start(struct pwm_lowerhalf_s *dev,
                            const struct pwm_info_s *info)
{
  struct bk7258_pwm_lowerhalf_s *priv =
    (struct bk7258_pwm_lowerhalf_s *)dev;
  uint64_t period;
  uint64_t duty;
  irqstate_t flags;

  if (!priv->setup || info == NULL || info->frequency == 0)
    {
      return -EINVAL;
    }

  period = BK7258_PWM_CLOCK_HZ / info->frequency;
  if (period < 2 || period > UINT32_MAX)
    {
      return -ERANGE;
    }

  duty = (period * info->duty) >> 16;
  if (duty > period)
    {
      duty = period;
    }

  flags = enter_critical_section();
  modifyreg32(BK7258_PWM_CR1, BK7258_PWM_CR1_CEN2, 0);
  modifyreg32(BK7258_PWM_CR1, 0,
              BK7258_PWM_CR1_ARPE2 | BK7258_PWM_CR1_OC2PE);
  modifyreg32(BK7258_PWM_PRESCALER, BK7258_PWM_PSC2_MASK, 0);
  putreg32((uint32_t)period - 1u, BK7258_PWM_TIM2_ARR);
  putreg32((uint32_t)(period - duty), BK7258_PWM_CCR4);
  putreg32((uint32_t)period, BK7258_PWM_CCR5);
  modifyreg32(BK7258_PWM_CCMR,
              BK7258_PWM_CCMR_TIM2CCM | BK7258_PWM_CCMR_OC2M_MASK,
              BK7258_PWM_CCMR_CH4E |
              (duty == 0 ? BK7258_PWM_CCMR_OC2M_LOW :
               duty == period ? BK7258_PWM_CCMR_OC2M_HIGH :
               BK7258_PWM_CCMR_OC2M_TOGGLE));
  modifyreg32(BK7258_PWM_EDTR, 0, BK7258_PWM_EDTR_UG2);
  modifyreg32(BK7258_PWM_CR1, 0, BK7258_PWM_CR1_CEN2);
  bk7258_gpio_write(BK7258_MOTOR_LDO_GPIO, duty != 0);
  leave_critical_section(flags);

  return OK;
}

static int bk7258_pwm_stop(struct pwm_lowerhalf_s *dev)
{
  irqstate_t flags;

  (void)dev;
  flags = enter_critical_section();
  modifyreg32(BK7258_PWM_CCMR, BK7258_PWM_CCMR_CH4E, 0);
  modifyreg32(BK7258_PWM_CR1, BK7258_PWM_CR1_CEN2, 0);
  bk7258_gpio_write(BK7258_MOTOR_LDO_GPIO, false);
  leave_critical_section(flags);

  return OK;
}

static int bk7258_pwm_ioctl(struct pwm_lowerhalf_s *dev, int cmd,
                            unsigned long arg)
{
  (void)dev;
  (void)cmd;
  (void)arg;
  return -ENOTTY;
}

struct pwm_lowerhalf_s *bk7258_pwminitialize(void)
{
  return (struct pwm_lowerhalf_s *)&g_motor_pwm;
}
