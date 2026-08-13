/****************************************************************************
 * apps/camera_preview/preview_face.c
 *
 * Procedurally drawn faces for the two GC9D01 round panels.
 *
 * Why procedural and not bitmaps
 * ------------------------------
 * One RGB565 frame is 51200 bytes.  An eight-frame blink stored as bitmaps
 * would be 400KB, and the ai_agent configuration already spends
 * 604992 of the AP's 1088KB of flash.  A parameter table costs about a
 * kilobyte and animates by interpolating numbers, so that is what this is.
 *
 * Anti-aliasing
 * -------------
 * Every shape supplies a boolean inside() test and the drawing loop takes
 * 3x3 subsamples per pixel, turning the hit count into an alpha.  The
 * alternative -- an analytic coverage function per shape -- would be faster
 * but needs a correct distance function for ellipses, parabolic arcs,
 * spirals, stars and the heart curve.  Getting one of those wrong produces a
 * plausible-looking but subtly deformed shape, which is exactly the class of
 * bug that is expensive to find on a 160x160 round panel.  Nine integer
 * predicate evaluations are cheap by comparison.
 *
 * Round glass
 * -----------
 * The visible area is the inscribed circle, r=80 about (80,80).  The corners
 * are behind the bezel, so the background is filled as a plain rectangle and
 * every element is placed inside the circle; test_face.c asserts that no
 * element pixel ever lands outside it, because such a pixel is work whose
 * result nobody can see.
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

#include "preview_face.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Subpixel resolution used inside the renderer.  Face parameters are in
 * whole pixels; everything is scaled by FP on the way in.
 */

#define FP              16
#define FPX(v)          ((int32_t)(v) * FP)

/* 3x3 supersampling.  Offsets are the subpixel centres of the three bands,
 * 1/6, 3/6 and 5/6 of a pixel.
 */

#define SUBN            3

/* Panel geometry.  The renderer is told the real size by the caller and
 * scales the table to it, but the table itself is authored for 160x160.
 */

#define FACE_REF_W      160
#define FACE_REF_H      160

/* RGB565 palette.  Named so the expression table reads as intent rather
 * than hex.
 */

#define C_BLACK         0x0000
#define C_WHITE         0xffff
#define C_CYAN          0x07ff
#define C_MOUTH         0x8000    /* dark red mouth cavity                 */
#define C_PINK          0xfa94    /* blush, tongue                         */
#define C_TEAR          0x87ff    /* light blue                            */
#define C_YELLOW        0xffe0
#define C_RED           0xf9c4    /* heart eyes                            */

/* Eye styles. */

enum eye_style_e
{
  EYE_ROUND = 0,    /* sclera + iris + pupil + specular, lid_* closes it   */
  EYE_CURVE,        /* thick upward arc, the ^ ^ smiling eye               */
  EYE_LINE,         /* thick horizontal bar, a closed eye                  */
  EYE_ANGRY,        /* two segments meeting at the inner corner            */
  EYE_X,            /* two crossing strokes                                */
  EYE_HEART,        /* filled heart curve                                  */
  EYE_STAR,         /* filled five-pointed star                            */
  EYE_SPIRAL        /* Archimedean spiral, the dizzy eye                   */
};

/* Mouth styles. */

enum mouth_style_e
{
  MOUTH_ARC = 0,    /* parabolic stroke, curve > 0 smiles                  */
  MOUTH_OPEN,       /* filled cavity + outline + optional teeth and tongue */
  MOUTH_LINE,       /* straight bar                                        */
  MOUTH_TEETH,      /* gritted teeth                                       */
  MOUTH_WAVE,       /* wavy stroke                                         */
  MOUTH_O           /* ring                                                */
};

/* Expression flags. */

#define FACE_SPIN       (1u << 0) /* rotate EYE_SPIRAL/EYE_STAR with phase */

/****************************************************************************
 * Private Types
 ****************************************************************************/

/* A face is a set of fixed-point parameters.
 *
 * The block from eye_dx to (but not including) fg is what animation
 * interpolates, and it is walked as an int16_t array -- see face_lerp().
 * Anything added inside that range is interpolated automatically; anything
 * that must NOT be interpolated (styles, colours) has to stay outside it.
 */

struct face_s
{
  uint8_t  eye_style;
  uint8_t  mouth_style;

  int16_t  eye_dx;        /* eye centre offset from the face centre        */
  int16_t  eye_y;
  int16_t  eye_rx;
  int16_t  eye_ry;
  int16_t  lid_l;         /* upper lid travel, 0 open, 2*eye_ry closed     */
  int16_t  lid_r;
  int16_t  eye_curve;     /* EYE_CURVE arc height                          */
  int16_t  iris_r;
  int16_t  pupil_r;
  int16_t  spin;          /* rotation phase, 256 = full turn               */
  int16_t  mouth_y;
  int16_t  mouth_w;
  int16_t  mouth_curve;   /* > 0 smiles, < 0 frowns                        */
  int16_t  mouth_open;    /* cavity height                                 */
  int16_t  mouth_thick;
  int16_t  tongue;        /* tongue height, 0 for none                     */
  int16_t  brow_len;      /* 0 draws no brows                              */
  int16_t  brow_tilt;     /* > 0 inner end low (angry), < 0 inner high     */
  int16_t  brow_y;
  int16_t  brow_thick;
  int16_t  tear;          /* 0 none, 1..1000 fall progress                 */
  int16_t  blush;         /* 0..255                                        */

  uint16_t fg;
  uint16_t bg;
  uint16_t sclera;
  uint16_t iris;
  uint16_t mouth_fill;
};

#define FACE_NUM_OFF    offsetof(struct face_s, eye_dx)
#define FACE_NUM_END    offsetof(struct face_s, fg)
#define FACE_NUM_COUNT  ((FACE_NUM_END - FACE_NUM_OFF) / sizeof(int16_t))

/* One keyframe.  hold_ms is the duration of the segment that starts at this
 * keyframe and ends at the next one, wrapping around to keyframe 0.  A
 * static expression is a single keyframe.
 *
 * "Hold then move" is expressed by repeating a keyframe: two identical
 * keyframes give a segment with no visible change.  That is why there is no
 * separate transition-time field.
 */

struct face_key_s
{
  struct face_s f;
  uint16_t      hold_ms;
};

struct expression_s
{
  const char               *name;
  const struct face_key_s  *keys;
  uint8_t                   nkeys;
  uint8_t                   flags;
};

/* Destination for the renderer. */

struct canvas_s
{
  uint8_t *buf;
  size_t   stride;
  int      w;
  int      h;
};

/* Primitive kinds.  cx/cy/rx/ry are subpixel; rx/ry are bounding half
 * extents, which is what the drawing loop uses to size the box it scans.
 */

enum prim_kind_e
{
  PRIM_ELLIPSE = 0, /* -                                                   */
  PRIM_RING,        /* p0 = thickness                                      */
  PRIM_ARC,         /* p0 = curve, p1 = thickness                          */
  PRIM_RRECT,       /* p0 = corner radius                                  */
  PRIM_WAVE,        /* p0 = amplitude, p1 = thickness, p2 = periods        */
  PRIM_TAPER,       /* p0 = height, <0 points down; p1 = base half width   */
  PRIM_POLY         /* pts = x,y pairs in SUBPIXELS, npts = count          */
};

struct prim_s
{
  uint8_t        kind;
  int32_t        cx;
  int32_t        cy;
  int32_t        rx;
  int32_t        ry;
  int32_t        p0;
  int32_t        p1;
  int32_t        p2;
  const int16_t *pts;
  int            npts;
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* sin(2*pi*i/64) * 256.  Index 0..63 is a full turn; cos is sin(i+16). */

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

/* Subsample hit count (0..9) to alpha (0..256). */

static const int16_t g_cov_alpha[SUBN * SUBN + 1] =
{
  0, 28, 57, 85, 114, 142, 171, 199, 228, 256
};

/* Subpixel positions of the three subsample bands within a pixel.
 *
 * These must be symmetric about FP/2, i.e. FP - off must also be in the set.
 * Otherwise the coverage of a shape's left edge differs from its mirrored
 * right edge and the face comes out subtly lopsided.  The first version of
 * this table was written as {FP/6, FP/2, FP*5/6}, which integer-divides to
 * {2, 8, 13}: 16-2 = 14 is not in the set, and test_face's symmetry check
 * failed on every expression by one coverage step per edge.
 */

static const int32_t g_sub_off[SUBN] =
{
  3, FP / 2, FP - 3
};

/* Fields shared by every expression.  Individual keyframes override what
 * they need; the build has no -Wextra, so -Woverride-init is not in play.
 */

#define FACE_BASE                                                          \
  .eye_dx = 30, .eye_y = 62, .eye_rx = 20, .eye_ry = 22,                   \
  .iris_r = 13, .pupil_r = 6,                                              \
  .mouth_y = 112, .mouth_w = 44, .mouth_thick = 6,                         \
  .brow_y = 32, .brow_thick = 5,                                           \
  .fg = C_WHITE, .bg = C_BLACK, .sclera = C_WHITE,                         \
  .iris = C_CYAN, .mouth_fill = C_MOUTH

/* ---- static expressions ---- */

static const struct face_key_s g_k_neutral[] =
{
  {
    {
      FACE_BASE,
      .eye_style = EYE_ROUND, .mouth_style = MOUTH_LINE,
      .mouth_w = 36
    },
    0
  }
};

static const struct face_key_s g_k_smile[] =
{
  {
    {
      FACE_BASE,
      .eye_style = EYE_ROUND, .mouth_style = MOUTH_ARC,
      .mouth_curve = 11, .blush = 70
    },
    0
  }
};

static const struct face_key_s g_k_grin[] =
{
  {
    {
      FACE_BASE,
      .eye_style = EYE_CURVE, .eye_curve = 15,
      .mouth_style = MOUTH_OPEN, .mouth_w = 54, .mouth_open = 28,
      .blush = 100
    },
    0
  }
};

static const struct face_key_s g_k_sad[] =
{
  {
    {
      FACE_BASE,
      .eye_style = EYE_ROUND, .lid_l = 14, .lid_r = 14,
      .mouth_style = MOUTH_ARC, .mouth_curve = -11,
      .brow_len = 26, .brow_tilt = -7
    },
    0
  }
};

static const struct face_key_s g_k_angry[] =
{
  {
    {
      FACE_BASE,
      .eye_style = EYE_ANGRY,
      .mouth_style = MOUTH_TEETH, .mouth_w = 50, .mouth_open = 22,
      .brow_len = 28, .brow_tilt = 8
    },
    0
  }
};

static const struct face_key_s g_k_surprise[] =
{
  {
    {
      FACE_BASE,
      .eye_style = EYE_ROUND, .eye_rx = 24, .eye_ry = 26,
      .iris_r = 11, .pupil_r = 4,
      .mouth_style = MOUTH_O, .mouth_w = 28, .mouth_open = 30,
      .mouth_thick = 5,
      .brow_len = 24, .brow_tilt = -3, .brow_y = 26
    },
    0
  }
};

static const struct face_key_s g_k_meh[] =
{
  {
    {
      FACE_BASE,
      .eye_style = EYE_LINE, .eye_rx = 18, .mouth_style = MOUTH_WAVE,
      .mouth_w = 44, .mouth_open = 5, .mouth_thick = 5
    },
    0
  }
};

static const struct face_key_s g_k_tongue[] =
{
  {
    {
      FACE_BASE,
      .eye_style = EYE_CURVE, .eye_curve = 13,
      .mouth_style = MOUTH_OPEN, .mouth_w = 46, .mouth_open = 22,
      .tongue = 14, .blush = 90
    },
    0
  }
};

/* ---- animated expressions ---- */

/* 哭笑: curved laughing eyes, wide open mouth, and a tear running down each
 * cheek.  The tear restarts every loop, which is what makes it read as
 * laughing-until-crying rather than simply crying.
 */

static const struct face_key_s g_k_laughcry[] =
{
  {
    {
      FACE_BASE,
      .eye_style = EYE_CURVE, .eye_curve = 15,
      .mouth_style = MOUTH_OPEN, .mouth_w = 54, .mouth_open = 30,
      .blush = 120, .tear = 1
    },
    170
  },
  {
    {
      FACE_BASE,
      .eye_style = EYE_CURVE, .eye_curve = 18,
      .mouth_style = MOUTH_OPEN, .mouth_w = 56, .mouth_open = 34,
      .blush = 140, .tear = 340
    },
    170
  },
  {
    {
      FACE_BASE,
      .eye_style = EYE_CURVE, .eye_curve = 15,
      .mouth_style = MOUTH_OPEN, .mouth_w = 54, .mouth_open = 30,
      .blush = 120, .tear = 670
    },
    170
  },
  {
    {
      FACE_BASE,
      .eye_style = EYE_CURVE, .eye_curve = 13,
      .mouth_style = MOUTH_OPEN, .mouth_w = 52, .mouth_open = 26,
      .blush = 110, .tear = 1000
    },
    170
  }
};

/* Two identical keyframes hold the eye open, then two short segments close
 * and reopen it.
 */

static const struct face_key_s g_k_blink[] =
{
  {
    {
      FACE_BASE, .eye_style = EYE_ROUND, .mouth_style = MOUTH_ARC,
      .mouth_curve = 8
    },
    800
  },
  {
    {
      FACE_BASE, .eye_style = EYE_ROUND, .mouth_style = MOUTH_ARC,
      .mouth_curve = 8
    },
    70
  },
  {
    {
      FACE_BASE, .eye_style = EYE_ROUND, .lid_l = 44, .lid_r = 44,
      .mouth_style = MOUTH_ARC, .mouth_curve = 8
    },
    70
  }
};

static const struct face_key_s g_k_wink[] =
{
  {
    {
      FACE_BASE, .eye_style = EYE_ROUND, .mouth_style = MOUTH_ARC,
      .mouth_curve = 12, .blush = 80
    },
    700
  },
  {
    {
      FACE_BASE, .eye_style = EYE_ROUND, .mouth_style = MOUTH_ARC,
      .mouth_curve = 12, .blush = 80
    },
    80
  },
  {
    {
      FACE_BASE, .eye_style = EYE_ROUND, .lid_l = 44,
      .mouth_style = MOUTH_ARC, .mouth_curve = 12, .blush = 80
    },
    500
  },
  {
    {
      FACE_BASE, .eye_style = EYE_ROUND, .lid_l = 44,
      .mouth_style = MOUTH_ARC, .mouth_curve = 12, .blush = 80
    },
    80
  }
};

static const struct face_key_s g_k_sleepy[] =
{
  {
    {
      FACE_BASE, .eye_style = EYE_ROUND, .lid_l = 26, .lid_r = 26,
      .mouth_style = MOUTH_WAVE, .mouth_open = 4, .mouth_thick = 5
    },
    600
  },
  {
    {
      FACE_BASE, .eye_style = EYE_ROUND, .lid_l = 26, .lid_r = 26,
      .mouth_style = MOUTH_WAVE, .mouth_open = 4, .mouth_thick = 5
    },
    400
  },
  {
    {
      FACE_BASE, .eye_style = EYE_ROUND, .lid_l = 44, .lid_r = 44,
      .mouth_style = MOUTH_WAVE, .mouth_open = 4, .mouth_thick = 5
    },
    900
  },
  {
    {
      FACE_BASE, .eye_style = EYE_ROUND, .lid_l = 44, .lid_r = 44,
      .mouth_style = MOUTH_WAVE, .mouth_open = 4, .mouth_thick = 5
    },
    400
  }
};

/* The spiral rotates with the animation phase (FACE_SPIN), so the keyframes
 * only have to make the mouth wobble.
 */

static const struct face_key_s g_k_dizzy[] =
{
  {
    {
      FACE_BASE, .eye_style = EYE_SPIRAL, .eye_rx = 21, .eye_ry = 21,
      .mouth_style = MOUTH_WAVE, .mouth_open = 6, .mouth_thick = 5,
      .mouth_w = 40
    },
    240
  },
  {
    {
      FACE_BASE, .eye_style = EYE_SPIRAL, .eye_rx = 21, .eye_ry = 21,
      .mouth_style = MOUTH_WAVE, .mouth_open = -6, .mouth_thick = 5,
      .mouth_w = 40
    },
    240
  }
};

static const struct face_key_s g_k_ko[] =
{
  {
    {
      FACE_BASE, .eye_style = EYE_X, .eye_dx = 30, .eye_rx = 17,
      .eye_ry = 17, .mouth_style = MOUTH_TEETH, .mouth_w = 48,
      .mouth_open = 22
    },
    90
  },
  {
    {
      FACE_BASE, .eye_style = EYE_X, .eye_dx = 33, .eye_rx = 17,
      .eye_ry = 17, .mouth_style = MOUTH_TEETH, .mouth_w = 48,
      .mouth_open = 22
    },
    90
  },
  {
    {
      FACE_BASE, .eye_style = EYE_X, .eye_dx = 30, .eye_rx = 17,
      .eye_ry = 17, .mouth_style = MOUTH_TEETH, .mouth_w = 48,
      .mouth_open = 22
    },
    90
  },
  {
    {
      FACE_BASE, .eye_style = EYE_X, .eye_dx = 27, .eye_rx = 17,
      .eye_ry = 17, .mouth_style = MOUTH_TEETH, .mouth_w = 48,
      .mouth_open = 22
    },
    90
  }
};

static const struct face_key_s g_k_love[] =
{
  {
    {
      FACE_BASE, .eye_style = EYE_HEART, .eye_rx = 19, .eye_ry = 18,
      .iris = C_RED, .mouth_style = MOUTH_ARC, .mouth_curve = 13,
      .blush = 190
    },
    260
  },
  {
    {
      FACE_BASE, .eye_style = EYE_HEART, .eye_rx = 23, .eye_ry = 22,
      .iris = C_RED, .mouth_style = MOUTH_ARC, .mouth_curve = 15,
      .blush = 220
    },
    260
  }
};

static const struct face_key_s g_k_star[] =
{
  {
    {
      FACE_BASE, .eye_style = EYE_STAR, .eye_rx = 21, .eye_ry = 21,
      .iris = C_YELLOW, .mouth_style = MOUTH_OPEN, .mouth_w = 48,
      .mouth_open = 24, .blush = 120
    },
    200
  },
  {
    {
      FACE_BASE, .eye_style = EYE_STAR, .eye_rx = 23, .eye_ry = 23,
      .iris = C_YELLOW, .mouth_style = MOUTH_OPEN, .mouth_w = 50,
      .mouth_open = 28, .blush = 140
    },
    200
  }
};

#define EXPR(n, k, fl) { n, k, (uint8_t)(sizeof(k) / sizeof((k)[0])), fl }

static const struct expression_s g_expr[] =
{
  EXPR("neutral",  g_k_neutral,  0),
  EXPR("smile",    g_k_smile,    0),
  EXPR("grin",     g_k_grin,     0),
  EXPR("sad",      g_k_sad,      0),
  EXPR("angry",    g_k_angry,    0),
  EXPR("surprise", g_k_surprise, 0),
  EXPR("meh",      g_k_meh,      0),
  EXPR("tongue",   g_k_tongue,   0),
  EXPR("laughcry", g_k_laughcry, 0),
  EXPR("blink",    g_k_blink,    0),
  EXPR("wink",     g_k_wink,     0),
  EXPR("sleepy",   g_k_sleepy,   0),
  EXPR("dizzy",    g_k_dizzy,    FACE_SPIN),
  EXPR("ko",       g_k_ko,       0),
  EXPR("love",     g_k_love,     0),
  EXPR("star",     g_k_star,     FACE_SPIN)
};

#define EXPR_COUNT ((int)(sizeof(g_expr) / sizeof(g_expr[0])))

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: blend565
 *
 * Description:
 *   Alpha blend src over dst, alpha 0..256.  Channels are unpacked first:
 *   interpolating the packed halfword directly would carry between fields.
 *
 ****************************************************************************/

static uint16_t blend565(uint16_t dst, uint16_t src, int32_t alpha)
{
  int32_t ia = 256 - alpha;
  int32_t r = (((src >> 11) & 0x1f) * alpha + ((dst >> 11) & 0x1f) * ia) >> 8;
  int32_t g = (((src >> 5) & 0x3f) * alpha + ((dst >> 5) & 0x3f) * ia) >> 8;
  int32_t b = ((src & 0x1f) * alpha + (dst & 0x1f) * ia) >> 8;

  return (uint16_t)((r << 11) | (g << 5) | b);
}

/****************************************************************************
 * Name: shade565
 *
 * Description:
 *   Scale a colour's channels by num/den.  Used for the iris gradient so the
 *   table only has to name one colour.
 *
 ****************************************************************************/

static uint16_t shade565(uint16_t c, int32_t num, int32_t den)
{
  int32_t r = (((c >> 11) & 0x1f) * num) / den;
  int32_t g = (((c >> 5) & 0x3f) * num) / den;
  int32_t b = ((c & 0x1f) * num) / den;

  if (r > 0x1f)
    {
      r = 0x1f;
    }

  if (g > 0x3f)
    {
      g = 0x3f;
    }

  if (b > 0x1f)
    {
      b = 0x1f;
    }

  return (uint16_t)((r << 11) | (g << 5) | b);
}

static void px_blend(struct canvas_s *cv, int x, int y, uint16_t colour,
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
 * Name: isqrt32
 *
 * Description:
 *   Integer square root, bit at a time.  Used to turn an ellipse's row
 *   equation into an x span so the inner loop can be a comparison instead of
 *   a 64-bit multiply -- see ellipse_half_width().
 *
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

/****************************************************************************
 * Name: ellipse_half_width
 *
 * Description:
 *   Half width of the ellipse at vertical offset dy, or -1 when the row does
 *   not intersect it.
 *
 *   This is the whole reason the renderer is fast enough to animate.  The
 *   direct predicate needs dx*dx*ry*ry + dy*dy*rx*rx <= rx*rx*ry*ry, which is
 *   nine 64-bit multiplies; on a Cortex-M33 those are library calls, and at
 *   nine subsamples per pixel over four ellipses per eye the first version of
 *   this file measured 988ms per frame on the board.  Solving for the span
 *   once per subsample row moves all of that arithmetic out of the inner
 *   loop, which then only compares |dx| against the result.
 *
 *   Precision: u is a 12-bit fraction, so the span is accurate to rx/4096 --
 *   about 1/25 of a pixel at these sizes, far below the subsample grid.
 *
 ****************************************************************************/

static int32_t ellipse_half_width(int32_t dy, int32_t rx, int32_t ry)
{
  int32_t v2;
  uint32_t u;

  if (rx <= 0 || ry <= 0)
    {
      return -1;
    }

  if (dy < 0)
    {
      dy = -dy;
    }

  if (dy >= ry)
    {
      return dy == ry ? 0 : -1;
    }

  v2 = (int32_t)((((int64_t)dy * dy) << 16) / ((int64_t)ry * ry));
  u = isqrt32((uint32_t)(65536 - v2) << 8);

  return (int32_t)(((int64_t)rx * u) >> 12);
}

/****************************************************************************
 * Name: in_ellipse
 *
 * Description:
 *   dx^2/rx^2 + dy^2/ry^2 <= 1, cross-multiplied so there is no division.
 *   The products need 64 bits: dx and rx reach 160*16, so dx^2*ry^2 is about
 *   4.3e13.
 *
 ****************************************************************************/

static bool in_ellipse(int32_t dx, int32_t dy, int32_t rx, int32_t ry)
{
  int64_t a;
  int64_t b;

  if (rx <= 0 || ry <= 0)
    {
      return false;
    }

  a = (int64_t)dx * dx * ry * ry;
  b = (int64_t)dy * dy * rx * rx;

  return a + b <= (int64_t)rx * rx * ry * ry;
}

static bool in_poly(const int16_t *pts, int npts, int32_t x, int32_t y)
{
  bool inside = false;
  int i;
  int j;

  for (i = 0, j = npts - 1; i < npts; j = i++)
    {
      int32_t xi = pts[2 * i];
      int32_t yi = pts[2 * i + 1];
      int32_t xj = pts[2 * j];
      int32_t yj = pts[2 * j + 1];
      int64_t lhs;
      int64_t rhs;
      bool    cross;

      if ((yi > y) == (yj > y))
        {
          continue;
        }

      /* x < xi + (xj - xi) * (y - yi) / (yj - yi), multiplied out so the
       * division disappears; the comparison flips when yj < yi.
       */

      lhs = (int64_t)(x - xi) * (yj - yi);
      rhs = (int64_t)(xj - xi) * (y - yi);
      cross = yj > yi ? lhs < rhs : lhs > rhs;

      if (cross)
        {
          inside = !inside;
        }
    }

  return inside;
}

/****************************************************************************
 * Name: prim_inside
 *
 * Description:
 *   The one predicate every shape funnels through.  x and y are subpixel
 *   absolute coordinates.
 *
 ****************************************************************************/

static bool prim_inside(const struct prim_s *p, int32_t x, int32_t y)
{
  int32_t dx = x - p->cx;
  int32_t dy = y - p->cy;

  switch (p->kind)
    {
      case PRIM_ELLIPSE:
        return in_ellipse(dx, dy, p->rx, p->ry);

      case PRIM_RING:
        return in_ellipse(dx, dy, p->rx, p->ry) &&
               !in_ellipse(dx, dy, p->rx - p->p0, p->ry - p->p0);

      case PRIM_ARC:
        {
          int32_t q;
          int32_t yc;

          if (dx < -p->rx || dx > p->rx || p->rx <= 0)
            {
              return false;
            }

          /* Parabola through (+-rx, cy) with its vertex p0 below cy. */

          q = (dx * 256) / p->rx;
          yc = p->p0 - ((p->p0 * q * q) >> 16);

          return dy - yc <= p->p1 / 2 && yc - dy <= p->p1 / 2;
        }

      case PRIM_RRECT:
        {
          int32_t ax = dx < 0 ? -dx : dx;
          int32_t ay = dy < 0 ? -dy : dy;
          int32_t ox = ax - (p->rx - p->p0);
          int32_t oy = ay - (p->ry - p->p0);

          if (ax > p->rx || ay > p->ry)
            {
              return false;
            }

          if (ox <= 0 || oy <= 0)
            {
              return true;
            }

          return (int64_t)ox * ox + (int64_t)oy * oy <=
                 (int64_t)p->p0 * p->p0;
        }

      case PRIM_WAVE:
        {
          int32_t idx;
          int32_t yc;

          if (dx < -p->rx || dx > p->rx || p->rx <= 0)
            {
              return false;
            }

          /* Phase is offset by a quarter turn so the wave is a cosine of dx,
           * which is even and therefore mirror symmetric.  A plain sine of
           * (dx + rx) is odd about the centre and made the wavy mouths the
           * only asymmetric faces in the table.
           */

          idx = 16 + (int32_t)(((int64_t)dx * 64 * p->p2) / (2 * p->rx));
          yc = (p->p0 * g_sin64[idx & 63]) / 256;

          return dy - yc <= p->p1 / 2 && yc - dy <= p->p1 / 2;
        }

      case PRIM_TAPER:
        {
          /* A wedge that narrows to a point p0 above cy.  Written as an
           * analytic predicate on |dx| rather than as a filled triangle: the
           * even-odd polygon test compares strictly against its edges, so a
           * subsample landing exactly on an edge is inside on one side and
           * outside on the mirrored side.  The tear drop's apex sits exactly
           * on the subpixel grid, and that cost two mismatching pixels in
           * test_face's symmetry check.
           */

          int32_t ax = dx < 0 ? -dx : dx;
          int32_t h = p->p0 < 0 ? -p->p0 : p->p0;
          int32_t d = p->p0 < 0 ? dy : -dy;

          if (h == 0 || d < 0 || d > h)
            {
              return false;
            }

          return ax <= (p->p1 * (h - d)) / h;
        }

      case PRIM_POLY:
        return in_poly(p->pts, p->npts, x, y);

      default:
        return false;
    }
}

/****************************************************************************
 * Name: prim_draw
 *
 * Description:
 *   Scan the primitive's bounding box, take 3x3 subsamples per pixel and
 *   blend by coverage.  When c1 differs from c0 the colour is interpolated
 *   vertically across the box, which is how the iris gets its gradient.
 *
 ****************************************************************************/

static void prim_draw(struct canvas_s *cv, const struct prim_s *p,
                      uint16_t c0, uint16_t c1)
{
  int x0 = (int)((p->cx - p->rx) / FP) - 1;
  int x1 = (int)((p->cx + p->rx) / FP) + 1;
  int y0 = (int)((p->cy - p->ry) / FP) - 1;
  int y1 = (int)((p->cy + p->ry) / FP) + 1;
  int x;
  int y;

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
      uint16_t colour = c0;
      int32_t outer[SUBN];
      int32_t inner[SUBN];
      bool span = p->kind == PRIM_ELLIPSE || p->kind == PRIM_RING ||
                  p->kind == PRIM_TAPER;
      bool curve = p->kind == PRIM_ARC || p->kind == PRIM_WAVE;
      int j;

      if (c1 != c0 && y1 > y0)
        {
          colour = blend565(c0, c1, ((y - y0) * 256) / (y1 - y0));
        }

      if (span)
        {
          for (j = 0; j < SUBN; j++)
            {
              int32_t dy = FPX(y) + g_sub_off[j] - p->cy;

              inner[j] = -1;

              if (p->kind == PRIM_TAPER)
                {
                  int32_t h = p->p0 < 0 ? -p->p0 : p->p0;
                  int32_t d = p->p0 < 0 ? dy : -dy;

                  outer[j] = (h == 0 || d < 0 || d > h) ? -1 :
                             (p->p1 * (h - d)) / h;
                }
              else
                {
                  outer[j] = ellipse_half_width(dy, p->rx, p->ry);

                  if (p->kind == PRIM_RING)
                    {
                      inner[j] = ellipse_half_width(dy, p->rx - p->p0,
                                                    p->ry - p->p0);
                    }
                }
            }
        }

      /* Interior fast path: for a plain ellipse the three row spans give the
       * range of pixels that are covered by all nine subsamples, and those
       * need one write instead of nine predicate evaluations.  On this board
       * that is the difference between an animation and a slideshow: the AP
       * runs the renderer at roughly 0.4us per pixel of work, so a 44-pixel
       * wide eye must not pay full subsampling for the 40 pixels that are
       * unambiguously inside it.
       */

      if (span && p->kind == PRIM_ELLIPSE && outer[0] >= 0 &&
          outer[1] >= 0 && outer[2] >= 0)
        {
          int32_t narrow = outer[0];
          int fx0;
          int fx1;

          for (j = 1; j < SUBN; j++)
            {
              if (outer[j] < narrow)
                {
                  narrow = outer[j];
                }
            }

          fx0 = (int)((p->cx - narrow + FP - 1) / FP);
          fx1 = (int)((p->cx + narrow) / FP) - 1;

          if (fx0 < x0)
            {
              fx0 = x0;
            }

          if (fx1 > x1)
            {
              fx1 = x1;
            }

          if (fx1 >= fx0)
            {
              for (x = fx0; x <= fx1; x++)
                {
                  px_blend(cv, x, y, colour, 256);
                }

              /* Leave the two partially covered margins to the general
               * path below.
               */

              for (x = x0; x <= x1; x++)
                {
                  int cov = 0;
                  int i;

                  if (x >= fx0 && x <= fx1)
                    {
                      continue;
                    }

                  for (j = 0; j < SUBN; j++)
                    {
                      for (i = 0; i < SUBN; i++)
                        {
                          int32_t dx = FPX(x) + g_sub_off[i] - p->cx;

                          if (dx < 0)
                            {
                              dx = -dx;
                            }

                          if (dx <= outer[j])
                            {
                              cov++;
                            }
                        }
                    }

                  if (cov > 0)
                    {
                      px_blend(cv, x, y, colour, g_cov_alpha[cov]);
                    }
                }

              continue;
            }
        }

      for (x = x0; x <= x1; x++)
        {
          int32_t yc[SUBN];
          int cov = 0;
          int i;

          /* The arc and the wave depend on x only, so their curve value is
           * evaluated once per subsample column instead of once per
           * subsample point -- three 64-bit divisions per pixel rather than
           * nine.
           */

          if (curve)
            {
              for (i = 0; i < SUBN; i++)
                {
                  int32_t dx = FPX(x) + g_sub_off[i] - p->cx;

                  if (dx < -p->rx || dx > p->rx || p->rx <= 0)
                    {
                      yc[i] = INT32_MIN;
                    }
                  else if (p->kind == PRIM_ARC)
                    {
                      /* yc = p0 * (1 - (dx/rx)^2), normalised to 8.8 so the
                       * whole expression stays in 32 bits: q^2 <= 65536 and
                       * p0 <= FPX(40), so p0*q^2 stays under 2^26.  The
                       * cross-multiplied 64-bit form cost a library division
                       * per subsample column.
                       */

                      int32_t q = (dx * 256) / p->rx;

                      yc[i] = p->p0 - ((p->p0 * q * q) >> 16);
                    }
                  else
                    {
                      int32_t si = 16 + (int32_t)(((int64_t)dx * 64 *
                                   p->p2) / (2 * p->rx));

                      yc[i] = (p->p0 * g_sin64[si & 63]) / 256;
                    }
                }
            }

          for (j = 0; j < SUBN; j++)
            {
              for (i = 0; i < SUBN; i++)
                {
                  int32_t sx = FPX(x) + g_sub_off[i];

                  if (curve)
                    {
                      int32_t dy = FPX(y) + g_sub_off[j] - p->cy;

                      if (yc[i] != INT32_MIN &&
                          dy - yc[i] <= p->p1 / 2 &&
                          yc[i] - dy <= p->p1 / 2)
                        {
                          cov++;
                        }
                    }
                  else if (span)
                    {
                      int32_t dx = sx - p->cx;

                      if (dx < 0)
                        {
                          dx = -dx;
                        }

                      if (outer[j] >= 0 && dx <= outer[j] &&
                          (inner[j] < 0 || dx > inner[j]))
                        {
                          cov++;
                        }
                    }
                  else if (prim_inside(p, sx, FPX(y) + g_sub_off[j]))
                    {
                      cov++;
                    }
                }
            }

          if (cov > 0)
            {
              px_blend(cv, x, y, colour, g_cov_alpha[cov]);
            }
        }
    }
}

/****************************************************************************
 * Name: draw_seg
 *
 * Description:
 *   Thick line segment with round caps: every point within thick/2 of the
 *   segment.  Distance is compared squared, so there is no square root.
 *
 ****************************************************************************/

static void draw_seg(struct canvas_s *cv, int32_t ax, int32_t ay,
                     int32_t bx, int32_t by, int32_t thick, uint16_t colour)
{
  int32_t r = thick / 2;
  int32_t dx = bx - ax;
  int32_t dy = by - ay;
  int32_t len2 = dx * dx + dy * dy;

  /* |cross| <= r * len is the same test as cross^2 <= r^2 * len2 but fits in
   * 32 bits: cross reaches 2^24 and its square would not.  The length is
   * needed once per segment, not once per subsample.
   */

  int32_t rlen = (int32_t)(((int64_t)r * isqrt32((uint32_t)len2)));
  int32_t lo;
  int32_t hi;
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

  lo = ax < bx ? ax : bx;
  hi = ax > bx ? ax : bx;
  x0 = (int)((lo - r) / FP) - 1;
  x1 = (int)((hi + r) / FP) + 1;
  lo = ay < by ? ay : by;
  hi = ay > by ? ay : by;
  y0 = (int)((lo - r) / FP) - 1;
  y1 = (int)((hi + r) / FP) + 1;

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
                  int32_t sx = FPX(x) + g_sub_off[i];
                  int32_t sy = FPX(y) + g_sub_off[j];
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
              px_blend(cv, x, y, colour, g_cov_alpha[cov]);
            }
        }
    }
}

/****************************************************************************
 * Name: draw_blush
 *
 * Description:
 *   Soft radial patch: alpha falls off with the square of the radius, so the
 *   edge has no visible boundary.  A hard-edged circle here looks like a
 *   sticker rather than a cheek.
 *
 ****************************************************************************/

static void draw_blush(struct canvas_s *cv, int32_t cx, int32_t cy,
                       int32_t r, int32_t strength, uint16_t colour)
{
  int x0 = (int)((cx - r) / FP);
  int x1 = (int)((cx + r) / FP);
  int y0 = (int)((cy - r) / FP);
  int y1 = (int)((cy + r) / FP);
  int x;
  int y;

  if (strength <= 0 || r <= 0)
    {
      return;
    }

  for (y = y0; y <= y1; y++)
    {
      for (x = x0; x <= x1; x++)
        {
          int32_t dx = FPX(x) + FP / 2 - cx;
          int32_t dy = FPX(y) + FP / 2 - cy;
          int32_t d2 = dx * dx + dy * dy;
          int32_t r2 = r * r;
          int32_t alpha;

          /* 32-bit throughout: r is at most FPX(20) so r2 stays under 2^19
           * and (r2 - d2) * strength under 2^27.  The 64-bit version of this
           * expression cost a library division per pixel of the patch.
           */

          if (d2 >= r2)
            {
              continue;
            }

          alpha = ((r2 - d2) * strength) / r2;
          if (alpha > 256)
            {
              alpha = 256;
            }

          px_blend(cv, x, y, colour, alpha);
        }
    }
}

/****************************************************************************
 * Name: draw_tear
 *
 * Description:
 *   A drop is a circle with a tapered top, plus a specular dot.  Built from
 *   two primitives rather than its own implicit curve: reusing the ellipse
 *   and polygon predicates keeps the shape list short.
 *
 ****************************************************************************/

static void draw_tear(struct canvas_s *cv, int32_t cx, int32_t cy,
                      int32_t r, uint16_t colour, bool mirror)
{
  struct prim_s p;

  memset(&p, 0, sizeof(p));

  /* Tapered top. */

  p.kind = PRIM_TAPER;
  p.cx   = cx;
  p.cy   = cy;
  p.rx   = r + FP;
  p.ry   = r * 2 + FP;
  p.p0   = r * 2;
  p.p1   = (r * 3) / 4;
  prim_draw(cv, &p, colour, colour);

  memset(&p, 0, sizeof(p));
  p.kind = PRIM_ELLIPSE;
  p.cx   = cx;
  p.cy   = cy;
  p.rx   = r;
  p.ry   = r;
  prim_draw(cv, &p, colour, colour);

  /* Specular dot, offset like the eye highlight and mirrored with it. */

  memset(&p, 0, sizeof(p));
  p.kind = PRIM_ELLIPSE;
  p.cx   = cx + (mirror ? r / 3 : -r / 3);
  p.cy   = cy - r / 3;
  p.rx   = r / 3;
  p.ry   = r / 3;
  prim_draw(cv, &p, C_WHITE, C_WHITE);
}

static void draw_spiral(struct canvas_s *cv, int32_t cx, int32_t cy,
                        int32_t r, int32_t phase, int32_t thick,
                        uint16_t colour)
{
  const int steps = 40;
  const int turns = 3;
  int32_t px = cx;
  int32_t py = cy;
  int i;

  for (i = 1; i <= steps; i++)
    {
      int32_t ang = phase + (i * turns * 256) / steps;
      int     si  = (ang & 255) >> 2;
      int32_t rr  = (r * i) / steps;
      int32_t nx  = cx + (rr * g_sin64[(si + 16) & 63]) / 256;
      int32_t ny  = cy + (rr * g_sin64[si & 63]) / 256;

      draw_seg(cv, px, py, nx, ny, thick, colour);
      px = nx;
      py = ny;
    }
}

static void draw_star(struct canvas_s *cv, int32_t cx, int32_t cy,
                      int32_t r, int32_t phase, uint16_t colour)
{
  struct prim_s p;
  int16_t pts[20];
  int i;

  for (i = 0; i < 10; i++)
    {
      int32_t ang = phase + (i * 256 + 5) / 10 - 64;
      int     si  = (ang & 255) >> 2;
      int32_t rr  = (i & 1) ? (r * 45) / 100 : r;

      pts[2 * i]     = (int16_t)(cx + (rr * g_sin64[(si + 16) & 63]) / 256);
      pts[2 * i + 1] = (int16_t)(cy + (rr * g_sin64[si & 63]) / 256);
    }

  memset(&p, 0, sizeof(p));
  p.kind = PRIM_POLY;
  p.cx   = cx;
  p.cy   = cy;
  p.rx   = r + FP;
  p.ry   = r + FP;
  p.pts  = pts;
  p.npts = 10;
  prim_draw(cv, &p, colour, colour);
}

/****************************************************************************
 * Name: draw_eye
 *
 * Description:
 *   One eye.  cx is the eye centre in subpixels; mirror flips the shapes
 *   that are not left/right symmetric (the angry wedge and the specular
 *   highlight).
 *
 ****************************************************************************/

static void draw_eye(struct canvas_s *cv, const struct face_s *f,
                     int32_t cx, int32_t cy, int32_t lid, bool mirror)
{
  int32_t rx = FPX(f->eye_rx);
  int32_t ry = FPX(f->eye_ry);
  struct prim_s p;

  memset(&p, 0, sizeof(p));

  switch (f->eye_style)
    {
      case EYE_ROUND:
        {
          int32_t irisr = FPX(f->iris_r);
          int32_t pupr  = FPX(f->pupil_r);
          int32_t spec  = irisr / 3;

          p.kind = PRIM_ELLIPSE;
          p.cx = cx;
          p.cy = cy;
          p.rx = rx;
          p.ry = ry;
          prim_draw(cv, &p, f->sclera, f->sclera);

          if (irisr > 0)
            {
              p.rx = irisr;
              p.ry = irisr;
              prim_draw(cv, &p, f->iris, shade565(f->iris, 55, 100));
            }

          if (pupr > 0)
            {
              p.rx = pupr;
              p.ry = pupr;
              prim_draw(cv, &p, C_BLACK, C_BLACK);
            }

          if (spec > 0)
            {
              p.cx = cx + (mirror ? -irisr / 2 : irisr / 2);
              p.cy = cy - irisr / 2;
              p.rx = spec;
              p.ry = spec;
              prim_draw(cv, &p, C_WHITE, C_WHITE);
            }

          /* The lid is painted in the background colour, so its lower edge
           * is anti-aliased like any other shape -- a half-closed eye must
           * not show a stair-stepped horizontal cut.
           */

          if (lid > 0)
            {
              int32_t l = FPX(lid);

              if (l > 2 * ry)
                {
                  l = 2 * ry;
                }

              memset(&p, 0, sizeof(p));
              p.kind = PRIM_RRECT;
              p.rx   = rx + FP;
              p.ry   = l / 2;
              p.cx   = cx;
              p.cy   = cy - ry + p.ry;
              p.p0   = 0;
              prim_draw(cv, &p, f->bg, f->bg);

              /* Fully closed: leave a lash line behind, otherwise the eye
               * simply vanishes and the blink reads as a glitch.
               */

              if (l >= 2 * ry)
                {
                  draw_seg(cv, cx - rx, cy, cx + rx, cy,
                           FPX(4), f->fg);
                }
            }
        }
        break;

      case EYE_CURVE:
        p.kind = PRIM_ARC;
        p.cx   = cx;
        p.cy   = cy + FPX(f->eye_curve) / 2;
        p.rx   = rx;
        p.ry   = FPX(f->eye_curve) + FPX(8);
        p.p0   = -FPX(f->eye_curve);
        p.p1   = FPX(5);
        prim_draw(cv, &p, f->fg, f->fg);
        break;

      case EYE_LINE:
        p.kind = PRIM_RRECT;
        p.cx   = cx;
        p.cy   = cy;
        p.rx   = rx;
        p.ry   = FPX(3);
        p.p0   = FPX(3);
        prim_draw(cv, &p, f->fg, f->fg);
        break;

      case EYE_ANGRY:
        {
          int32_t inner = mirror ? cx - rx : cx + rx;
          int32_t outer = mirror ? cx + rx : cx - rx;

          draw_seg(cv, outer, cy - ry, inner, cy, FPX(5), f->fg);
          draw_seg(cv, outer, cy + ry, inner, cy, FPX(5), f->fg);
        }
        break;

      case EYE_X:
        draw_seg(cv, cx - rx, cy - ry, cx + rx, cy + ry, FPX(5), f->fg);
        draw_seg(cv, cx + rx, cy - ry, cx - rx, cy + ry, FPX(5), f->fg);
        break;

      case EYE_HEART:
        {
          /* Two lobes and a downward wedge rather than the heart curve.
           *
           * The implicit curve (x^2+y^2-1)^3 - x^2 y^3 <= 0 renders a nicer
           * heart, but it needs three 64-bit multiplies per subsample and
           * measured 1204ms per frame for the two eyes of 'love' on this
           * board.  Circles and a wedge land on the fast row-span path and
           * are indistinguishable at 40 pixels across.
           */

          int32_t lobe = (ry * 58) / 100;
          int32_t lobe_dx = (rx * 38) / 100;
          int32_t lobe_y = cy - (ry * 38) / 100;

          /* Flat colour, not a gradient: the wedge's bounding box is twice
           * as tall as the lobes', so a shared vertical gradient made the
           * bottom half noticeably darker and the shape read as an ice
           * cream cone rather than a heart.
           */

          p.kind = PRIM_TAPER;
          p.cx   = cx;
          p.cy   = lobe_y;
          p.p1   = lobe_dx + lobe - lobe / 8;
          p.p0   = -((ry * 90) / 100 + (ry * 38) / 100);
          p.rx   = p.p1 + FP;
          p.ry   = -p.p0 + FP;
          prim_draw(cv, &p, f->iris, f->iris);

          memset(&p, 0, sizeof(p));
          p.kind = PRIM_ELLIPSE;
          p.cy   = lobe_y;
          p.rx   = lobe;
          p.ry   = lobe;
          p.cx   = cx - lobe_dx;
          prim_draw(cv, &p, f->iris, f->iris);
          p.cx   = cx + lobe_dx;
          prim_draw(cv, &p, f->iris, f->iris);
        }
        break;

      case EYE_STAR:
        draw_star(cv, cx, cy, rx, f->spin, f->iris);
        break;

      case EYE_SPIRAL:
        draw_spiral(cv, cx, cy, rx, f->spin, FPX(4), f->fg);
        break;

      default:
        break;
    }
}

static void draw_mouth(struct canvas_s *cv, const struct face_s *f)
{
  int32_t cx = FPX(cv->w / 2);
  int32_t cy = FPX(f->mouth_y);
  int32_t hw = FPX(f->mouth_w) / 2;
  int32_t th = FPX(f->mouth_thick);
  struct prim_s p;

  memset(&p, 0, sizeof(p));

  switch (f->mouth_style)
    {
      case MOUTH_ARC:
        p.kind = PRIM_ARC;
        p.cx   = cx;
        p.cy   = cy;
        p.rx   = hw;
        p.ry   = FPX(f->mouth_curve < 0 ? -f->mouth_curve :
                     f->mouth_curve) + th;
        p.p0   = FPX(f->mouth_curve);
        p.p1   = th;
        prim_draw(cv, &p, f->fg, f->fg);
        break;

      case MOUTH_LINE:
        p.kind = PRIM_RRECT;
        p.cx   = cx;
        p.cy   = cy;
        p.rx   = hw;
        p.ry   = th / 2;
        p.p0   = th / 2;
        prim_draw(cv, &p, f->fg, f->fg);
        break;

      case MOUTH_O:
        p.kind = PRIM_RING;
        p.cx   = cx;
        p.cy   = cy;
        p.rx   = hw;
        p.ry   = FPX(f->mouth_open) / 2;
        p.p0   = th;
        prim_draw(cv, &p, f->fg, f->fg);
        break;

      case MOUTH_OPEN:
        {
          int32_t oh = FPX(f->mouth_open) / 2;

          p.kind = PRIM_ELLIPSE;
          p.cx   = cx;
          p.cy   = cy;
          p.rx   = hw;
          p.ry   = oh;
          prim_draw(cv, &p, f->mouth_fill, shade565(f->mouth_fill, 60, 100));

          p.kind = PRIM_RING;
          p.p0   = th / 2;
          prim_draw(cv, &p, f->fg, f->fg);

          /* Teeth: a band across the top of the cavity.  Without it a wide
           * open mouth reads as a hole rather than a laugh.
           */

          if (oh > FPX(6))
            {
              p.kind = PRIM_RRECT;
              p.cx   = cx;
              p.cy   = cy - oh + th / 2 + FPX(3);
              p.rx   = (hw * 7) / 10;
              p.ry   = FPX(3);
              p.p0   = FPX(2);
              prim_draw(cv, &p, C_WHITE, C_WHITE);
            }

          if (f->tongue > 0)
            {
              p.kind = PRIM_ELLIPSE;
              p.cx   = cx;
              p.cy   = cy + oh - FPX(f->tongue) / 2;
              p.rx   = hw / 2;
              p.ry   = FPX(f->tongue) / 2;
              p.p0   = 0;
              prim_draw(cv, &p, C_PINK, shade565(C_PINK, 70, 100));
            }
        }
        break;

      case MOUTH_TEETH:
        {
          int32_t oh = FPX(f->mouth_open) / 2;
          int i;

          p.kind = PRIM_RRECT;
          p.cx   = cx;
          p.cy   = cy;
          p.rx   = hw;
          p.ry   = oh;
          p.p0   = FPX(3);
          prim_draw(cv, &p, f->fg, f->fg);

          p.rx = hw - FPX(2);
          p.ry = oh - FPX(2);
          p.p0 = FPX(2);
          prim_draw(cv, &p, C_WHITE, C_WHITE);

          for (i = -1; i <= 1; i++)
            {
              int32_t x = cx + (i * hw) / 2;

              draw_seg(cv, x, cy - oh, x, cy + oh, FPX(2), f->fg);
            }

          draw_seg(cv, cx - hw, cy, cx + hw, cy, FPX(2), f->fg);
        }
        break;

      case MOUTH_WAVE:
        p.kind = PRIM_WAVE;
        p.cx   = cx;
        p.cy   = cy;
        p.rx   = hw;
        p.ry   = FPX(f->mouth_open < 0 ? -f->mouth_open : f->mouth_open) +
                 th;
        p.p0   = FPX(f->mouth_open);
        p.p1   = th;
        p.p2   = 2;
        prim_draw(cv, &p, f->fg, f->fg);
        break;

      default:
        break;
    }
}

static void face_render_one(struct canvas_s *cv, const struct face_s *f)
{
  int32_t cxc = FPX(cv->w / 2);
  int32_t eyl = cxc - FPX(f->eye_dx);
  int32_t eyr = cxc + FPX(f->eye_dx);
  int32_t eyy = FPX(f->eye_y);
  int     y;

  /* Background.  The glass is round, so the corners of this rectangle are
   * behind the bezel and cost nothing to fill.
   */

  /* Word-at-a-time.  memset() and halfword stores were both measured slower
   * than this on the board: the C library's memset is byte-wise here, and the
   * same change on the panel-to-panel copy in camera_preview_main.c took it
   * from 65ms to 15ms for the same 51200 bytes.
   */

  {
    uint32_t pair = ((uint32_t)f->bg << 16) | f->bg;

    for (y = 0; y < cv->h; y++)
      {
        uint32_t *row = (uint32_t *)(void *)(cv->buf + (size_t)y * cv->stride);
        int n = cv->w / 2;
        int x;

        for (x = 0; x < n; x++)
          {
            row[x] = pair;
          }

        if (cv->w & 1)
          {
            ((uint16_t *)(void *)row)[cv->w - 1] = f->bg;
          }
      }
  }

  if (f->blush > 0)
    {
      draw_blush(cv, eyl - FPX(10), FPX(f->mouth_y) - FPX(14), FPX(15),
                 f->blush, C_PINK);
      draw_blush(cv, eyr + FPX(10), FPX(f->mouth_y) - FPX(14), FPX(15),
                 f->blush, C_PINK);
    }

  draw_eye(cv, f, eyl, eyy, f->lid_l, false);
  draw_eye(cv, f, eyr, eyy, f->lid_r, true);

  if (f->brow_len > 0)
    {
      int32_t hl = FPX(f->brow_len) / 2;
      int32_t ti = FPX(f->brow_tilt);
      int32_t by = FPX(f->brow_y);
      int32_t bt = FPX(f->brow_thick);

      draw_seg(cv, eyl - hl, by - ti, eyl + hl, by + ti, bt, f->fg);
      draw_seg(cv, eyr + hl, by - ti, eyr - hl, by + ti, bt, f->fg);
    }

  draw_mouth(cv, f);

  if (f->tear > 0)
    {
      /* Fall from just below the eye to the lower cheek.  The end point is
       * kept at y=128 so the drop stays inside the round glass: at y=150 the
       * visible half-width is only 38px and the eye columns are outside it.
       */

      int32_t ty = eyy + FPX(f->eye_ry) + ((FPX(128) - eyy -
                   FPX(f->eye_ry)) * f->tear) / 1000;

      draw_tear(cv, eyl, ty, FPX(5), C_TEAR, false);
      draw_tear(cv, eyr, ty, FPX(5), C_TEAR, true);
    }
}

/****************************************************************************
 * Name: face_lerp
 *
 * Description:
 *   Interpolate the numeric block between two keyframes.  Styles and colours
 *   are taken from a: an enum has no meaningful midpoint, and interpolating
 *   packed RGB565 would carry between channels.
 *
 *   The block is walked as an int16_t array so that a field added to
 *   struct face_s between eye_dx and fg is animated without touching this
 *   function.  test_face.c checks that every field in the range moves.
 *
 ****************************************************************************/

static void face_lerp(struct face_s *out, const struct face_s *a,
                      const struct face_s *b, int32_t num, int32_t den)
{
  const int16_t *pa = (const int16_t *)(const void *)
                      ((const uint8_t *)a + FACE_NUM_OFF);
  const int16_t *pb = (const int16_t *)(const void *)
                      ((const uint8_t *)b + FACE_NUM_OFF);
  int16_t *po;
  size_t i;

  *out = *a;
  po = (int16_t *)(void *)((uint8_t *)out + FACE_NUM_OFF);

  if (den <= 0)
    {
      return;
    }

  for (i = 0; i < FACE_NUM_COUNT; i++)
    {
      po[i] = (int16_t)(pa[i] + ((int32_t)(pb[i] - pa[i]) * num) / den);
    }
}

static uint32_t expr_duration(const struct expression_s *e)
{
  uint32_t total = 0;
  int i;

  if (e->nkeys < 2)
    {
      return 0;
    }

  for (i = 0; i < e->nkeys; i++)
    {
      total += e->keys[i].hold_ms;
    }

  return total;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int preview_face_count(void)
{
  return EXPR_COUNT;
}

const char *preview_face_name(int idx)
{
  if (idx < 0 || idx >= EXPR_COUNT)
    {
      return NULL;
    }

  return g_expr[idx].name;
}

int preview_face_lookup(const char *name)
{
  int i;

  if (name == NULL)
    {
      return -1;
    }

  for (i = 0; i < EXPR_COUNT; i++)
    {
      if (strcmp(g_expr[i].name, name) == 0)
        {
          return i;
        }
    }

  return -1;
}

uint32_t preview_face_duration_ms(int idx)
{
  if (idx < 0 || idx >= EXPR_COUNT)
    {
      return 0;
    }

  return expr_duration(&g_expr[idx]);
}

void preview_face_render(uint8_t *buf, size_t stride, int w, int h,
                         int idx, uint32_t phase_ms)
{
  const struct expression_s *e;
  struct canvas_s cv;
  struct face_s f;
  uint32_t total;
  uint32_t t;
  int i;

  if (buf == NULL || w <= 0 || h <= 0 || idx < 0 || idx >= EXPR_COUNT)
    {
      return;
    }

  e = &g_expr[idx];
  cv.buf    = buf;
  cv.stride = stride;
  cv.w      = w;
  cv.h      = h;

  total = expr_duration(e);
  if (total == 0)
    {
      f = e->keys[0].f;
    }
  else
    {
      t = phase_ms % total;

      for (i = 0; i < e->nkeys; i++)
        {
          uint32_t hold = e->keys[i].hold_ms;

          if (t < hold || i == e->nkeys - 1)
            {
              const struct face_s *nxt =
                &e->keys[(i + 1) % e->nkeys].f;

              face_lerp(&f, &e->keys[i].f, nxt, (int32_t)t,
                        (int32_t)(hold == 0 ? 1 : hold));
              break;
            }

          t -= hold;
        }
    }

  if ((e->flags & FACE_SPIN) != 0)
    {
      /* A spiral or star has no meaningful midpoint to interpolate towards
       * across the loop's wrap-around, so its rotation is driven straight
       * from the phase instead of from keyframes.
       */

      f.spin = (int16_t)((f.spin + (int32_t)(phase_ms / 4)) & 255);
    }

  face_render_one(&cv, &f);
}
