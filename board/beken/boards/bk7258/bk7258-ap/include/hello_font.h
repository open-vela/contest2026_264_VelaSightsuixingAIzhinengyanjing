/****************************************************************************
 * apps/hello_screen/hello_font.h
 *
 * Monoline stroke font for the boot greeting.
 *
 * Glyphs are paths, not bitmaps, because the animation reveals the text the
 * way a pen would draw it: the renderer needs to know where the stroke goes
 * and how long it is, which a bitmap cannot tell it.
 *
 * Coordinates are in a per-glyph box: x from 0 to advance, y from 0 (top of
 * the ascender) to 64.  The baseline is y=48 and the x-height top is y=28,
 * so a lowercase bowl is 20 units tall.  int8_t is enough for every value
 * and keeps the table small.
 *
 * Angles are in 1/64 of a turn with y pointing down: 0 is +x (right), 16 is
 * down, 32 is left, 48 is up.  Arcs are drawn from a0 towards a1, so a1 < a0
 * means counter-clockwise on screen.
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef __APPS_HELLO_SCREEN_HELLO_FONT_H
#define __APPS_HELLO_SCREEN_HELLO_FONT_H

#include <stdint.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define HS_EM          64      /* glyph box height, in font units          */
#define HS_BASELINE    48
#define HS_XHEIGHT_TOP 28

enum hs_op_e
{
  HS_END = 0,
  HS_MOVE,               /* a=x  b=y                                       */
  HS_LINE,               /* a=x  b=y                                       */
  HS_ARC                 /* a=cx b=cy c=r  d=a0 e=a1  (1/64 turns)         */
};

struct hs_op_s
{
  int8_t op;
  int8_t a;
  int8_t b;
  int8_t c;
  int8_t d;
  int8_t e;
};

struct hs_glyph_s
{
  char                    ch;
  int8_t                  advance;
  const struct hs_op_s   *ops;
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/* Glyph for ch, or NULL when the font has no such character. */

const struct hs_glyph_s *hs_font_lookup(char ch);

#endif /* __APPS_HELLO_SCREEN_HELLO_FONT_H */
