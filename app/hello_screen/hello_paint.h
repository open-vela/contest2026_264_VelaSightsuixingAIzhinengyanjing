/****************************************************************************
 * apps/hello_screen/hello_paint.h
 *
 * Pen-stroke renderer for the greeting animation.
 *
 * The interface is arc-length based on purpose: the animation advances a pen
 * along the text's path, so a frame is "draw the path between these two
 * distances" rather than "draw the whole thing at progress p".  That lets the
 * animation add only the newly written stroke each frame instead of clearing
 * and redrawing, which on this board is the difference between a smooth
 * animation and a slideshow -- the panels take ~60ms each to push, and
 * redrawing the whole word every frame would add to that for no visible gain.
 *
 * No dependency on camera_preview's renderer: this module writes RGB565 into
 * caller memory and nothing else, so it can be tested on the host.
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef __APPS_HELLO_SCREEN_HELLO_PAINT_H
#define __APPS_HELLO_SCREEN_HELLO_PAINT_H

#include <stddef.h>
#include <stdint.h>

/****************************************************************************
 * Public Types
 ****************************************************************************/

struct hs_style_s
{
  uint16_t fg;        /* pen colour, RGB565                               */
  uint16_t bg;        /* background, RGB565                               */
  int32_t  em16;      /* em size in 1/16 pixel (the 64-unit glyph box)    */
  int32_t  thick16;   /* pen width in 1/16 pixel                          */
  int32_t  cx16;      /* text centre x in 1/16 pixel                      */
  int32_t  baseline16;/* baseline y in 1/16 pixel                         */
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/* Total pen travel for text, in 1/16 pixel, and its ink width through
 * width16 when that is not NULL.  Returns 0 for text the font cannot draw.
 */

int32_t hs_measure(const char *text, const struct hs_style_s *st,
                   int32_t *width16);

/* Fill the whole canvas with st->bg. */

void hs_clear(uint8_t *buf, size_t stride, int w, int h,
              const struct hs_style_s *st);

/* Draw the part of text's pen path between arc lengths from16 and to16.
 * Passing 0 and hs_measure()'s result draws the complete word.
 */

void hs_stroke(uint8_t *buf, size_t stride, int w, int h,
               const char *text, const struct hs_style_s *st,
               int32_t from16, int32_t to16);

#endif /* __APPS_HELLO_SCREEN_HELLO_PAINT_H */
