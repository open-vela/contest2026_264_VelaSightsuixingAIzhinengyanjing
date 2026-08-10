/****************************************************************************
 * board/beken/boards/bk7258/bk7258-ap/src/bk7258_gc9d01_fb.c
 *
 * openvela framebuffer driver for the GC9D01 panel (160x160 round, RGB565,
 * driven over QSPI1).  Implements the three hooks fb_register() needs --
 * up_fbinitialize(), up_fbgetvplane(), up_fbuninitialize() -- plus the
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
 * Panel geometry comes from the vendor's own device table
 * (bk_avdk_smp/ap/components/bk_peripheral/src/lcd/spi/lcd_spi_gc9d01.c:
 * lcd_device_gc9d01 = { .width = 160, .height = 160 }), so one RGB565
 * frame is 160*160*2 = 51200 bytes -- small enough to live in the kernel
 * heap instead of competing with the camera for PSRAM bandwidth.
 *
 * The panel is round: the corners of the 160x160 square are not visible.
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

#define GC9D01_XRES        160
#define GC9D01_YRES        160
#define GC9D01_BPP         16
#define GC9D01_STRIDE      (GC9D01_XRES * 2)
#define GC9D01_FBLEN       (GC9D01_STRIDE * GC9D01_YRES)

/* Column/row address set and memory write, standard MIPI DCS opcodes that
 * the GC9-series follows.
 */

#define GC9D01_CMD_CASET   0x2A
#define GC9D01_CMD_RASET   0x2B

/****************************************************************************
 * Private Data
 ****************************************************************************/

static FAR uint8_t *g_fbmem;
static uint32_t g_updates;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static int gc9d01_getvideoinfo(FAR struct fb_vtable_s *vtable,
                               FAR struct fb_videoinfo_s *vinfo)
{
  if (vinfo == NULL)
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
  if (pinfo == NULL || planeno != 0)
    {
      return -EINVAL;
    }

  /* Single buffer: fbcount is 1, so yres_virtual == yres and fblen is one
   * frame.  See the openvela Framebuffer guide's field table -- when a
   * second buffer is added later, fblen/yres_virtual must grow with it and
   * this function has to answer per 'display' index.
   */

  pinfo->fbmem        = g_fbmem;
  pinfo->fblen        = GC9D01_FBLEN;
  pinfo->stride       = GC9D01_STRIDE;
  pinfo->display      = 0;
  pinfo->bpp          = GC9D01_BPP;
  pinfo->xres_virtual = GC9D01_XRES;
  pinfo->yres_virtual = GC9D01_YRES;
  pinfo->xoffset      = 0;
  pinfo->yoffset      = 0;

  return OK;
}

/****************************************************************************
 * Name: gc9d01_set_window
 *
 * Description:
 *   Programs the panel's drawing window.  CASET/RASET each take a 16-bit
 *   start and a 16-bit end, big-endian, so four payload bytes.
 *
 ****************************************************************************/

static bool gc9d01_set_window(uint16_t x0, uint16_t y0,
                              uint16_t x1, uint16_t y1)
{
  uint8_t args[4];

  args[0] = (uint8_t)(x0 >> 8);
  args[1] = (uint8_t)(x0 & 0xff);
  args[2] = (uint8_t)(x1 >> 8);
  args[3] = (uint8_t)(x1 & 0xff);
  if (!bk7258_qspi0_send_cmd(GC9D01_CMD_CASET, args, 4))
    {
      return false;
    }

  args[0] = (uint8_t)(y0 >> 8);
  args[1] = (uint8_t)(y0 & 0xff);
  args[2] = (uint8_t)(y1 >> 8);
  args[3] = (uint8_t)(y1 & 0xff);

  return bk7258_qspi0_send_cmd(GC9D01_CMD_RASET, args, 4);
}

static int gc9d01_updatearea(FAR struct fb_vtable_s *vtable,
                             FAR const struct fb_area_s *area)
{
  /* Partial updates would need a per-row copy out of the framebuffer,
   * because the panel expects a densely packed rectangle while the
   * framebuffer rows are GC9D01_STRIDE apart.  At 51200 bytes a full frame
   * is cheap enough (~7ms at 60MHz) that the first implementation always
   * pushes everything and ignores the requested rectangle.
   */

  clock_t start = clock_systime_ticks();

  UNUSED(area);

  if (g_fbmem == NULL)
    {
      return -ENODEV;
    }

  if (!gc9d01_set_window(0, 0, GC9D01_XRES - 1, GC9D01_YRES - 1))
    {
      return -EIO;
    }

  if (!bk7258_qspi1_lcd_write_frame(g_fbmem, GC9D01_FBLEN))
    {
      printf("gc9d01_fb: pixel burst failed\n");
      return -EIO;
    }

  g_updates++;

  /* First few pushes get a timing report: it is the only way to know the
   * real cost of a frame on this bus, which decides whether the preview
   * path needs DMA.  After that stay quiet -- this runs per displayed
   * frame.
   */

  if (g_updates <= 3)
    {
      uint32_t ms = TICK2MSEC(clock_systime_ticks() - start);

      printf("gc9d01_fb: update %u: %d bytes in %ums (%u frames on bus)\n",
             (unsigned int)g_updates, GC9D01_FBLEN, (unsigned int)ms,
             (unsigned int)bk7258_qspi1_lcd_frame_count());
    }

  return OK;
}

static struct fb_vtable_s g_gc9d01_vtable =
{
  .getvideoinfo = gc9d01_getvideoinfo,
  .getplaneinfo = gc9d01_getplaneinfo,
#ifdef CONFIG_FB_UPDATE
  .updatearea   = gc9d01_updatearea,
#endif
};

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int up_fbinitialize(int display)
{
  if (display != 0)
    {
      return -EINVAL;
    }

  if (g_fbmem == NULL)
    {
      g_fbmem = kmm_zalloc(GC9D01_FBLEN);
      if (g_fbmem == NULL)
        {
          printf("gc9d01_fb: cannot allocate %d-byte framebuffer\n",
                 GC9D01_FBLEN);
          return -ENOMEM;
        }
    }

  /* Panel reset + QSPI + init command sequence.  Kept in
   * bk7258_gc9d01.c so the command table has a single owner.
   */

  if (bk7258_gc9d01_panel_init() < 0)
    {
      printf("gc9d01_fb: panel init failed\n");
      kmm_free(g_fbmem);
      g_fbmem = NULL;
      return -EIO;
    }

  printf("gc9d01_fb: %dx%d RGB565, framebuffer %d bytes at %p\n",
         GC9D01_XRES, GC9D01_YRES, GC9D01_FBLEN, g_fbmem);

  return OK;
}

FAR struct fb_vtable_s *up_fbgetvplane(int display, int vplane)
{
  if (display != 0 || vplane != 0)
    {
      return NULL;
    }

  return &g_gc9d01_vtable;
}

void up_fbuninitialize(int display)
{
  UNUSED(display);
}

/****************************************************************************
 * Name: bk7258_gc9d01_fb_fill
 *
 * Description:
 *   Bring-up helper: fills the framebuffer with a solid RGB565 colour and
 *   pushes it.  This is the first thing that can prove the whole chain
 *   (QSPI framing, init sequence, CASET/RASET, pixel burst) actually
 *   reaches the glass -- the panel has no readable ID register, so until
 *   something is visible there is no evidence the init sequence worked.
 *
 ****************************************************************************/

int bk7258_gc9d01_fb_fill(uint16_t rgb565)
{
  uint16_t *px = (uint16_t *)g_fbmem;
  int i;

  if (g_fbmem == NULL)
    {
      return -ENODEV;
    }

  for (i = 0; i < GC9D01_XRES * GC9D01_YRES; i++)
    {
      px[i] = rgb565;
    }

  printf("gc9d01_fb: fill 0x%04x pushed\n", rgb565);

  return gc9d01_updatearea(&g_gc9d01_vtable, NULL);
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

int bk7258_gc9d01_fb_test_pattern(void)
{
  uint16_t *px = (uint16_t *)g_fbmem;
  int x;
  int y;

  if (g_fbmem == NULL)
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

  printf("gc9d01_fb: test pattern drawn (TL red 0xf800, TR green 0x07e0, "
         "BL blue 0x001f, BR white, 4px black border)\n");

  return gc9d01_updatearea(&g_gc9d01_vtable, NULL);
}
