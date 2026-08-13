/****************************************************************************
 * apps/hello_screen/hello_screen_main.c
 *
 * Boot-greeting animation: the text is written on the panels the way a pen
 * would draw it.
 *
 * Usage:
 *   hello                      write "hello" once
 *   hello ni hao               any text (the font is single-case, a-z . , - !)
 *   hello ms=1600 hold=1200    reveal time and how long to hold the result
 *   hello loop                 repeat until Ctrl-C
 *   hello em=52 thick=4        glyph size and pen width, in pixels
 *   hello fb=0                 one panel only (default: every panel)
 *
 * Only the newly written stroke is drawn each frame -- the pen adds ink, it
 * never repaints -- so a frame costs one small stroke plus the panel push.
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <sys/ioctl.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>

#include <nuttx/video/fb.h>

#include "hello_paint.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define HS_MAX_PANELS   2
#define HS_FP           16
#define HS_TEXT_MAX     64

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct hs_panel_s
{
  int                  fd;
  int                  idx;
  uint8_t             *mem;
  struct fb_videoinfo_s vinfo;
  struct fb_planeinfo_s pinfo;
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static volatile sig_atomic_t g_stop;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void hs_sigint(int signo)
{
  (void)signo;
  g_stop = 1;
}

static uint32_t hs_now_ms(void)
{
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

/****************************************************************************
 * Name: hs_ease
 *
 * Description:
 *   Smoothstep on 0..1000.  A constant pen speed looks mechanical and a
 *   quadratic ease-out starts too abruptly; smoothstep accelerates into the
 *   stroke and settles out of it, which is what reads as a hand writing.
 *
 ****************************************************************************/

static int32_t hs_ease(int32_t permille)
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

static int hs_open_panels(struct hs_panel_s *p, int want, int *npanels)
{
  int n = 0;
  int display;

  for (display = 0; display < HS_MAX_PANELS; display++)
    {
      char path[16];
      int fd;

      if (want >= 0 && want != display)
        {
          continue;
        }

      snprintf(path, sizeof(path), "/dev/fb%d", display);
      fd = open(path, O_RDWR);
      if (fd < 0)
        {
          continue;
        }

      if (ioctl(fd, FBIOGET_VIDEOINFO, (uintptr_t)&p[n].vinfo) < 0 ||
          ioctl(fd, FBIOGET_PLANEINFO, (uintptr_t)&p[n].pinfo) < 0)
        {
          printf("hello: fb%d info failed: %d\n", display, errno);
          close(fd);
          continue;
        }

      p[n].mem = mmap(NULL, p[n].pinfo.fblen, PROT_READ | PROT_WRITE,
                      MAP_SHARED, fd, 0);
      if (p[n].mem == MAP_FAILED)
        {
          printf("hello: fb%d mmap failed: %d\n", display, errno);
          close(fd);
          continue;
        }

      p[n].fd  = fd;
      p[n].idx = display;
      n++;
    }

  *npanels = n;
  return n > 0 ? 0 : -ENODEV;
}

static void hs_push(struct hs_panel_s *p, int n)
{
  int i;

  for (i = 0; i < n; i++)
    {
      struct fb_area_s area;

      area.x = 0;
      area.y = 0;
      area.w = p[i].vinfo.xres;
      area.h = p[i].vinfo.yres;

      if (ioctl(p[i].fd, FBIO_UPDATE, (uintptr_t)&area) < 0)
        {
          printf("hello: fb%d FBIO_UPDATE failed: %d\n", p[i].idx, errno);
        }
    }
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(int argc, FAR char *argv[])
{
  struct hs_panel_s panel[HS_MAX_PANELS];
  struct hs_style_s st;
  char text[HS_TEXT_MAX];
  uint32_t reveal_ms = 1400;
  uint32_t hold_ms = 1000;
  bool loop = false;
  int want_fb = -1;
  int em = 44;
  int thick = 3;
  int npanels = 0;
  int32_t total;
  int32_t width16;
  int i;

  /* NuttX keeps a builtin's statics across invocations, so a Ctrl-C from a
   * previous run would otherwise make every later run exit immediately.
   */

  g_stop = 0;
  text[0] = '\0';

  for (i = 1; i < argc; i++)
    {
      const char *a = argv[i];

      if (strncmp(a, "ms=", 3) == 0)
        {
          reveal_ms = (uint32_t)atoi(a + 3);
        }
      else if (strncmp(a, "hold=", 5) == 0)
        {
          hold_ms = (uint32_t)atoi(a + 5);
        }
      else if (strncmp(a, "em=", 3) == 0)
        {
          em = atoi(a + 3);
        }
      else if (strncmp(a, "thick=", 6) == 0)
        {
          thick = atoi(a + 6);
        }
      else if (strncmp(a, "fb=", 3) == 0)
        {
          want_fb = atoi(a + 3);
        }
      else if (strcmp(a, "loop") == 0)
        {
          loop = true;
        }
      else
        {
          /* Everything else is the text, so 'hello ni hao' works. */

          if (text[0] != '\0')
            {
              strncat(text, " ", sizeof(text) - strlen(text) - 1);
            }

          strncat(text, a, sizeof(text) - strlen(text) - 1);
        }
    }

  if (text[0] == '\0')
    {
      strncpy(text, "hello", sizeof(text) - 1);
      text[sizeof(text) - 1] = '\0';
    }

  if (em < 8)
    {
      em = 8;
    }

  if (thick < 1)
    {
      thick = 1;
    }

  if (hs_open_panels(panel, want_fb, &npanels) < 0)
    {
      printf("hello: no framebuffer\n");
      return EXIT_FAILURE;
    }

  memset(&st, 0, sizeof(st));
  st.fg         = 0xffff;
  st.bg         = 0x0000;
  st.em16       = (int32_t)em * HS_FP;
  st.thick16    = (int32_t)thick * HS_FP;
  st.cx16       = 0;
  st.baseline16 = ((int32_t)panel[0].vinfo.yres * HS_FP) / 2 +
                  (st.em16 * 10) / 64;

  total = hs_measure(text, &st, &width16);

  /* Auto-fit: the glass is round, so the usable width is a chord rather than
   * the full 160 pixels.  85% of the panel keeps the ends of the word clear
   * of the bezel.  Without this, anything longer than about six characters
   * runs off the left edge and the first letter loses its ascender -- which
   * looks like a font bug rather than an overflow.
   */

  {
    int32_t budget = ((int32_t)panel[0].vinfo.xres * HS_FP * 85) / 100;

    if (width16 > budget && width16 > 0)
      {
        st.em16 = (st.em16 * budget) / width16;
        st.thick16 = (st.thick16 * budget) / width16;

        if (st.thick16 < 2 * HS_FP)
          {
            st.thick16 = 2 * HS_FP;
          }

        st.baseline16 = ((int32_t)panel[0].vinfo.yres * HS_FP) / 2 +
                        (st.em16 * 10) / 64;
        total = hs_measure(text, &st, &width16);
        printf("hello: text is wide, em scaled to %dpx\n",
               (int)(st.em16 / HS_FP));
      }
  }

  if (total <= 0)
    {
      printf("hello: '%s' has nothing this font can draw "
             "(a-z, space, . , - !)\n", text);
      return EXIT_FAILURE;
    }

  st.cx16 = ((int32_t)panel[0].vinfo.xres * HS_FP - width16) / 2;

  printf("hello: '%s' on %d panel(s), %dpx em, pen %dpx, "
         "path %d px, reveal %ums\n", text, npanels, em, thick,
         (int)(total / HS_FP), (unsigned int)reveal_ms);

  {
    struct sigaction act;

    memset(&act, 0, sizeof(act));
    act.sa_handler = hs_sigint;
    sigemptyset(&act.sa_mask);
    (void)sigaction(SIGINT, &act, NULL);
  }

  do
    {
      uint32_t start = hs_now_ms();
      int32_t drawn = 0;
      uint32_t frames = 0;
      uint32_t ink_ms = 0;
      uint32_t push_ms = 0;

      for (i = 0; i < npanels; i++)
        {
          hs_clear(panel[i].mem, panel[i].pinfo.stride,
                   panel[i].vinfo.xres, panel[i].vinfo.yres, &st);
        }

      hs_push(panel, npanels);

      while (drawn < total && !g_stop)
        {
          uint32_t t = hs_now_ms() - start;
          int32_t target;
          uint32_t t0;
          uint32_t t1;

          if (reveal_ms == 0)
            {
              target = total;
            }
          else
            {
              target = (int32_t)(((int64_t)total *
                        hs_ease((int32_t)((t * 1000) / reveal_ms))) / 1000);
            }

          if (target <= drawn)
            {
              usleep(10000);
              continue;
            }

          t0 = hs_now_ms();

          for (i = 0; i < npanels; i++)
            {
              hs_stroke(panel[i].mem, panel[i].pinfo.stride,
                        panel[i].vinfo.xres, panel[i].vinfo.yres,
                        text, &st, drawn, target);
            }

          t1 = hs_now_ms();
          hs_push(panel, npanels);
          ink_ms += t1 - t0;
          push_ms += hs_now_ms() - t1;
          frames++;
          drawn = target;
        }

      printf("hello: written in %ums, %u frame(s), ink=%ums push=%ums\n",
             (unsigned int)(hs_now_ms() - start), (unsigned int)frames,
             (unsigned int)ink_ms, (unsigned int)push_ms);

      if (hold_ms > 0 && !g_stop)
        {
          usleep(hold_ms * 1000);
        }
    }
  while (loop && !g_stop);

  return EXIT_SUCCESS;
}
