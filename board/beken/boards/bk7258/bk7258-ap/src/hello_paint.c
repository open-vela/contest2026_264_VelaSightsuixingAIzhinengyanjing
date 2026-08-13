/****************************************************************************
 * apps/hello_screen/hello_paint.c
 *
 * Pen-stroke renderer.  See hello_paint.h for why the interface is
 * arc-length based.
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <arch/board/hello_font.h>
#include <arch/board/hello_paint.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define FP        16      /* subpixel units per pixel                      */
#define SUBN      3       /* 3x3 supersampling for the pen edge            */

/* One tessellation segment per 1/32 turn keeps an arc's chord error under a
 * tenth of a pixel at these sizes.
 */

#define ARC_STEP  2

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* sin(2*pi*i/64) * 256, y pointing down: index 0 is +x, 16 is down. */

static const int16_t g_sin64[64] =
{
      0,    25,    50,    74,    98,   121,   142,   162,
    181,   198,   213,   226,   237,   245,   251,   255,
    256,   255,   251,   245,   237,   226,   213,   198,
    181,   162,   142,   121,    98,    74,    50,    25,
      0,   -25,   -50,   -74,   -98,  -121,  -142,  -162,
   -181,  -198,  -213,  -226,  -237,  -245,  -251,  -255,
   -256,  -255,  -251,  -245,  -237,  -226,  -213,  -198,
   -181,  -162,  -142,  -121,   -98,   -74,   -50,   -25
};

/* Subsample hit count to alpha.  Symmetric about FP/2 so a stroke's left and
 * right edges get identical coverage.
 */

static const int16_t g_cov_alpha[SUBN * SUBN + 1] =
{
  0, 28, 57, 85, 114, 142, 171, 199, 228, 256
};

static const int32_t g_sub_off[SUBN] =
{
  3, FP / 2, FP - 3
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static uint32_t isqrt32(uint32_t v)
{
  uint32_t rem = 0;
  uint32_t root = 0;
  int i;

  for (i = 0; i < 16; i++)
    {
      root <<= 1;
      rem = (rem << 2) | (v >> 30);
      v <<= 2;

      if (root < rem)
        {
          rem -= root | 1;
          root += 2;
        }
    }

  return root >> 1;
}

static uint16_t blend565(uint16_t dst, uint16_t src, int32_t alpha)
{
  int32_t ia = 256 - alpha;
  int32_t r = (((src >> 11) & 0x1f) * alpha + ((dst >> 11) & 0x1f) * ia) >> 8;
  int32_t g = (((src >> 5) & 0x3f) * alpha + ((dst >> 5) & 0x3f) * ia) >> 8;
  int32_t b = ((src & 0x1f) * alpha + (dst & 0x1f) * ia) >> 8;

  return (uint16_t)((r << 11) | (g << 5) | b);
}

struct hs_canvas_s
{
  uint8_t *buf;
  size_t   stride;
  int      w;
  int      h;
};

static void hs_px(struct hs_canvas_s *cv, int x, int y, uint16_t colour,
                  int32_t alpha)
{
  uint16_t *p;

  if (alpha <= 0 || x < 0 || y < 0 || x >= cv->w || y >= cv->h)
    {
      return;
    }

  p = (uint16_t *)(void *)(cv->buf + (size_t)y * cv->stride) + x;
  *p = alpha >= 256 ? colour : blend565(*p, colour, alpha);
}

/****************************************************************************
 * Name: hs_seg
 *
 * Description:
 *   Round-capped thick segment, anti-aliased by 3x3 subsampling of the
 *   distance to the segment.  Round caps matter here: the path is drawn in
 *   pieces as the animation advances, and square caps would leave visible
 *   notches at the joins between one frame's stroke and the next.
 *
 ****************************************************************************/

static void hs_seg(struct hs_canvas_s *cv, int32_t ax, int32_t ay,
                   int32_t bx, int32_t by, int32_t thick, uint16_t colour)
{
  int32_t r = thick / 2;
  int32_t dx = bx - ax;
  int32_t dy = by - ay;
  int32_t len2 = dx * dx + dy * dy;
  int32_t rlen = (int32_t)((int64_t)r * isqrt32((uint32_t)len2));
  int x0;
  int x1;
  int y0;
  int y1;
  int x;
  int y;

  if (r <= 0)
    {
      return;
    }

  x0 = (int)(((ax < bx ? ax : bx) - r) / FP) - 1;
  x1 = (int)(((ax > bx ? ax : bx) + r) / FP) + 1;
  y0 = (int)(((ay < by ? ay : by) - r) / FP) - 1;
  y1 = (int)(((ay > by ? ay : by) + r) / FP) + 1;

  if (x0 < 0)
    {
      x0 = 0;
    }

  if (y0 < 0)
    {
      y0 = 0;
    }

  if (x1 >= cv->w)
    {
      x1 = cv->w - 1;
    }

  if (y1 >= cv->h)
    {
      y1 = cv->h - 1;
    }

  for (y = y0; y <= y1; y++)
    {
      for (x = x0; x <= x1; x++)
        {
          int cov = 0;
          int i;
          int j;

          for (j = 0; j < SUBN; j++)
            {
              for (i = 0; i < SUBN; i++)
                {
                  int32_t sx = (int32_t)x * FP + g_sub_off[i];
                  int32_t sy = (int32_t)y * FP + g_sub_off[j];
                  int32_t px = sx - ax;
                  int32_t py = sy - ay;
                  int32_t dot = px * dx + py * dy;
                  int32_t d2;

                  if (len2 == 0 || dot <= 0)
                    {
                      d2 = px * px + py * py;
                    }
                  else if (dot >= len2)
                    {
                      int32_t qx = sx - bx;
                      int32_t qy = sy - by;

                      d2 = qx * qx + qy * qy;
                    }
                  else
                    {
                      int32_t cr = px * dy - py * dx;

                      d2 = -1;
                      if (cr <= rlen && -cr <= rlen)
                        {
                          cov++;
                        }
                    }

                  if (d2 >= 0 && d2 <= r * r)
                    {
                      cov++;
                    }
                }
            }

          if (cov > 0)
            {
              hs_px(cv, x, y, colour, g_cov_alpha[cov]);
            }
        }
    }
}

static int32_t hs_len(int32_t ax, int32_t ay, int32_t bx, int32_t by)
{
  int32_t dx = bx - ax;
  int32_t dy = by - ay;

  return (int32_t)isqrt32((uint32_t)(dx * dx + dy * dy));
}

/****************************************************************************
 * Name: hs_walk
 *
 * Description:
 *   Single traversal of the text's pen path.  When cv is NULL it only
 *   measures; otherwise it draws the part of the path that falls between
 *   from16 and to16.  Measuring and drawing share this function so the two
 *   can never disagree about where the pen is at a given distance.
 *
 ****************************************************************************/

static int32_t hs_walk(struct hs_canvas_s *cv, const char *text,
                       const struct hs_style_s *st, int32_t from16,
                       int32_t to16, int32_t *width16)
{
  int32_t pen_x = 0;                       /* glyph origin, font units     */
  int32_t travelled = 0;
  int32_t x0 = 0;
  int32_t y0 = 0;
  bool    have_point = false;
  const char *p;

  /* Ink extent is needed before the first stroke can be placed, so the
   * caller measures once with width16 and then draws with a centred style.
   */

  for (p = text; *p != '\0'; p++)
    {
      const struct hs_glyph_s *g = hs_font_lookup(*p);
      const struct hs_op_s *op;

      if (g == NULL)
        {
          continue;
        }

      for (op = g->ops; op->op != HS_END; op++)
        {
          int32_t pts[2 * (64 / ARC_STEP + 2)];
          int     npts = 0;
          int     k;

          if (op->op == HS_MOVE)
            {
              x0 = pen_x + op->a;
              y0 = op->b;
              have_point = true;
              continue;
            }

          if (!have_point)
            {
              /* A path that starts with an arc: begin at its start point. */

              if (op->op != HS_ARC)
                {
                  continue;
                }
            }

          if (op->op == HS_LINE)
            {
              pts[0] = pen_x + op->a;
              pts[1] = op->b;
              npts = 1;
            }
          else
            {
              int32_t a = op->d;
              int32_t b = op->e;
              int32_t step = a <= b ? ARC_STEP : -ARC_STEP;
              int32_t ang;

              for (ang = a; ; ang += step)
                {
                  int idx = (int)(((ang % 64) + 64) % 64);

                  if (npts >= (int)(sizeof(pts) / sizeof(pts[0])) / 2)
                    {
                      break;
                    }

                  pts[2 * npts] = pen_x + op->a +
                                  (op->c * g_sin64[(idx + 16) & 63]) / 256;
                  pts[2 * npts + 1] = op->b +
                                      (op->c * g_sin64[idx]) / 256;
                  npts++;

                  if ((step > 0 && ang >= b) || (step < 0 && ang <= b))
                    {
                      break;
                    }
                }

              if (!have_point && npts > 0)
                {
                  x0 = pts[0];
                  y0 = pts[1];
                  have_point = true;
                }
            }

          for (k = 0; k < npts; k++)
            {
              int32_t x1 = pts[2 * k];
              int32_t y1 = pts[2 * k + 1];
              int32_t ax = st->cx16 + (x0 * st->em16) / HS_EM;
              int32_t ay = st->baseline16 +
                           ((y0 - HS_BASELINE) * st->em16) / HS_EM;
              int32_t bx = st->cx16 + (x1 * st->em16) / HS_EM;
              int32_t by = st->baseline16 +
                           ((y1 - HS_BASELINE) * st->em16) / HS_EM;
              int32_t seg = hs_len(ax, ay, bx, by);

              if (width16 != NULL)
                {
                  if (bx > *width16)
                    {
                      *width16 = bx;
                    }

                  if (ax > *width16)
                    {
                      *width16 = ax;
                    }
                }

              if (cv != NULL && seg > 0 && travelled + seg > from16 &&
                  travelled < to16)
                {
                  int32_t s = from16 > travelled ? from16 - travelled : 0;
                  int32_t e = to16 < travelled + seg ? to16 - travelled : seg;
                  int32_t sx = ax + ((bx - ax) * s) / seg;
                  int32_t sy = ay + ((by - ay) * s) / seg;
                  int32_t ex = ax + ((bx - ax) * e) / seg;
                  int32_t ey = ay + ((by - ay) * e) / seg;

                  hs_seg(cv, sx, sy, ex, ey, st->thick16, st->fg);
                }

              travelled += seg;
              x0 = x1;
              y0 = y1;
            }
        }

      pen_x += g->advance;
      have_point = false;
    }

  return travelled;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int32_t hs_ease(int32_t permille)
{
  int64_t x = permille;

  if (x < 0)
    {
      x = 0;
    }

  if (x > 1000)
    {
      x = 1000;
    }

  return (int32_t)((x * x * (3000 - 2 * x)) / 1000000);
}

int32_t hs_measure(const char *text, const struct hs_style_s *st,
                   int32_t *width16)
{
  struct hs_style_s probe = *st;
  int32_t right = 0;
  int32_t total;

  probe.cx16 = 0;
  total = hs_walk(NULL, text, &probe, 0, 0, &right);

  if (width16 != NULL)
    {
      *width16 = right;
    }

  return total;
}

void hs_clear(uint8_t *buf, size_t stride, int w, int h,
              const struct hs_style_s *st)
{
  uint32_t pair = ((uint32_t)st->bg << 16) | st->bg;
  int y;

  for (y = 0; y < h; y++)
    {
      uint32_t *row = (uint32_t *)(void *)(buf + (size_t)y * stride);
      int n = w / 2;
      int x;

      for (x = 0; x < n; x++)
        {
          row[x] = pair;
        }

      if (w & 1)
        {
          ((uint16_t *)(void *)row)[w - 1] = st->bg;
        }
    }
}

void hs_stroke(uint8_t *buf, size_t stride, int w, int h,
               const char *text, const struct hs_style_s *st,
               int32_t from16, int32_t to16)
{
  struct hs_canvas_s cv;

  if (buf == NULL || w <= 0 || h <= 0 || to16 <= from16)
    {
      return;
    }

  cv.buf    = buf;
  cv.stride = stride;
  cv.w      = w;
  cv.h      = h;

  hs_walk(&cv, text, st, from16, to16, NULL);
}
