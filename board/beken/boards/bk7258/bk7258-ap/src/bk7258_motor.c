/****************************************************************************
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#include <nuttx/config.h>

#include <fcntl.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <nuttx/board.h>
#include <nuttx/kthread.h>
#include <nuttx/signal.h>
#include <nuttx/timers/pwm.h>

#include <arch/board/board.h>

#define BK7258_KEY_POLL_US       5000
#define BK7258_KEY_DEBOUNCE_COUNT 6
#define BK7258_MOTOR_FREQUENCY   1000
#define BK7258_MOTOR_DUTY        ((ub16_t)((7u * 65536u) / 10u))
#define BK7258_PM_CLK_CTRL_CMD   2
#define BK7258_PM_CLK_ID_PWM_1   3
#define BK7258_PM_CLK_POWER_UP   1

struct pwm_lowerhalf_s *bk7258_pwminitialize(void);
int bk7258_mailbox_send_pwc(uint8_t command, uint32_t p1, uint32_t p2,
                            uint32_t p3);

#ifdef CONFIG_BK7258_POWER_KEY_MOTOR
static int bk7258_motor_button_worker(int argc, char **argv)
{
  struct pwm_info_s info =
  {
    .frequency = BK7258_MOTOR_FREQUENCY,
    .duty = BK7258_MOTOR_DUTY,
  };
  bool candidate = false;
  bool pressed = false;
  unsigned int samples = 0;
  int fd;

  (void)argc;
  (void)argv;

  fd = bk7258_mailbox_send_pwc(BK7258_PM_CLK_CTRL_CMD,
                               BK7258_PM_CLK_ID_PWM_1,
                               BK7258_PM_CLK_POWER_UP, 0);
  if (fd < 0)
    {
      return fd;
    }

  /* 当前 PWC 仅入队请求，不等待 CP 语义响应。PWM0 时钟门控由 CP 收到
   * mailbox 命令后才打开，此处延时等待 CP 完成时钟使能，避免后续 PWM
   * 寄存器写入因时钟未就绪而丢失。待 PWC worker 补上 ACK 消费后应改为
   * 有界等待。
   */
  nxsig_usleep(20000);
  fd = open("/dev/pwm0", O_RDWR);
  if (fd < 0)
    {
      return -errno;
    }

  if (ioctl(fd, PWMIOC_SETCHARACTERISTICS,
            (unsigned long)(uintptr_t)&info) < 0)
    {
      int ret = -errno;
      close(fd);
      return ret;
    }

  for (;;)
    {
      bool sample = (board_buttons() & BUTTON_POWER_BIT) != 0;

      if (sample != candidate)
        {
          candidate = sample;
          samples = 1;
        }
      else if (samples < BK7258_KEY_DEBOUNCE_COUNT)
        {
          samples++;
        }

      if (samples == BK7258_KEY_DEBOUNCE_COUNT && pressed != candidate)
        {
          int cmd = candidate ? PWMIOC_START : PWMIOC_STOP;

          if (ioctl(fd, cmd, 0) < 0)
            {
              close(fd);
              return -errno;
            }

          pressed = candidate;
        }

      nxsig_usleep(BK7258_KEY_POLL_US);
    }
}
#endif

int bk7258_motor_setup(void)
{
  static bool initialized;
  struct pwm_lowerhalf_s *pwm;
  int ret;

  if (initialized)
    {
      return OK;
    }

  pwm = bk7258_pwminitialize();
  if (pwm == NULL)
    {
      return -ENODEV;
    }

  ret = pwm_register("/dev/pwm0", pwm);
  if (ret >= 0)
    {
      initialized = true;
    }

  return ret;
}

int bk7258_power_key_motor_start(void)
{
#ifdef CONFIG_BK7258_POWER_KEY_MOTOR
  int pid = kthread_create("power-key-motor", 100, 1536,
                           bk7258_motor_button_worker, NULL);

  return pid < 0 ? pid : OK;
#else
  return OK;
#endif
}
