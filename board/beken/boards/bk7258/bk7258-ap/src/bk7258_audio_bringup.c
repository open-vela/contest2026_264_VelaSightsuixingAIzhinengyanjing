/****************************************************************************
 * board/beken/boards/bk7258/bk7258-ap/src/bk7258_audio_bringup.c
 *
 * Registration glue for the internal-DAC audio path, plus ownership of the
 * one piece of board wiring the chip layer must not know about: the
 * HT6872 amplifier's mute GPIO.
 *
 * Board facts this file encodes (AIDK_AI玩具开发板_原理图.pdf):
 *
 *   - Net MUTE is BK7258 P50 (the sheet 2/6 chip pin table lists it
 *     against "P50/ENET_RXD1/R0"), reaching the HT6872's CTRL pin through
 *     R70 (1K).  R71 (10K) pulls CTRL to ground, so the amplifier is
 *     muted at power-on and stays muted until software drives P50 high --
 *     which is what suppresses the classic boot pop.
 *   - The amplifier is supplied from PA_VDD, which comes from VBAT, not
 *     from the LDO_3V3 rail gated by LDO33_EN.  Audio therefore does not
 *     depend on that rail's enable.
 *
 * A note on the GPIO number, because the same reasoning already cost this
 * project repeated camera bring-up failures: GPIO50's name in the vendor
 * pinmux table is ENET_RXD1 (gpio_map.h:78), which says nothing about what
 * the board actually connects it to.  Only the schematic net name does --
 * here PA_SD, reaching the HT6872 CTRL pin through R70.  The camera's
 * control bus went through the same correction; see the file header of
 * chips/bk7258/include/bk7258_gc2145_i2c_bitbang.h for that write-up.
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#include <nuttx/audio/audio.h>

#include "bk7258_aud.h"
#include "bk7258_audio_bringup.h"

#ifdef CONFIG_BK7258_AUDIO

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#ifndef CONFIG_BK7258_AUDIO_PA_MUTE_GPIO
#  define CONFIG_BK7258_AUDIO_PA_MUTE_GPIO 50
#endif

#define BK7258_AUDIO_PA_GPIO ((unsigned int)CONFIG_BK7258_AUDIO_PA_MUTE_GPIO)

/****************************************************************************
 * External Function Prototypes
 ****************************************************************************/

/* Chip-layer GPIO helpers.  bk7258_gpio_output() also clears the pad's
 * second-function select, which GPIO50 needs because its power-on default
 * is GPIO_SECOND_FUNC_ENABLE / GPIO_DEV_LCD_R0.
 */

void bk7258_gpio_output(unsigned int pin, bool value);
void bk7258_gpio_write(unsigned int pin, bool value);

FAR struct audio_lowerhalf_s *bk7258_audio_dev_initialize(bool playback);

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: bk7258_audio_pa_enable
 ****************************************************************************/

void bk7258_audio_pa_enable(bool enable)
{
  bk7258_gpio_write(BK7258_AUDIO_PA_GPIO, enable);
}

/****************************************************************************
 * Name: bk7258_audio_initialize
 ****************************************************************************/

int bk7258_audio_initialize(void)
{
  FAR struct audio_lowerhalf_s *lower;
  int ret;

  /* Park the amplifier in its muted state before anything else touches
   * the audio path.  Configuring the pad as an output-low both takes it
   * off LCD_R0 and makes the R71 pull-down's intent explicit rather than
   * relying on the pad staying an input.
   */

  bk7258_gpio_output(BK7258_AUDIO_PA_GPIO, false);

  ret = bk7258_aud_initialize();
  if (ret < 0)
    {
      printf("bk7258_audio: AUD block bring-up failed, error=%d\n", ret);
      return ret;
    }

  printf("bk7258_audio: AUD id=0x%08lx version=0x%08lx, PA mute on GPIO%u\n",
         (unsigned long)bk7258_aud_read_id(),
         (unsigned long)bk7258_aud_read_version(),
         BK7258_AUDIO_PA_GPIO);

  lower = bk7258_audio_dev_initialize(true);
  if (lower == NULL)
    {
      return -ENODEV;
    }

  ret = audio_register("pcm0p", lower);
  if (ret < 0)
    {
      printf("bk7258_audio: failed to register /dev/audio/pcm0p, "
             "error=%d\n", ret);
      return ret;
    }

  printf("bk7258_audio: /dev/audio/pcm0p registered (internal DAC -> "
         "HT6872)\n");

#ifdef CONFIG_BK7258_AUDIO_CAPTURE
  lower = bk7258_audio_dev_initialize(false);
  if (lower == NULL)
    {
      return -ENODEV;
    }

  ret = audio_register("pcm0c", lower);
  if (ret < 0)
    {
      printf("bk7258_audio: failed to register /dev/audio/pcm0c, "
             "error=%d\n", ret);
      return ret;
    }

#  ifdef CONFIG_BK7258_AUDIO_AEC_REFERENCE
  printf("bk7258_audio: /dev/audio/pcm0c registered (MIC1 voice + MIC2 "
         "echo reference, L/R interleaved)\n");
#  else
  printf("bk7258_audio: /dev/audio/pcm0c registered (MIC1 voice, mono)\n");
#  endif
#endif

  return OK;
}

#endif /* CONFIG_BK7258_AUDIO */
