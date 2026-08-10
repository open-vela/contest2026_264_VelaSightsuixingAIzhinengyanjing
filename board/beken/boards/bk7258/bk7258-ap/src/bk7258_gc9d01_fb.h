/****************************************************************************
 * board/beken/boards/bk7258/bk7258-ap/src/bk7258_gc9d01_fb.h
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef __BOARDS_BEKEN_BK7258_AP_SRC_BK7258_GC9D01_FB_H
#define __BOARDS_BEKEN_BK7258_AP_SRC_BK7258_GC9D01_FB_H

#include <stdint.h>

/* Panel reset, QSPI bring-up and the GC9D01 initialization command
 * sequence.  Implemented in bk7258_gc9d01.c, which owns the command table.
 * Returns OK or a negated errno.
 */

int bk7258_gc9d01_panel_init(void);

/* Bring-up helpers: fill the framebuffer with one colour, or draw a
 * four-quadrant test pattern, and push it to the panel.  These exist
 * because GC9D01 has no readable ID register -- until something is
 * visible on the glass there is no evidence the init sequence took
 * effect.
 */

int bk7258_gc9d01_fb_fill(uint16_t rgb565);
int bk7258_gc9d01_fb_test_pattern(void);

#endif /* __BOARDS_BEKEN_BK7258_AP_SRC_BK7258_GC9D01_FB_H */
