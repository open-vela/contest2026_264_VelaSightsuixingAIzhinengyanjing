/****************************************************************************
 * boards/beken/boards/bk7258/bk7258-ap/src/bk7258_audio_bringup.h
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef __BOARDS_BEKEN_BK7258_AP_SRC_BK7258_AUDIO_BRINGUP_H
#define __BOARDS_BEKEN_BK7258_AP_SRC_BK7258_AUDIO_BRINGUP_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdbool.h>

#include <nuttx/audio/audio.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Private diagnostic ioctl.
 *
 * AUDIOIOC_DUMP cannot be used for this: the upper half handles it itself
 * (audio.c "case AUDIOIOC_DUMP: ret = audio_dump(upper, ...)") and never
 * forwards it down.  Unrecognized commands, on the other hand, do reach the
 * lower half through the upper half's default case, so a private number
 * gets us a channel to the driver.
 *
 * The handler prints the AUD and analog registers and then performs a
 * short *polled* FIFO read, deliberately bypassing the interrupt path.
 * That separates the two candidate explanations for a silent capture:
 * if polling returns samples the ADC is running and only interrupt
 * delivery is broken; if it returns nothing the ADC itself is not
 * producing data.
 *
 * The same number is duplicated as AUDIO_TEST_IOC_DIAG in
 * app/audio_test/audio_test_main.c, because applications do not get this
 * private src directory on their include path.  Keep the two in step.
 */

#define BK7258_AUDIOIOC_DIAG _AUDIOIOC(200)

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

#ifdef CONFIG_BK7258_AUDIO

/****************************************************************************
 * Name: bk7258_audio_initialize
 *
 * Description:
 *   Bring up the internal-DAC audio path and register the NuttX audio
 *   devices.  Must be called after bk7258_pwc_start(): the AUD block needs
 *   the AUDP power domain and the AUD module clock, both of which are
 *   requested from CP1 over the PWC mailbox channel.
 *
 *   Registers /dev/audio/pcm0p for playback and, when
 *   CONFIG_BK7258_AUDIO_CAPTURE is set, /dev/audio/pcm0c for capture.
 *
 * Returned Value:
 *   OK on success, a negated errno on failure.
 *
 ****************************************************************************/

int bk7258_audio_initialize(void);

/****************************************************************************
 * Name: bk7258_audio_pa_enable
 *
 * Description:
 *   Drive the HT6872 amplifier's CTRL pin (net MUTE, GPIO50) high to
 *   un-mute or low to mute.  Exposed so the audio lower-half can sequence
 *   the amplifier against the DAC without owning board wiring knowledge.
 *
 *   The pin is configured as a plain output at bring-up.  Note
 *   bk7258_gpio_output() clears BK7258_GPIO_SECOND_FUNCTION as part of
 *   configuring the pad, which matters here because GPIO50's power-on
 *   default in the vendor pinmux table (gpio_map.h:158) is
 *   GPIO_SECOND_FUNC_ENABLE / GPIO_DEV_LCD_R0.
 *
 ****************************************************************************/

void bk7258_audio_pa_enable(bool enable);

#endif /* CONFIG_BK7258_AUDIO */

#endif /* __BOARDS_BEKEN_BK7258_AP_SRC_BK7258_AUDIO_BRINGUP_H */
