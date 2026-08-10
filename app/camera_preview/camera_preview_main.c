/****************************************************************************
 * app/camera_preview/camera_preview_main.c
 *
 * Live camera preview: GC2145 (640x480 UYVY) -> GC9D01 (160x160 RGB565).
 *
 * Why this exists instead of `nxcamera; output /dev/fb0`: nxcamera's
 * show_image() cannot serve this board.  It passes the camera's own width
 * and height as both source and crop rectangle and gives libyuv only the
 * framebuffer's stride, so it would write a 640x480 image into a 160x160
 * framebuffer and run off the end of it; and for a non-I420 source on an
 * RGB565 panel it allocates width*height*3/2 (460800 bytes) from the
 * kernel heap, which on this board holds roughly 300KB.  nxcamera also
 * lives in the shared apps repository, outside this project's scope, so it
 * is left alone.
 *
 * Conversion: a single pass that samples the source directly into the
 * framebuffer, with no intermediate buffer at all.
 *
 *   centre-crop 480x480 out of 640x480, then take every 3rd pixel
 *   (480 / 160 == 3 exactly, so nearest-neighbour needs no arithmetic
 *   beyond an integer stride) and convert that UYVY pair to RGB565.
 *
 * Going through libyuv would have meant staging I420 at crop size
 * (480*480*3/2 = 345600 bytes) and again at panel size, i.e. exactly the
 * allocation that defeats nxcamera on this board.  Sampling directly also
 * reads only ~100KB of the 614400-byte source frame per displayed frame,
 * which matters because PSRAM bandwidth is already the tightest resource
 * here (the capture path alone runs at ~37MB/s).
 *
 * The centre crop is what keeps the picture from being stretched: the panel
 * is a 160x160 round display, so a 4:3 frame is cropped to its central
 * square first.  The corners of that square are not visible on a round
 * panel -- that is a composition matter, not something this code hides.
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#include <nuttx/config.h>

#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

#include <nuttx/video/fb.h>
#include <sys/videoio.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define CAM_DEV        "/dev/video0"
#define FB_DEV         "/dev/fb0"

#define CAM_WIDTH      640
#define CAM_HEIGHT     480

/* Centre square of the 4:3 sensor frame; 480x480 keeps the full height. */

#define CROP_SIZE      480
#define CROP_X         ((CAM_WIDTH - CROP_SIZE) / 2)

#define NBUFFERS       3

/* Report progress this often so the console does not drown in output. */

#define REPORT_FRAMES  30

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct preview_s
{
  int camfd;
  int fbfd;

  FAR uint8_t *bufs[NBUFFERS];
  uint32_t buf_sizes[NBUFFERS];
  int nbuffers;

  struct fb_videoinfo_s vinfo;
  struct fb_planeinfo_s pinfo;
  FAR uint8_t *fbmem;
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static uint32_t now_ms(void)
{
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

static int preview_open_fb(FAR struct preview_s *p)
{
  p->fbfd = open(FB_DEV, O_RDWR);
  if (p->fbfd < 0)
    {
      printf("preview: open %s failed: %d\n", FB_DEV, errno);
      return -errno;
    }

  if (ioctl(p->fbfd, FBIOGET_VIDEOINFO, (uintptr_t)&p->vinfo) < 0)
    {
      printf("preview: FBIOGET_VIDEOINFO failed: %d\n", errno);
      return -errno;
    }

  if (ioctl(p->fbfd, FBIOGET_PLANEINFO, (uintptr_t)&p->pinfo) < 0)
    {
      printf("preview: FBIOGET_PLANEINFO failed: %d\n", errno);
      return -errno;
    }

  printf("preview: fb %ux%u fmt=%u bpp=%u stride=%u fblen=%u\n",
         p->vinfo.xres, p->vinfo.yres, p->vinfo.fmt,
         p->pinfo.bpp, (unsigned int)p->pinfo.stride,
         (unsigned int)p->pinfo.fblen);

  if (p->vinfo.fmt != FB_FMT_RGB16_565)
    {
      printf("preview: only RGB565 panels are supported (fmt=%u)\n",
             p->vinfo.fmt);
      return -ENOTSUP;
    }

  p->fbmem = mmap(NULL, p->pinfo.fblen, PROT_READ | PROT_WRITE,
                  MAP_SHARED | MAP_FILE, p->fbfd, 0);
  if (p->fbmem == MAP_FAILED)
    {
      printf("preview: fb mmap failed: %d\n", errno);
      return -errno;
    }

  return OK;
}

static int preview_open_camera(FAR struct preview_s *p)
{
  struct v4l2_requestbuffers req;
  struct v4l2_buffer buf;
  struct v4l2_format fmt;
  int i;

  p->camfd = open(CAM_DEV, O_RDWR);
  if (p->camfd < 0)
    {
      printf("preview: open %s failed: %d\n", CAM_DEV, errno);
      return -errno;
    }

  memset(&fmt, 0, sizeof(fmt));
  fmt.type                = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  fmt.fmt.pix.width       = CAM_WIDTH;
  fmt.fmt.pix.height      = CAM_HEIGHT;
  fmt.fmt.pix.field       = V4L2_FIELD_ANY;

  /* UYVY: the byte order this hardware actually writes into the frame
   * buffer, as advertised by the imgsensor driver.
   */

  fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_UYVY;

  if (ioctl(p->camfd, VIDIOC_S_FMT, (uintptr_t)&fmt) < 0)
    {
      printf("preview: VIDIOC_S_FMT failed: %d\n", errno);
      return -errno;
    }

  memset(&req, 0, sizeof(req));
  req.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  req.memory = V4L2_MEMORY_MMAP;
  req.count  = NBUFFERS;

  if (ioctl(p->camfd, VIDIOC_REQBUFS, (uintptr_t)&req) < 0)
    {
      printf("preview: VIDIOC_REQBUFS failed: %d\n", errno);
      return -errno;
    }

  p->nbuffers = req.count < NBUFFERS ? req.count : NBUFFERS;

  for (i = 0; i < p->nbuffers; i++)
    {
      memset(&buf, 0, sizeof(buf));
      buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
      buf.memory = V4L2_MEMORY_MMAP;
      buf.index  = i;

      if (ioctl(p->camfd, VIDIOC_QUERYBUF, (uintptr_t)&buf) < 0)
        {
          printf("preview: VIDIOC_QUERYBUF(%d) failed: %d\n", i, errno);
          return -errno;
        }

      p->bufs[i] = mmap(NULL, buf.length, PROT_READ | PROT_WRITE,
                        MAP_SHARED, p->camfd, buf.m.offset);
      if (p->bufs[i] == MAP_FAILED)
        {
          printf("preview: buffer %d mmap failed: %d\n", i, errno);
          return -errno;
        }

      p->buf_sizes[i] = buf.length;

      if (ioctl(p->camfd, VIDIOC_QBUF, (uintptr_t)&buf) < 0)
        {
          printf("preview: VIDIOC_QBUF(%d) failed: %d\n", i, errno);
          return -errno;
        }
    }

  printf("preview: camera %dx%d UYVY, %d buffers of %u bytes\n",
         CAM_WIDTH, CAM_HEIGHT, p->nbuffers,
         (unsigned int)p->buf_sizes[0]);

  return OK;
}

/****************************************************************************
 * Name: preview_convert
 *
 * Description:
 *   One frame: centre-crop, 3:1 nearest-neighbour downscale and UYVY to
 *   RGB565, straight from the capture buffer into the framebuffer.
 *
 *   UYVY packs two pixels per 4 bytes as U Y0 V Y1, so a source x maps to
 *   quad (x >> 1) and picks Y0 or Y1 by x's parity.  BT.601 coefficients
 *   are used in 8-bit fixed point; the shifts are chosen so the whole
 *   conversion stays in 32-bit integer math.
 *
 ****************************************************************************/

static int preview_convert(FAR struct preview_s *p, int index)
{
  FAR const uint8_t *src = p->bufs[index];
  int dw = p->vinfo.xres;
  int dh = p->vinfo.yres;
  int step = CROP_SIZE / dw;          /* 480 / 160 = 3 */
  int dx;
  int dy;

  if (step < 1 || dh * step > CAM_HEIGHT)
    {
      printf("preview: %dx%d panel does not divide the %dx%d crop\n",
             dw, dh, CROP_SIZE, CROP_SIZE);
      return -EINVAL;
    }

  for (dy = 0; dy < dh; dy++)
    {
      FAR const uint8_t *srow = src + (size_t)(dy * step) * CAM_WIDTH * 2;
      FAR uint16_t *drow = (FAR uint16_t *)(p->fbmem +
                                            (size_t)dy * p->pinfo.stride);

      for (dx = 0; dx < dw; dx++)
        {
          int sx = CROP_X + dx * step;
          FAR const uint8_t *q = srow + (size_t)(sx >> 1) * 4;
          int u = (int)q[0] - 128;
          int v = (int)q[2] - 128;
          int y = (int)((sx & 1) ? q[3] : q[1]) - 16;
          int r;
          int g;
          int b;

          if (y < 0)
            {
              y = 0;
            }

          y *= 298;

          r = (y + 409 * v + 128) >> 8;
          g = (y - 100 * u - 208 * v + 128) >> 8;
          b = (y + 516 * u + 128) >> 8;

          r = r < 0 ? 0 : (r > 255 ? 255 : r);
          g = g < 0 ? 0 : (g > 255 ? 255 : g);
          b = b < 0 ? 0 : (b > 255 ? 255 : b);

          drow[dx] = (uint16_t)(((r & 0xf8) << 8) |
                                ((g & 0xfc) << 3) |
                                 (b >> 3));
        }
    }

  return OK;
}

static void preview_cleanup(FAR struct preview_s *p)
{
  int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  int i;

  if (p->camfd >= 0)
    {
      ioctl(p->camfd, VIDIOC_STREAMOFF, (uintptr_t)&type);

      for (i = 0; i < p->nbuffers; i++)
        {
          if (p->bufs[i] != NULL && p->bufs[i] != MAP_FAILED)
            {
              munmap(p->bufs[i], p->buf_sizes[i]);
            }
        }

      close(p->camfd);
    }

  if (p->fbfd >= 0)
    {
      if (p->fbmem != NULL && p->fbmem != MAP_FAILED)
        {
          munmap(p->fbmem, p->pinfo.fblen);
        }

      close(p->fbfd);
    }

}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(int argc, FAR char *argv[])
{
  struct preview_s p;
  struct fb_area_s area;
  struct v4l2_buffer buf;
  int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  uint32_t max_frames = 0;
  uint32_t frames = 0;
  uint32_t errors = 0;
  uint32_t conv_ms = 0;
  uint32_t push_ms = 0;
  uint32_t t_start;
  uint32_t t0;
  int ret;

  if (argc > 1)
    {
      max_frames = (uint32_t)atoi(argv[1]);
    }

  printf("preview: starting (%s -> %s), frames=%s\n", CAM_DEV, FB_DEV,
         max_frames ? argv[1] : "unlimited");

  memset(&p, 0, sizeof(p));
  p.camfd = -1;
  p.fbfd  = -1;

  ret = preview_open_fb(&p);
  if (ret < 0)
    {
      goto out;
    }

  ret = preview_open_camera(&p);
  if (ret < 0)
    {
      goto out;
    }

  if (ioctl(p.camfd, VIDIOC_STREAMON, (uintptr_t)&type) < 0)
    {
      printf("preview: VIDIOC_STREAMON failed: %d\n", errno);
      ret = -errno;
      goto out;
    }

  area.x = 0;
  area.y = 0;
  area.w = p.vinfo.xres;
  area.h = p.vinfo.yres;

  t_start = now_ms();

  for (; ; )
    {
      memset(&buf, 0, sizeof(buf));
      buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
      buf.memory = V4L2_MEMORY_MMAP;

      if (ioctl(p.camfd, VIDIOC_DQBUF, (uintptr_t)&buf) < 0)
        {
          printf("preview: VIDIOC_DQBUF failed: %d\n", errno);
          break;
        }

      /* The imgdata driver reports timed-out frames with
       * V4L2_BUF_FLAG_ERROR rather than blocking forever; skip those
       * instead of showing stale pixels.
       */

      if ((buf.flags & V4L2_BUF_FLAG_ERROR) != 0)
        {
          errors++;
        }
      else
        {
          t0 = now_ms();
          if (preview_convert(&p, buf.index) == OK)
            {
              conv_ms += now_ms() - t0;

              t0 = now_ms();
              if (ioctl(p.fbfd, FBIO_UPDATE, (uintptr_t)&area) < 0)
                {
                  printf("preview: FBIO_UPDATE failed: %d\n", errno);
                }

              push_ms += now_ms() - t0;
              frames++;
            }
          else
            {
              errors++;
            }
        }

      if (ioctl(p.camfd, VIDIOC_QBUF, (uintptr_t)&buf) < 0)
        {
          printf("preview: VIDIOC_QBUF failed: %d\n", errno);
          break;
        }

      if (frames > 0 && (frames % REPORT_FRAMES) == 0)
        {
          uint32_t el = now_ms() - t_start;

          printf("preview: %u frames in %ums = %u.%02u fps "
                 "(convert %ums/f, push %ums/f, %u errors)\n",
                 (unsigned int)frames, (unsigned int)el,
                 (unsigned int)(el ? frames * 1000 / el : 0),
                 (unsigned int)(el ? frames * 100000 / el % 100 : 0),
                 (unsigned int)(conv_ms / frames),
                 (unsigned int)(push_ms / frames),
                 (unsigned int)errors);
        }

      if (max_frames != 0 && frames >= max_frames)
        {
          break;
        }
    }

  printf("preview: done, %u frames, %u errors\n",
         (unsigned int)frames, (unsigned int)errors);
  ret = OK;

out:
  preview_cleanup(&p);
  return ret < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
