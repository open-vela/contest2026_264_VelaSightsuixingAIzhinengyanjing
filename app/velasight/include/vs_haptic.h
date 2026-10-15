/****************************************************************************
 * app/velasight/include/vs_haptic.h
 *
 * The vibration motor, for feedback the wearer feels rather than reads.
 *
 * Mounted on the temple the screen faces sideways, away from the wearer, so
 * an emotion alert that only changes the display reaches everyone except the
 * person it is for.  A short buzz is what tells them to look up.
 *
 * Patterns run on the high-priority work queue, one step per work item, so
 * nothing that asks for one ever waits for it: the UI thread that raises an
 * alert must not be held for the quarter-second the motor takes.  HPWORK
 * rather than LPWORK because the low-priority queue also runs the camera's
 * JPEG entropy validator during a session, measured at 252 ms a frame, and a
 * pulse queued behind that would run three times its length.
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef __APP_VELASIGHT_INCLUDE_VS_HAPTIC_H
#define __APP_VELASIGHT_INCLUDE_VS_HAPTIC_H

#include <stdbool.h>

/* Start count pulses of on_ms, off_ms apart, and return at once.  -EBUSY
 * while a pattern is still running (the request is dropped, not queued),
 * -EINVAL for a pattern outside 1..5 pulses of 20..1000 ms.
 */

int vs_haptic_pulse(unsigned int count, unsigned int on_ms,
                    unsigned int off_ms);

/* The emotion-alert pattern, CONFIG_VS_HAPTIC_ALERT_*, at most once per
 * CONFIG_VS_HAPTIC_ALERT_COOLDOWN_MS.  -EALREADY inside the cooldown,
 * -ENOSYS when the feature is configured out.  From the UI thread only.
 */

int vs_haptic_alert(void);

/* Whether a pattern is running. */

bool vs_haptic_busy(void);

#endif /* __APP_VELASIGHT_INCLUDE_VS_HAPTIC_H */
