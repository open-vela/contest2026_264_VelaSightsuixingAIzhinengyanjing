/****************************************************************************
 * apps/hello_screen/hello_font.c
 *
 * The stroke table.  See hello_font.h for the coordinate system.
 *
 * Forms are monoline and rounded rather than a faithful cursive script: on a
 * 160x160 round panel a word is about 90 pixels wide, so a lowercase bowl is
 * ~12 pixels across and the flourishes of a real script hand disappear.  What
 * survives at that size, and what makes the animation read as handwriting, is
 * that each letter is one continuous pen movement drawn in the order a hand
 * would draw it.  That is what this table encodes.
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#include <stddef.h>

#include "hello_font.h"

/****************************************************************************
 * Private Data
 ****************************************************************************/

#define OPS(name) static const struct hs_op_s name[] =

/* Lowercase.  A bowl is the circle (cx=13, cy=38, r=10); stems run from the
 * ascender (y=8) or the x-height (y=28) down to the baseline (y=48).
 */

OPS(g_a)
{
  {HS_ARC, 13, 38, 10, 0, -64}, {HS_MOVE, 23, 28}, {HS_LINE, 23, 48},
  {HS_END}
};

OPS(g_b)
{
  {HS_MOVE, 3, 8}, {HS_LINE, 3, 48}, {HS_ARC, 13, 38, 10, 32, -32},
  {HS_END}
};

OPS(g_c)
{
  {HS_ARC, 13, 38, 10, 12, 52}, {HS_END}
};

OPS(g_d)
{
  {HS_ARC, 13, 38, 10, 0, -64}, {HS_MOVE, 23, 8}, {HS_LINE, 23, 48},
  {HS_END}
};

OPS(g_e)
{
  {HS_MOVE, 3, 38}, {HS_LINE, 23, 38}, {HS_MOVE, 23, 38},
  {HS_ARC, 13, 38, 10, 0, -44},
  {HS_END}
};

OPS(g_f)
{
  {HS_ARC, 13, 16, 8, 32, 56}, {HS_MOVE, 5, 16}, {HS_LINE, 5, 48},
  {HS_MOVE, 0, 28}, {HS_LINE, 14, 28}, {HS_END}
};

OPS(g_g)
{
  {HS_ARC, 13, 38, 10, 0, -64}, {HS_MOVE, 23, 28}, {HS_LINE, 23, 54},
  {HS_ARC, 15, 54, 8, 0, 28}, {HS_END}
};

OPS(g_h)
{
  {HS_MOVE, 3, 8}, {HS_LINE, 3, 48}, {HS_MOVE, 3, 38},
  {HS_ARC, 13, 38, 10, 32, 64}, {HS_MOVE, 23, 38}, {HS_LINE, 23, 48},
  {HS_END}
};

OPS(g_i)
{
  {HS_MOVE, 3, 28}, {HS_LINE, 3, 48}, {HS_MOVE, 3, 20},
  {HS_ARC, 3, 19, 1, 0, 64}, {HS_END}
};

OPS(g_j)
{
  {HS_MOVE, 8, 28}, {HS_LINE, 8, 52}, {HS_ARC, 2, 52, 6, 0, 28},
  {HS_MOVE, 8, 20}, {HS_ARC, 8, 19, 1, 0, 64}, {HS_END}
};

OPS(g_k)
{
  {HS_MOVE, 3, 8}, {HS_LINE, 3, 48}, {HS_MOVE, 15, 28}, {HS_LINE, 3, 40},
  {HS_MOVE, 7, 36}, {HS_LINE, 17, 48}, {HS_END}
};

OPS(g_l)
{
  {HS_MOVE, 3, 8}, {HS_LINE, 3, 44}, {HS_ARC, 8, 44, 5, 32, 16},
  {HS_END}
};

OPS(g_m)
{
  {HS_MOVE, 3, 28}, {HS_LINE, 3, 48}, {HS_MOVE, 3, 34},
  {HS_ARC, 9, 34, 6, 32, 64}, {HS_MOVE, 15, 34}, {HS_LINE, 15, 48},
  {HS_MOVE, 15, 34}, {HS_ARC, 21, 34, 6, 32, 64}, {HS_MOVE, 27, 34},
  {HS_LINE, 27, 48}, {HS_END}
};

OPS(g_n)
{
  {HS_MOVE, 3, 28}, {HS_LINE, 3, 48}, {HS_MOVE, 3, 36},
  {HS_ARC, 11, 36, 8, 32, 64}, {HS_MOVE, 19, 36}, {HS_LINE, 19, 48},
  {HS_END}
};

OPS(g_o)
{
  {HS_ARC, 13, 38, 10, 32, -32}, {HS_END}
};

OPS(g_p)
{
  {HS_MOVE, 3, 28}, {HS_LINE, 3, 58}, {HS_MOVE, 3, 38},
  {HS_ARC, 13, 38, 10, 32, -32}, {HS_END}
};

OPS(g_q)
{
  {HS_ARC, 13, 38, 10, 0, -64}, {HS_MOVE, 23, 28}, {HS_LINE, 23, 58},
  {HS_END}
};

OPS(g_r)
{
  {HS_MOVE, 3, 28}, {HS_LINE, 3, 48}, {HS_MOVE, 3, 36},
  {HS_ARC, 11, 36, 8, 32, 52},
  {HS_END}
};

OPS(g_s)
{
  {HS_ARC, 11, 33, 6, 8, 40}, {HS_ARC, 11, 43, 6, 56, 24}, {HS_END}
};

OPS(g_t)
{
  {HS_MOVE, 7, 16}, {HS_LINE, 7, 43}, {HS_ARC, 12, 43, 5, 32, 16},
  {HS_MOVE, 1, 28}, {HS_LINE, 14, 28}, {HS_END}
};

OPS(g_u)
{
  {HS_MOVE, 3, 28}, {HS_LINE, 3, 40}, {HS_MOVE, 3, 40},
  {HS_ARC, 11, 40, 8, 32, 0},
  {HS_MOVE, 19, 28}, {HS_LINE, 19, 48}, {HS_END}
};

OPS(g_v)
{
  {HS_MOVE, 2, 28}, {HS_LINE, 11, 48}, {HS_LINE, 20, 28}, {HS_END}
};

OPS(g_w)
{
  {HS_MOVE, 2, 28}, {HS_LINE, 8, 48}, {HS_LINE, 14, 32},
  {HS_LINE, 20, 48}, {HS_LINE, 26, 28}, {HS_END}
};

OPS(g_x)
{
  {HS_MOVE, 2, 28}, {HS_LINE, 18, 48}, {HS_MOVE, 18, 28},
  {HS_LINE, 2, 48}, {HS_END}
};

OPS(g_y)
{
  {HS_MOVE, 2, 28}, {HS_LINE, 11, 46}, {HS_MOVE, 20, 28},
  {HS_LINE, 6, 58}, {HS_END}
};

OPS(g_z)
{
  {HS_MOVE, 2, 28}, {HS_LINE, 18, 28}, {HS_LINE, 2, 48},
  {HS_LINE, 18, 48}, {HS_END}
};

OPS(g_excl)
{
  {HS_MOVE, 3, 20}, {HS_LINE, 3, 40}, {HS_MOVE, 3, 47},
  {HS_ARC, 3, 46, 1, 0, 64}, {HS_END}
};

OPS(g_comma)
{
  {HS_MOVE, 4, 44}, {HS_ARC, 1, 47, 4, 48, 20}, {HS_END}
};

OPS(g_period)
{
  {HS_MOVE, 3, 47}, {HS_ARC, 3, 46, 1, 0, 64}, {HS_END}
};

OPS(g_dash)
{
  {HS_MOVE, 2, 38}, {HS_LINE, 16, 38}, {HS_END}
};

OPS(g_space)
{
  {HS_END}
};

static const struct hs_glyph_s g_glyphs[] =
{
  {'a', 28, g_a}, {'b', 28, g_b}, {'c', 26, g_c}, {'d', 28, g_d},
  {'e', 27, g_e}, {'f', 18, g_f}, {'g', 28, g_g}, {'h', 28, g_h},
  {'i', 10, g_i}, {'j', 14, g_j}, {'k', 22, g_k}, {'l', 16, g_l},
  {'m', 32, g_m}, {'n', 24, g_n}, {'o', 27, g_o}, {'p', 28, g_p},
  {'q', 28, g_q}, {'r', 18, g_r}, {'s', 22, g_s}, {'t', 18, g_t},
  {'u', 24, g_u}, {'v', 24, g_v}, {'w', 30, g_w}, {'x', 22, g_x},
  {'y', 24, g_y}, {'z', 22, g_z},
  {'!', 10, g_excl}, {',', 8, g_comma}, {'.', 8, g_period},
  {'-', 20, g_dash}, {' ', 14, g_space}
};

#define NGLYPHS ((int)(sizeof(g_glyphs) / sizeof(g_glyphs[0])))

/****************************************************************************
 * Public Functions
 ****************************************************************************/

const struct hs_glyph_s *hs_font_lookup(char ch)
{
  int i;

  /* Uppercase folds to lowercase: the font is monoline and single-case, and
   * silently dropping 'H' from "Hello" would be worse than drawing 'h'.
   */

  if (ch >= 'A' && ch <= 'Z')
    {
      ch = (char)(ch - 'A' + 'a');
    }

  for (i = 0; i < NGLYPHS; i++)
    {
      if (g_glyphs[i].ch == ch)
        {
          return &g_glyphs[i];
        }
    }

  return NULL;
}
