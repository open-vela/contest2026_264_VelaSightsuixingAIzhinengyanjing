/****************************************************************************
 * board/beken/chips/bk7258/include/bk7258_ldo33.h
 *
 * Reference-counted control of the board's shared 3.3V rail enable
 * (net LDO33_EN).
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef __VENDOR_BEKEN_CHIPS_BK7258_INCLUDE_BK7258_LDO33_H
#define __VENDOR_BEKEN_CHIPS_BK7258_INCLUDE_BK7258_LDO33_H

#include <stdbool.h>
#include <stdint.h>

/* GPIO52 drives the LDO33_EN net (AIDK_AI玩具开发板_原理图.pdf sheet 2/6
 * main-chip pin table, chip pin row "P52/ENET_TXD0/G0" -> net LDO33_EN).
 * That LDO's output, LDO_3V3, is a *board-wide* rail: on sheet 5/6 it
 * feeds the LCD1 connector's VDD (pin 5) and LEDA (pin 4) as well as the
 * touch/NFC/SD-NAND sections.  It is therefore not owned by any single
 * peripheral driver, which is exactly the bug this module exists to fix:
 * bk7258_pwm.c used to call it BK7258_MOTOR_LDO_GPIO and drive it low in
 * its setup path, so by the time the display came up the panel had no
 * VDD and no backlight anode at all.  Every consumer must instead take a
 * reference for as long as it needs the rail.
 */

#define BK7258_LDO33_EN_GPIO 52u

/* Enable the rail if this is the first outstanding reference.  Returns the
 * new reference count.  Safe to call before any other GPIO setup: the
 * first call configures the pad as an output.
 */

unsigned int bk7258_ldo33_request(void);

/* Drop one reference; disables the rail when the count reaches zero.
 * Returns the new reference count.  Dropping a reference that was never
 * taken is a no-op (count stays at 0).
 */

unsigned int bk7258_ldo33_release(void);

/* Current reference count, for diagnostics/tests. */

unsigned int bk7258_ldo33_refcount(void);

#endif /* __VENDOR_BEKEN_CHIPS_BK7258_INCLUDE_BK7258_LDO33_H */
