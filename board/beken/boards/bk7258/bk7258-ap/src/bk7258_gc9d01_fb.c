/****************************************************************************
 * board/beken/boards/bk7258/bk7258-ap/src/bk7258_gc9d01_fb.c
 *
 * openvela framebuffer driver for the GC9D01 panels (160x160 round, RGB565,
 * one per QSPI controller).  Implements the three hooks fb_register() needs
 * -- up_fbinitialize(), up_fbgetvplane(), up_fbuninitialize() -- plus the
 * fb_vtable_s entries required for a panel that has no autonomous refresh:
 * getvideoinfo, getplaneinfo and updatearea.
 *
 * Why updatearea is mandatory here: a RAM-less RGB parallel panel is
 * scanned out continuously by an LCD controller, so writing the
 * framebuffer is enough.  GC9D01 has its own GRAM and is written over a
 * serial bus, so nothing reaches the glass until the driver explicitly
 * pushes it.  Without CONFIG_FB_UPDATE + this callback the screen would
 * simply never change.
 *
 * ---------------------------------------------------------------------
 * Multiple displays
 * ---------------------------------------------------------------------
 * fb_register(display, plane) (include/nuttx/video/fb.h) calls
 * up_fbinitialize(display), then up_fbgetvplane(display, plane), then
 * fb_register_device(), which names the node /dev/fb<display>.  So exposing
 * two panels is just a matter of calling fb_register() twice.
 *
 * The constraint that shapes this file: fb_vtable_s's callbacks receive
 * only the vtable pointer, with no display argument.  A single shared
 * vtable therefore cannot tell which panel it is being asked about.  Each
 * display consequently gets its own state structure whose *first* member is
 * the vtable, so a callback can recover its own state by casting the vtable
 * pointer back (the usual container-of trick, valid here because the vtable
 * sits at offset 0).
 *
 * Framebuffers come from the kernel heap: 2 x 51200 = 100KB out of roughly
 * 300KB, which is affordable and much faster for the CPU to write than
 * PSRAM (which is mapped non-cacheable).  The camera's own frame buffers do
 * live in PSRAM -- they are 614400 bytes each and have no alternative.
 *
 * The panels are round: the corners of the 160x160 square are not visible.
 * That is a composition concern for whatever draws into the framebuffer,
 * not something this driver compensates for.
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#include <nuttx/config.h>
#include <nuttx/arch.h>
#include <nuttx/kmalloc.h>
#include <nuttx/clock.h>
#include <nuttx/video/fb.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

#include "bk7258_qspi.h"
#include "bk7258_gc9d01_fb.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Boot greeting.  Cyan on black to match the expression palette that
 * camera_preview draws (see app/camera_preview/preview_face.c), so the boot
 * screen and the faces look like they belong to the same device.
 */

#define GC9D01_HELLO_COLS   5
#define GC9D01_HELLO_ROWS   7
#define GC9D01_HELLO_SCALE  3
#define GC9D01_HELLO_FG     0x07ff
#define GC9D01_HELLO_BG     0x0000

/****************************************************************************
 * Private Types
 ****************************************************************************/

/* vtable MUST stay first: the callbacks recover this structure by casting
 * their vtable argument, since fb_vtable_s carries no display index.
 */

struct gc9d01_fb_s
{
  struct fb_vtable_s vtable;
  int display;
  int bus;
  FAR uint8_t *fbmem;
  uint32_t updates;
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static int gc9d01_getvideoinfo(FAR struct fb_vtable_s *vtable,
                               FAR struct fb_videoinfo_s *vinfo);
static int gc9d01_getplaneinfo(FAR struct fb_vtable_s *vtable, int planeno,
                               FAR struct fb_planeinfo_s *pinfo);
#ifdef CONFIG_FB_UPDATE
static int gc9d01_updatearea(FAR struct fb_vtable_s *vtable,
                             FAR const struct fb_area_s *area);
#endif

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct gc9d01_fb_s g_gc9d01_fb[GC9D01_NDISPLAYS] =
{
  {
    .vtable =
      {
        .getvideoinfo = gc9d01_getvideoinfo,
        .getplaneinfo = gc9d01_getplaneinfo,
#ifdef CONFIG_FB_UPDATE
        .updatearea   = gc9d01_updatearea,
#endif
      },
    .display = 0,
    .bus     = -1,
  },
  {
    .vtable =
      {
        .getvideoinfo = gc9d01_getvideoinfo,
        .getplaneinfo = gc9d01_getplaneinfo,
#ifdef CONFIG_FB_UPDATE
        .updatearea   = gc9d01_updatearea,
#endif
      },
    .display = 1,
    .bus     = -1,
  },
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static inline FAR struct gc9d01_fb_s *
gc9d01_from_vtable(FAR struct fb_vtable_s *vtable)
{
  /* Safe because .vtable is the first member of struct gc9d01_fb_s. */

  return (FAR struct gc9d01_fb_s *)vtable;
}

static int gc9d01_getvideoinfo(FAR struct fb_vtable_s *vtable,
                               FAR struct fb_videoinfo_s *vinfo)
{
  if (vtable == NULL || vinfo == NULL)
    {
      return -EINVAL;
    }

  vinfo->fmt     = FB_FMT_RGB16_565;
  vinfo->xres    = GC9D01_XRES;
  vinfo->yres    = GC9D01_YRES;
  vinfo->nplanes = 1;

  return OK;
}

static int gc9d01_getplaneinfo(FAR struct fb_vtable_s *vtable, int planeno,
                               FAR struct fb_planeinfo_s *pinfo)
{
  FAR struct gc9d01_fb_s *priv;

  if (vtable == NULL || pinfo == NULL || planeno != 0)
    {
      return -EINVAL;
    }

  priv = gc9d01_from_vtable(vtable);

  /* Single buffer: fbcount is 1, so yres_virtual == yres and fblen is one
   * frame.  If a second buffer is added later, fblen/yres_virtual must grow
   * with it.
   */

  pinfo->fbmem        = priv->fbmem;
  pinfo->fblen        = GC9D01_FBLEN;
  pinfo->stride       = GC9D01_STRIDE;
  pinfo->display      = priv->display;
  pinfo->bpp          = GC9D01_BPP;
  pinfo->xres_virtual = GC9D01_XRES;
  pinfo->yres_virtual = GC9D01_YRES;
  pinfo->xoffset      = 0;
  pinfo->yoffset      = 0;

  return OK;
}

/****************************************************************************
 * Name: gc9d01_push
 *
 * Description:
 *   Re-assert the full-panel window and stream the framebuffer out.
 *
 ****************************************************************************/

static int gc9d01_push(FAR struct gc9d01_fb_s *priv)
{
  clock_t start = clock_systime_ticks();

  if (priv->fbmem == NULL || priv->bus < 0)
    {
      return -ENODEV;
    }

  if (!bk7258_gc9d01_window_full(priv->display))
    {
      return -EIO;
    }

  if (!bk7258_lcd_spi_write_frame(priv->bus, priv->fbmem, GC9D01_FBLEN))
    {
      printf("gc9d01_fb[%d]: pixel burst failed\n", priv->display);
      return -EIO;
    }

  priv->updates++;

  /* First few pushes get a timing report: it is the only way to know the
   * real cost of a frame on this bus, which decides whether the preview
   * path needs DMA.  After that stay quiet -- this runs per displayed
   * frame.
   */

  if (priv->updates <= 3)
    {
      uint32_t ms = TICK2MSEC(clock_systime_ticks() - start);

      printf("gc9d01_fb[%d]: update %u: %d bytes in %ums "
             "(%u frames on bus)\n", priv->display,
             (unsigned int)priv->updates, GC9D01_FBLEN, (unsigned int)ms,
             (unsigned int)bk7258_lcd_spi_frame_count(priv->bus));
    }

  return OK;
}

#ifdef CONFIG_FB_UPDATE
static int gc9d01_updatearea(FAR struct fb_vtable_s *vtable,
                             FAR const struct fb_area_s *area)
{
  /* Partial updates would need a per-row copy out of the framebuffer,
   * because the panel expects a densely packed rectangle while the
   * framebuffer rows are GC9D01_STRIDE apart.  At 51200 bytes a full frame
   * is cheap enough that this always pushes everything and ignores the
   * requested rectangle.
   */

  UNUSED(area);

  if (vtable == NULL)
    {
      return -EINVAL;
    }

  return gc9d01_push(gc9d01_from_vtable(vtable));
}
#endif

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int up_fbinitialize(int display)
{
  FAR struct gc9d01_fb_s *priv;
  int ret;

  if (display < 0 || display >= GC9D01_NDISPLAYS)
    {
      return -EINVAL;
    }

  priv = &g_gc9d01_fb[display];

  if (priv->fbmem == NULL)
    {
      priv->fbmem = kmm_zalloc(GC9D01_FBLEN);
      if (priv->fbmem == NULL)
        {
          printf("gc9d01_fb[%d]: cannot allocate %d-byte framebuffer\n",
                 display, GC9D01_FBLEN);
          return -ENOMEM;
        }
    }

  /* Rail, panel reset, bus bring-up and the init command sequence.  Kept in
   * bk7258_gc9d01.c so the command table and the pin map have a single
   * owner.
   */

  ret = bk7258_gc9d01_panel_init(display);
  if (ret < 0)
    {
      printf("gc9d01_fb[%d]: panel init failed: %d\n", display, ret);
      kmm_free(priv->fbmem);
      priv->fbmem = NULL;
      return ret;
    }

  priv->bus = bk7258_gc9d01_bus(display);

  printf("gc9d01_fb[%d]: %dx%d RGB565, framebuffer %d bytes at %p, bus %d\n",
         display, GC9D01_XRES, GC9D01_YRES, GC9D01_FBLEN, priv->fbmem,
         priv->bus);

  return OK;
}

FAR struct fb_vtable_s *up_fbgetvplane(int display, int vplane)
{
  if (display < 0 || display >= GC9D01_NDISPLAYS || vplane != 0)
    {
      return NULL;
    }

  return &g_gc9d01_fb[display].vtable;
}

void up_fbuninitialize(int display)
{
  UNUSED(display);
}

/****************************************************************************
 * Name: bk7258_gc9d01_fb_fill
 *
 * Description:
 *   Bring-up helper: fills one display's framebuffer with a solid RGB565
 *   colour and pushes it.  This is the first thing that can prove the whole
 *   chain (bus framing, init sequence, CASET/RASET, pixel burst) actually
 *   reaches the glass -- the panel has no readable ID register, so until
 *   something is visible there is no evidence the init sequence worked.
 *
 ****************************************************************************/

int bk7258_gc9d01_fb_fill(int display, uint16_t rgb565)
{
  FAR struct gc9d01_fb_s *priv;
  FAR uint16_t *px;
  int i;

  if (display < 0 || display >= GC9D01_NDISPLAYS)
    {
      return -EINVAL;
    }

  priv = &g_gc9d01_fb[display];
  px = (FAR uint16_t *)priv->fbmem;

  if (px == NULL)
    {
      return -ENODEV;
    }

  for (i = 0; i < GC9D01_XRES * GC9D01_YRES; i++)
    {
      px[i] = rgb565;
    }

  printf("gc9d01_fb[%d]: fill 0x%04x pushed\n", display, rgb565);

  return gc9d01_push(priv);
}

/****************************************************************************
 * Name: bk7258_gc9d01_fb_hello
 *
 * Description:
 *   Boot greeting: the word "hello" centred on the panel.
 *
 *   This replaced the four-quadrant test pattern as the boot screen.  The
 *   pattern is still here and still worth having -- it is the thing that
 *   makes a byte-order or stride mistake obvious by eye -- but it is a
 *   diagnostic, not something to greet a user with.  It is reachable from
 *   the shell as 'camera_preview pattern', which draws the same quadrants
 *   and border through the framebuffer.
 *
 *   The glyphs are a 5x7 bitmap scaled by 3, so the word is 87x21 pixels
 *   and fits inside the round glass with room to spare: the top-left corner
 *   lands 45 pixels from the centre against a visible radius of 80.
 *
 ****************************************************************************/

int bk7258_gc9d01_fb_hello(int display)
{
  /* 'h', 'e', 'l', 'l', 'o' as 5x7 columns-in-bits, MSB leftmost. */

  static const uint8_t glyphs[5][GC9D01_HELLO_ROWS] =
  {
    {
      0x10, 0x10, 0x16, 0x19, 0x11, 0x11, 0x11
    },                                              /* h */
    {
      0x00, 0x00, 0x0e, 0x11, 0x1f, 0x10, 0x0e
    },                                              /* e */
    {
      0x0c, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0e
    },                                              /* l */
    {
      0x0c, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0e
    },                                              /* l */
    {
      0x00, 0x00, 0x0e, 0x11, 0x11, 0x11, 0x0e
    }                                               /* o */
  };

  FAR struct gc9d01_fb_s *priv;
  FAR uint16_t *px;
  int text_w = 5 * (GC9D01_HELLO_COLS + 1) - 1;
  int x0 = (GC9D01_XRES - text_w * GC9D01_HELLO_SCALE) / 2;
  int y0 = (GC9D01_YRES - GC9D01_HELLO_ROWS * GC9D01_HELLO_SCALE) / 2;
  int i;
  int g;
  int row;
  int col;

  if (display < 0 || display >= GC9D01_NDISPLAYS)
    {
      return -EINVAL;
    }

  priv = &g_gc9d01_fb[display];
  px = (FAR uint16_t *)priv->fbmem;

  if (px == NULL)
    {
      return -ENODEV;
    }

  for (i = 0; i < GC9D01_XRES * GC9D01_YRES; i++)
    {
      px[i] = GC9D01_HELLO_BG;
    }

  for (g = 0; g < 5; g++)
    {
      int gx = x0 + g * (GC9D01_HELLO_COLS + 1) * GC9D01_HELLO_SCALE;

      for (row = 0; row < GC9D01_HELLO_ROWS; row++)
        {
          for (col = 0; col < GC9D01_HELLO_COLS; col++)
            {
              int sx;
              int sy;

              if ((glyphs[g][row] &
                   (1u << (GC9D01_HELLO_COLS - 1 - col))) == 0)
                {
                  continue;
                }

              for (sy = 0; sy < GC9D01_HELLO_SCALE; sy++)
                {
                  int y = y0 + row * GC9D01_HELLO_SCALE + sy;

                  for (sx = 0; sx < GC9D01_HELLO_SCALE; sx++)
                    {
                      int x = gx + col * GC9D01_HELLO_SCALE + sx;

                      if (x >= 0 && x < GC9D01_XRES &&
                          y >= 0 && y < GC9D01_YRES)
                        {
                          px[y * GC9D01_XRES + x] = GC9D01_HELLO_FG;
                        }
                    }
                }
            }
        }
    }

  printf("gc9d01_fb[%d]: hello drawn (%dx%d at %d,%d)\n", display,
         text_w * GC9D01_HELLO_SCALE,
         GC9D01_HELLO_ROWS * GC9D01_HELLO_SCALE, x0, y0);

  return gc9d01_push(priv);
}

/****************************************************************************
 * Name: bk7258_gc9d01_fb_test_pattern
 *
 * Description:
 *   Draws four quadrants (red / green / blue / white) with a black border.
 *   Colour order and geometry are both wrong-detectable by eye: a swapped
 *   byte order turns red into something else, and a stride mistake shears
 *   the quadrant boundaries.
 *
 ****************************************************************************/

int bk7258_gc9d01_fb_test_pattern(int display)
{
  FAR struct gc9d01_fb_s *priv;
  FAR uint16_t *px;
  int x;
  int y;

  if (display < 0 || display >= GC9D01_NDISPLAYS)
    {
      return -EINVAL;
    }

  priv = &g_gc9d01_fb[display];
  px = (FAR uint16_t *)priv->fbmem;

  if (px == NULL)
    {
      return -ENODEV;
    }

  for (y = 0; y < GC9D01_YRES; y++)
    {
      for (x = 0; x < GC9D01_XRES; x++)
        {
          uint16_t c;

          if (x < 4 || y < 4 || x >= GC9D01_XRES - 4 ||
              y >= GC9D01_YRES - 4)
            {
              c = 0x0000;                                  /* black edge */
            }
          else if (y < GC9D01_YRES / 2)
            {
              c = (x < GC9D01_XRES / 2) ? 0xf800 : 0x07e0;  /* red green */
            }
          else
            {
              c = (x < GC9D01_XRES / 2) ? 0x001f : 0xffff;  /* blue white */
            }

          px[y * GC9D01_XRES + x] = c;
        }
    }

  printf("gc9d01_fb[%d]: test pattern drawn (TL red, TR green, BL blue, "
         "BR white, 4px black border)\n", display);

  return gc9d01_push(priv);
}
