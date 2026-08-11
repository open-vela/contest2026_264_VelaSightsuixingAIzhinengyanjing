/****************************************************************************
 * app/jpeg_test/jpeg_test_main.c
 *
 * Exercise the BK7258 V4L2 M2M JPEG encoder without a camera.
 *
 * The encoder takes raw YUV from memory, so it can be driven entirely from a
 * synthetic frame -- which is the point: this validates the codec while the
 * camera capture path is still being built, and keeps the two pieces of work
 * independent.
 *
 * The generated frame is deliberately not noise.  Colour bars plus a
 * vertical luma ramp compress the way a real photo does (flat regions, hard
 * edges) and, more importantly, are recognisable by eye: after 'dump' the
 * JPEG can be
 * rebuilt on a PC and a wrong plane order, a swapped Cb/Cr, or an off-by-one
 * stride is obvious at a glance rather than hidden behind a plausible
 * compression ratio.
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <unistd.h>

#include <sys/videoio.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#ifndef CONFIG_JPEG_TEST_DEV_PATH
#  define CONFIG_JPEG_TEST_DEV_PATH "/dev/video1"
#endif

#define JPEG_TEST_NBUFFERS      2

#define JPEG_TEST_DEF_WIDTH     320
#define JPEG_TEST_DEF_HEIGHT    240
#define JPEG_TEST_DEF_QUALITY   80
#define JPEG_TEST_DEF_FRAMES    1

/* How long to wait for an encoded frame before giving up.  Software JPEG at
 * 320x240 is milliseconds of work, so anything approaching this means the
 * work queue never ran.
 */

/* Kept under the CP heartbeat's 2 s window on purpose.  A 5 s wait here plus
 * one frame of encoding added up to the six seconds of silence that made CP
 * assert and reset the board -- a test tool must fail its own run, not take
 * the system down with it.  A 320x240 frame measures ~830 ms, so 1500 ms is
 * generous while still bailing out before the heartbeat is at risk.
 */

/* How long to wait for one encoded frame.
 *
 * Measured cost is roughly 11 us per pixel (820 ms for 320x240), so a fixed
 * budget either starves large frames or lets a wedged driver hang the shell.
 * Scale it, with a floor for tiny frames.
 */

#define JPEG_TEST_TIMEOUT_FLOOR 1000
#define JPEG_TEST_US_PER_PIXEL  40
#define JPEG_TEST_POLL_MS       20

/* Bytes of JPEG per line of base64 output.  57 encodes to exactly 76
 * characters, which stays clear of the console's line length.
 */

#define JPEG_TEST_B64_CHUNK     57

/****************************************************************************
 * Private Types
 ****************************************************************************/

/* Frames the results array can hold.  Beyond this the run still encodes,
 * it just stops recording per-frame detail.
 */

#define JPEG_TEST_MAX_RESULTS   32

struct jpeg_test_buf_s
{
  void     *addr;
  size_t    length;

  /* What QUERYBUF reported.  Required on every subsequent QBUF: the driver
   * rebuilds its pointer from it, so dropping it corrupts the address.
   */

  uint32_t  offset;
};

/* One frame's outcome, recorded rather than printed.
 *
 * Printing inside the loop is what caused the resets: AP console output
 * travels over the mailbox UART0_TX channel, and while that channel is busy
 * bk7258_mailbox_send_wire() returns -EBUSY, which the heartbeat discards
 * without retrying.  Enough console traffic and CP stops seeing heartbeats.
 */

struct jpeg_test_result_s
{
  uint32_t bytes;
  uint32_t ms;
  bool     soi_ok;
  bool     eoi_ok;
  bool     driver_error;
};

struct jpeg_test_ctx_s
{
  int    fd;
  struct jpeg_test_buf_s out[JPEG_TEST_NBUFFERS];   /* raw YUV in  */
  struct jpeg_test_buf_s cap[JPEG_TEST_NBUFFERS];   /* JPEG out    */
  int    nout;
  int    ncap;
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* The most recent encoded frame, kept so 'dump' can run as a separate
 * command.  Sized on first use.
 */

static uint8_t *g_last_jpeg;
static size_t   g_last_len;
static uint32_t g_last_width;
static uint32_t g_last_height;

static const char g_b64[] =
  "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void jpeg_test_usage(void)
{
  printf("Usage: jpeg_test <enc|dump|info> [options]\n"
         "  enc   generate a synthetic YUV frame and encode it\n"
         "  dump  base64 the last encoded JPEG to the console\n"
         "  info  report the encoder's capabilities and formats\n"
         "  -w <px>  width, default %d\n"
         "  -h <px>  height, default %d\n"
         "  -q <1..100> quality, default %d\n"
         "  -f <i420|uyvy> input format, default i420\n"
         "  -n <count>  frames to encode, default %d\n",
         JPEG_TEST_DEF_WIDTH, JPEG_TEST_DEF_HEIGHT,
         JPEG_TEST_DEF_QUALITY, JPEG_TEST_DEF_FRAMES);
}

/****************************************************************************
 * Name: jpeg_test_now_ms
 ****************************************************************************/

static uint64_t jpeg_test_now_ms(void)
{
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/****************************************************************************
 * Name: jpeg_test_fill_i420
 *
 * Description:
 *   Eight colour bars over a top-to-bottom luma ramp, in planar 4:2:0.
 *
 *   Chroma is written at half resolution in both directions, so a driver
 *   that mixes up the Cb and Cr planes produces visibly wrong colours rather
 *   than a subtle shift.
 *
 ****************************************************************************/

static void jpeg_test_fill_i420(uint8_t *dst, uint32_t w, uint32_t h)
{
  /* Rec.601 chroma for white, yellow, cyan, green, magenta, red, blue and
   * black -- the classic bar order, so the result is recognisable.
   */

  static const uint8_t cb[8] =
    {
      128, 16, 166, 54, 202, 90, 240, 128
    };

  static const uint8_t cr[8] =
    {
      128, 146, 16, 34, 222, 240, 110, 128
    };

  uint8_t *y = dst;
  uint8_t *u = dst + (size_t)w * h;
  uint8_t *v = u + (size_t)w * h / 4;
  uint32_t row;
  uint32_t col;

  for (row = 0; row < h; row++)
    {
      uint8_t ramp = 40 + (uint8_t)((row * 175) / h);

      for (col = 0; col < w; col++)
        {
          uint32_t bar = col * 8 / w;

          /* Bar 7 stays black so the ramp shows up unmodulated
           * somewhere.
           */

          y[(size_t)row * w + col] = bar == 7 ? 16 : ramp;
        }
    }

  for (row = 0; row < h / 2; row++)
    {
      for (col = 0; col < w / 2; col++)
        {
          uint32_t bar = (col * 2) * 8 / w;

          u[(size_t)row * (w / 2) + col] = cb[bar];
          v[(size_t)row * (w / 2) + col] = cr[bar];
        }
    }
}

/****************************************************************************
 * Name: jpeg_test_fill_uyvy
 *
 * Description:
 *   The same picture as UYVY, so the two input paths can be compared against
 *   each other rather than only against expectations.
 *
 ****************************************************************************/

static void jpeg_test_fill_uyvy(uint8_t *dst, uint32_t w, uint32_t h)
{
  static const uint8_t cb[8] =
    {
      128, 16, 166, 54, 202, 90, 240, 128
    };

  static const uint8_t cr[8] =
    {
      128, 146, 16, 34, 222, 240, 110, 128
    };

  uint32_t row;
  uint32_t col;

  for (row = 0; row < h; row++)
    {
      uint8_t *p = dst + (size_t)row * w * 2;
      uint8_t ramp = 40 + (uint8_t)((row * 175) / h);

      for (col = 0; col + 1 < w; col += 2)
        {
          uint32_t bar = col * 8 / w;
          uint8_t luma = bar == 7 ? 16 : ramp;

          *p++ = cb[bar];
          *p++ = luma;
          *p++ = cr[bar];
          *p++ = luma;
        }
    }
}

/****************************************************************************
 * Name: jpeg_test_setup_queue
 *
 * Description:
 *   REQBUFS plus QUERYBUF and mmap for one queue.  CAPTURE buffers are
 *   queued immediately so the driver always has somewhere to put a result;
 *   OUTPUT buffers stay with us until there is a frame to put in them.
 *
 ****************************************************************************/

static int jpeg_test_setup_queue(struct jpeg_test_ctx_s *ctx,
                                 enum v4l2_buf_type type,
                                 struct jpeg_test_buf_s *bufs,
                                 int *count)
{
  struct v4l2_requestbuffers req;
  int i;

  memset(&req, 0, sizeof(req));
  req.count  = JPEG_TEST_NBUFFERS;
  req.memory = V4L2_MEMORY_MMAP;
  req.type   = type;

  if (ioctl(ctx->fd, VIDIOC_REQBUFS, &req) < 0)
    {
      printf("jpeg_test: REQBUFS(%s) failed: %d\n",
             V4L2_TYPE_IS_OUTPUT(type) ? "output" : "capture", errno);
      return -errno;
    }

  *count = req.count;

  for (i = 0; i < req.count; i++)
    {
      struct v4l2_buffer buf;

      memset(&buf, 0, sizeof(buf));
      buf.type   = type;
      buf.memory = V4L2_MEMORY_MMAP;
      buf.index  = i;

      if (ioctl(ctx->fd, VIDIOC_QUERYBUF, &buf) < 0)
        {
          printf("jpeg_test: QUERYBUF %d failed: %d\n", i, errno);
          return -errno;
        }

      bufs[i].length = buf.length;
      bufs[i].offset = buf.m.offset;
      bufs[i].addr   = mmap(NULL, buf.length, PROT_READ | PROT_WRITE,
                            MAP_SHARED, ctx->fd, buf.m.offset);
      if (bufs[i].addr == MAP_FAILED)
        {
          printf("jpeg_test: mmap %d failed: %d\n", i, errno);
          return -errno;
        }

      if (!V4L2_TYPE_IS_OUTPUT(type))
        {
          if (ioctl(ctx->fd, VIDIOC_QBUF, &buf) < 0)
            {
              printf("jpeg_test: QBUF(capture) %d failed: %d\n", i, errno);
              return -errno;
            }
        }
    }

  return 0;
}

/****************************************************************************
 * Name: jpeg_test_encode
 ****************************************************************************/

static int jpeg_test_encode(uint32_t width, uint32_t height, int quality,
                            uint32_t pixfmt, int nframes)
{
  struct jpeg_test_ctx_s ctx;
  struct v4l2_format fmt;
  struct v4l2_ext_controls ctrls;
  struct v4l2_ext_control ctrl;
  struct v4l2_capability cap;
  struct jpeg_test_result_s results[JPEG_TEST_MAX_RESULTS];
  uint32_t timeout_ms = JPEG_TEST_TIMEOUT_FLOOR +
                        (width * height * JPEG_TEST_US_PER_PIXEL) / 1000;
  size_t rawsize;
  int recorded = 0;
  int type;
  int frame;
  int ret = 0;
  int i;

  memset(results, 0, sizeof(results));

  memset(&ctx, 0, sizeof(ctx));

  ctx.fd = open(CONFIG_JPEG_TEST_DEV_PATH, O_RDWR);
  if (ctx.fd < 0)
    {
      printf("jpeg_test: cannot open %s: %d\n",
             CONFIG_JPEG_TEST_DEV_PATH, errno);
      return -errno;
    }

  memset(&cap, 0, sizeof(cap));
  if (ioctl(ctx.fd, VIDIOC_QUERYCAP, &cap) == 0)
    {
      printf("jpeg_test: %s [%s]\n", cap.driver, cap.card);
    }

  /* Input format first: the encoder derives the encoded geometry from it, so
   * setting CAPTURE first would just be overwritten.
   */

  memset(&fmt, 0, sizeof(fmt));
  fmt.type                = V4L2_BUF_TYPE_VIDEO_OUTPUT;
  fmt.fmt.pix.width       = width;
  fmt.fmt.pix.height      = height;
  fmt.fmt.pix.pixelformat = pixfmt;

  if (ioctl(ctx.fd, VIDIOC_S_FMT, &fmt) < 0)
    {
      printf("jpeg_test: S_FMT(output) failed: %d\n", errno);
      ret = -errno;
      goto out;
    }

  /* The driver may have adjusted the geometry; everything below uses what it
   * actually accepted, not what was asked for.
   */

  width   = fmt.fmt.pix.width;
  height  = fmt.fmt.pix.height;
  rawsize = fmt.fmt.pix.sizeimage;

  printf("jpeg_test: input %lux%lu %c%c%c%c, %zu bytes/frame\n",
         (unsigned long)width, (unsigned long)height,
         (int)(pixfmt & 0xff), (int)((pixfmt >> 8) & 0xff),
         (int)((pixfmt >> 16) & 0xff), (int)((pixfmt >> 24) & 0xff),
         rawsize);

  memset(&fmt, 0, sizeof(fmt));
  fmt.type                = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_JPEG;

  if (ioctl(ctx.fd, VIDIOC_S_FMT, &fmt) < 0)
    {
      printf("jpeg_test: S_FMT(capture) failed: %d\n", errno);
      ret = -errno;
      goto out;
    }

  memset(&ctrl, 0, sizeof(ctrl));
  memset(&ctrls, 0, sizeof(ctrls));
  ctrl.id       = V4L2_CID_JPEG_COMPRESSION_QUALITY;
  ctrl.value    = quality;
  ctrls.count    = 1;
  ctrls.controls = &ctrl;

  if (ioctl(ctx.fd, VIDIOC_S_EXT_CTRLS, &ctrls) < 0)
    {
      printf("jpeg_test: quality request rejected: %d\n", errno);
    }

  ret = jpeg_test_setup_queue(&ctx, V4L2_BUF_TYPE_VIDEO_OUTPUT,
                              ctx.out, &ctx.nout);
  if (ret < 0)
    {
      goto out;
    }

  ret = jpeg_test_setup_queue(&ctx, V4L2_BUF_TYPE_VIDEO_CAPTURE,
                              ctx.cap, &ctx.ncap);
  if (ret < 0)
    {
      goto out;
    }

  type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
  if (ioctl(ctx.fd, VIDIOC_STREAMON, &type) < 0)
    {
      printf("jpeg_test: STREAMON(output) failed: %d\n", errno);
      ret = -errno;
      goto out;
    }

  type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  if (ioctl(ctx.fd, VIDIOC_STREAMON, &type) < 0)
    {
      printf("jpeg_test: STREAMON(capture) failed: %d\n", errno);
      ret = -errno;
      goto out;
    }

  for (frame = 0; frame < nframes; frame++)
    {
      struct v4l2_buffer buf;
      const uint8_t *jpeg;
      uint8_t *keep;
      uint32_t dequeued;
      uint64_t start;
      uint64_t waited;
      int slot = frame % ctx.nout;

      if (pixfmt == V4L2_PIX_FMT_UYVY)
        {
          jpeg_test_fill_uyvy(ctx.out[slot].addr, width, height);
        }
      else
        {
          jpeg_test_fill_i420(ctx.out[slot].addr, width, height);
        }

      memset(&buf, 0, sizeof(buf));
      buf.type      = V4L2_BUF_TYPE_VIDEO_OUTPUT;
      buf.memory    = V4L2_MEMORY_MMAP;
      buf.index     = slot;
      buf.m.offset  = ctx.out[slot].offset;
      buf.bytesused = rawsize;

      start = jpeg_test_now_ms();

      if (ioctl(ctx.fd, VIDIOC_QBUF, &buf) < 0)
        {
          printf("jpeg_test: QBUF(output) failed: %d\n", errno);
          ret = -errno;
          goto stop;
        }

      /* Wait for the encoded frame.  DQBUF here is non-blocking in this
       * framework, so poll rather than assume it sleeps.
       */

      for (waited = 0; waited < (int)timeout_ms;
           waited += JPEG_TEST_POLL_MS)
        {
          memset(&buf, 0, sizeof(buf));
          buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
          buf.memory = V4L2_MEMORY_MMAP;

          if (ioctl(ctx.fd, VIDIOC_DQBUF, &buf) == 0)
            {
              break;
            }

          if (errno != EAGAIN)
            {
              printf("jpeg_test: DQBUF(capture) failed: %d\n", errno);
              ret = -errno;
              goto stop;
            }

          usleep(JPEG_TEST_POLL_MS * 1000);
        }

      if (waited >= (int)timeout_ms)
        {
          ret = -ETIMEDOUT;
          goto stop;
        }

      if (frame < JPEG_TEST_MAX_RESULTS)
        {
          recorded = frame + 1;
          results[frame].bytes = buf.bytesused;
          results[frame].ms    = (uint32_t)(jpeg_test_now_ms() - start);
          results[frame].driver_error =
            (buf.flags & V4L2_BUF_FLAG_ERROR) != 0;
        }

      if (buf.flags & V4L2_BUF_FLAG_ERROR)
        {
          ret = -EIO;
          goto stop;
        }

      /* Keep the last frame so 'dump' can emit it after this command has
       * released the device.
       */

      if (buf.bytesused > 0)
        {
          keep = realloc(g_last_jpeg, buf.bytesused);

          if (keep != NULL)
            {
              g_last_jpeg   = keep;
              memcpy(g_last_jpeg, ctx.cap[buf.index].addr, buf.bytesused);
              g_last_len    = buf.bytesused;
              g_last_width  = width;
              g_last_height = height;
            }

          /* A JPEG must start with SOI and end with EOI.  Recorded, not
           * printed, so the console stays quiet until the run is over.
           */

          jpeg = ctx.cap[buf.index].addr;

          if (frame < JPEG_TEST_MAX_RESULTS)
            {
              results[frame].soi_ok = jpeg[0] == 0xff && jpeg[1] == 0xd8;
              results[frame].eoi_ok =
                jpeg[buf.bytesused - 2] == 0xff &&
                jpeg[buf.bytesused - 1] == 0xd9;
            }
        }

      /* Requeue the buffer that was actually dequeued.
       *
       * Not "frame % ncap": the driver picks which buffer to fill, and
       * assuming a rotation instead of using the index DQBUF reported queued
       * one buffer twice and lost the other, after which the driver had
       * nowhere to write and stopped producing frames entirely.
       */

      dequeued = buf.index;

      memset(&buf, 0, sizeof(buf));
      buf.type     = V4L2_BUF_TYPE_VIDEO_CAPTURE;
      buf.memory   = V4L2_MEMORY_MMAP;
      buf.index    = dequeued;
      buf.m.offset = ctx.cap[dequeued].offset;

      if (ioctl(ctx.fd, VIDIOC_QBUF, &buf) < 0)
        {
          printf("jpeg_test: QBUF(capture %lu) failed: %d\n",
                 (unsigned long)dequeued, errno);
          ret = -errno;
          goto stop;
        }

      /* Reclaim the consumed input buffers.
       *
       * Bounded on purpose: an unbounded "while (DQBUF == 0);" spins at
       * application priority with no yield, which starved the CP heartbeat
       * for six seconds and got the board reset.  There can never be more
       * than the queue depth outstanding, so that is the limit.
       */

      for (i = 0; i < ctx.nout; i++)
        {
          memset(&buf, 0, sizeof(buf));
          buf.type   = V4L2_BUF_TYPE_VIDEO_OUTPUT;
          buf.memory = V4L2_MEMORY_MMAP;

          if (ioctl(ctx.fd, VIDIOC_DQBUF, &buf) < 0)
            {
              break;
            }
        }
    }

stop:
  type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
  ioctl(ctx.fd, VIDIOC_STREAMOFF, &type);
  type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  ioctl(ctx.fd, VIDIOC_STREAMOFF, &type);

  /* Safe to talk now: streaming has stopped, so competing for the mailbox
   * console channel can no longer cost a heartbeat.
   */

  for (i = 0; i < recorded; i++)
    {
      printf("jpeg_test: frame %d -> %lu bytes in %lu ms, ratio %lu:1  %s\n",
             i, (unsigned long)results[i].bytes,
             (unsigned long)results[i].ms,
             (unsigned long)(results[i].bytes ?
                             rawsize / results[i].bytes : 0),
             results[i].driver_error ? "DRIVER ERROR" :
             (results[i].soi_ok && results[i].eoi_ok) ? "valid JPEG" :
             !results[i].soi_ok ? "NO SOI" : "NO EOI");
    }

  if (ret == -ETIMEDOUT)
    {
      printf("jpeg_test: timed out after %d frame(s), budget was %lu ms\n",
             recorded, (unsigned long)timeout_ms);
    }

out:
  for (i = 0; i < ctx.nout; i++)
    {
      if (ctx.out[i].addr != NULL && ctx.out[i].addr != MAP_FAILED)
        {
          munmap(ctx.out[i].addr, ctx.out[i].length);
        }
    }

  for (i = 0; i < ctx.ncap; i++)
    {
      if (ctx.cap[i].addr != NULL && ctx.cap[i].addr != MAP_FAILED)
        {
          munmap(ctx.cap[i].addr, ctx.cap[i].length);
        }
    }

  close(ctx.fd);

  if (ret == 0 && g_last_len > 0)
    {
      printf("jpeg_test: run 'jpeg_test dump' to get the JPEG off "
             "the board\n");
    }

  return ret;
}

/****************************************************************************
 * Name: jpeg_test_dump
 *
 * Description:
 *   Print the last encoded frame as base64.
 *
 *   The board has no writable filesystem and the AP console is a mailbox
 *   command path, so text over the console is the only way out.  base64
 *   survives that path; raw bytes would not.
 *
 ****************************************************************************/

static int jpeg_test_dump(void)
{
  size_t i;

  if (g_last_jpeg == NULL || g_last_len == 0)
    {
      printf("jpeg_test: nothing encoded yet, run 'jpeg_test enc' first\n");
      return -ENODATA;
    }

  printf("jpeg_test: ---BEGIN JPEG BASE64--- %lux%lu %zu bytes\n",
         (unsigned long)g_last_width, (unsigned long)g_last_height,
         g_last_len);

  for (i = 0; i < g_last_len; i += JPEG_TEST_B64_CHUNK)
    {
      size_t n = g_last_len - i;
      size_t j;
      char line[80];
      int p = 0;

      if (n > JPEG_TEST_B64_CHUNK)
        {
          n = JPEG_TEST_B64_CHUNK;
        }

      for (j = 0; j < n; j += 3)
        {
          uint32_t v = (uint32_t)g_last_jpeg[i + j] << 16;
          size_t rem = n - j;

          if (rem > 1)
            {
              v |= (uint32_t)g_last_jpeg[i + j + 1] << 8;
            }

          if (rem > 2)
            {
              v |= g_last_jpeg[i + j + 2];
            }

          line[p++] = g_b64[(v >> 18) & 0x3f];
          line[p++] = g_b64[(v >> 12) & 0x3f];
          line[p++] = rem > 1 ? g_b64[(v >> 6) & 0x3f] : '=';
          line[p++] = rem > 2 ? g_b64[v & 0x3f] : '=';
        }

      line[p] = '\0';
      printf("%s\n", line);
    }

  printf("jpeg_test: ---END JPEG BASE64---\n");
  return 0;
}

/****************************************************************************
 * Name: jpeg_test_info
 ****************************************************************************/

static int jpeg_test_info(void)
{
  struct v4l2_capability cap;
  struct v4l2_fmtdesc fmt;
  int fd;
  int i;

  fd = open(CONFIG_JPEG_TEST_DEV_PATH, O_RDWR);
  if (fd < 0)
    {
      printf("jpeg_test: cannot open %s: %d\n",
             CONFIG_JPEG_TEST_DEV_PATH, errno);
      return -errno;
    }

  memset(&cap, 0, sizeof(cap));
  if (ioctl(fd, VIDIOC_QUERYCAP, &cap) == 0)
    {
      printf("jpeg_test: driver=%s card=%s caps=0x%08lx\n",
             cap.driver, cap.card, (unsigned long)cap.capabilities);
    }

  for (i = 0; i < 8; i++)
    {
      memset(&fmt, 0, sizeof(fmt));
      fmt.index = i;
      fmt.type  = V4L2_BUF_TYPE_VIDEO_OUTPUT;

      if (ioctl(fd, VIDIOC_ENUM_FMT, &fmt) < 0)
        {
          break;
        }

      printf("  input  %d: %c%c%c%c  %s\n", i,
             (int)(fmt.pixelformat & 0xff),
             (int)((fmt.pixelformat >> 8) & 0xff),
             (int)((fmt.pixelformat >> 16) & 0xff),
             (int)((fmt.pixelformat >> 24) & 0xff),
             fmt.description);
    }

  for (i = 0; i < 8; i++)
    {
      memset(&fmt, 0, sizeof(fmt));
      fmt.index = i;
      fmt.type  = V4L2_BUF_TYPE_VIDEO_CAPTURE;

      if (ioctl(fd, VIDIOC_ENUM_FMT, &fmt) < 0)
        {
          break;
        }

      printf("  output %d: %c%c%c%c  %s\n", i,
             (int)(fmt.pixelformat & 0xff),
             (int)((fmt.pixelformat >> 8) & 0xff),
             (int)((fmt.pixelformat >> 16) & 0xff),
             (int)((fmt.pixelformat >> 24) & 0xff),
             fmt.description);
    }

  close(fd);
  return 0;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(int argc, char *argv[])
{
  uint32_t width = JPEG_TEST_DEF_WIDTH;
  uint32_t height = JPEG_TEST_DEF_HEIGHT;
  uint32_t pixfmt = V4L2_PIX_FMT_YUV420;
  int quality = JPEG_TEST_DEF_QUALITY;
  int nframes = JPEG_TEST_DEF_FRAMES;
  int i;

  if (argc < 2)
    {
      jpeg_test_usage();
      return EXIT_FAILURE;
    }

  for (i = 2; i + 1 < argc; i += 2)
    {
      unsigned long value = strtoul(argv[i + 1], NULL, 0);

      if (strcmp(argv[i], "-w") == 0)
        {
          width = (uint32_t)value;
        }
      else if (strcmp(argv[i], "-h") == 0)
        {
          height = (uint32_t)value;
        }
      else if (strcmp(argv[i], "-q") == 0)
        {
          quality = (int)value;
        }
      else if (strcmp(argv[i], "-n") == 0)
        {
          nframes = (int)value;
        }
      else if (strcmp(argv[i], "-f") == 0)
        {
          if (strcmp(argv[i + 1], "uyvy") == 0)
            {
              pixfmt = V4L2_PIX_FMT_UYVY;
            }
          else if (strcmp(argv[i + 1], "i420") == 0)
            {
              pixfmt = V4L2_PIX_FMT_YUV420;
            }
          else
            {
              printf("jpeg_test: unknown format %s\n", argv[i + 1]);
              return EXIT_FAILURE;
            }
        }
      else
        {
          jpeg_test_usage();
          return EXIT_FAILURE;
        }
    }

  if (strcmp(argv[1], "enc") == 0)
    {
      return jpeg_test_encode(width, height, quality, pixfmt, nframes) < 0 ?
             EXIT_FAILURE : EXIT_SUCCESS;
    }
  else if (strcmp(argv[1], "dump") == 0)
    {
      return jpeg_test_dump() < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
    }
  else if (strcmp(argv[1], "info") == 0)
    {
      return jpeg_test_info() < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
    }

  jpeg_test_usage();
  return EXIT_FAILURE;
}
