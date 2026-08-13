/****************************************************************************
 * app/agent_camera/agent_camera_main.c
 *
 * Bench for the camera side of the ai_agent port.
 *
 * packages/ai_agent's camera_capture tool (src/tools/tool_camera.c) reaches
 * the camera through V4L2 and then hands the frame straight to a Vision LLM.
 * On this board the LLM half cannot run yet -- the AP core has no network
 * device (Wi-Fi lives on CP) -- so the tool as a whole cannot be exercised
 * on hardware.  Everything up to the LLM call can be, and that half is what
 * the board actually has to provide.
 *
 * This command runs exactly that half: the same ioctl order, the same
 * V4L2_MEMORY_MMAP buffers, the same JPEG-then-ENTROPY format fallback, the
 * same poll() timeout, the same "one frame, then stop" shape.  What it does
 * differently is only what happens afterwards: instead of posting the frame
 * to an LLM it checks the JPEG markers, prints the byte count and can write
 * the frame to a file.
 *
 * Two things it is meant to answer, in this order:
 *
 *   1. Does the JPEG capture path work at all through V4L2?  The path is
 *      new (bk7258_jpeg_enc.c plus the JPEG branch of
 *      bk7258_camera_imgdata.c) and the UYVY path is the only one with
 *      board evidence behind it, so "JPEG works" cannot be assumed from
 *      "preview works".
 *
 *   2. Does tool_camera.c's *request* fit this board?  It asks for
 *      320x180 (its "low", the default) or 1280x720 ("high").  This driver
 *      programs 480x480 / 640x480 / 864x480 and rejects anything else --
 *      bk7258_gc2145_find_mode() is an exact match, not a nearest one --
 *      so the tool's own geometry is expected to fail VIDIOC_S_FMT here.
 *      `agent_camera low` reproduces that failure deliberately; `auto`
 *      picks a size the driver enumerates and shows the path works once
 *      the geometry is right.  The fix belongs upstream in tool_camera.c
 *      (negotiate via VIDIOC_ENUM_FRAMESIZES instead of hardcoding), and
 *      the negotiation implemented here is the prototype for it.
 *
 * Usage:
 *   agent_camera [auto|low|high|<W>x<H>] [options]
 *
 *     auto      (default) first JPEG size the driver enumerates
 *     low       320x180  -- tool_camera.c's default request
 *     high      1280x720 -- tool_camera.c's "high" request
 *     <W>x<H>   an explicit geometry
 *
 *   Options:
 *     out=<path>   write the frame (e.g. out=/mnt/cap.jpg; needs a mounted
 *                  filesystem -- see docs/reference/camera.md 9.3)
 *     b64          print the first frame as base64 between fence lines, to
 *                  retrieve it with no filesystem involved:
 *
 *                    ./serial_cmd.sh -r -w 60 -o /tmp/b64.log 'agent_camera b64'
 *                    sed -n '/BEGIN AGENT_CAMERA/,/END AGENT_CAMERA/p' /tmp/b64.log \
 *                      | sed '1d;$d' | tr -d '\r\n' | base64 -d > cap.jpg
 *
 *                  Then check it with a real decoder, not just the markers:
 *                  `identify cap.jpg` / PIL.  See docs/reference/camera.md
 *                  14.5-14.6 for why the marker check is not enough.
 *     n=<count>    capture <count> frames in one streaming session
 *                  (default 1, which is what the tool does)
 *     caps         only enumerate formats and sizes, capture nothing
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#include <nuttx/config.h>

#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <nuttx/video/video.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define CAM_DEV            "/dev/video0"

/* Mirrors tool_camera.c: two MMAP buffers, a 160KB sizeimage hint and a 5s
 * frame timeout.  The sizeimage hint is what v4l2_cap.c's get_bufsize()
 * uses for a compressed format, so it -- not width*height -- decides how
 * much the imgdata allocator takes out of PSRAM for each buffer.
 */

#define CAM_BUF_SIZE       (160 * 1024)
#define CAM_NUM_BUFFERS    2
#define CAM_TIMEOUT_MS     5000

/* tool_camera.c's two hardcoded geometries. */

#define TOOL_LOW_WIDTH     320
#define TOOL_LOW_HEIGHT    180
#define TOOL_HIGH_WIDTH    1280
#define TOOL_HIGH_HEIGHT   720

/* tool_camera.c's fallback format, spelled the same way it spells it. */

#ifndef V4L2_PIX_FMT_ENTROPY
#  define V4L2_PIX_FMT_ENTROPY  v4l2_fourcc('G', 'R', 'E', 'P')
#endif

#define MAX_ENUM_SIZES     16

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct agent_camera_size_s
{
  uint16_t width;
  uint16_t height;
};

struct agent_camera_s
{
  int      fd;
  void    *bufs[CAM_NUM_BUFFERS];
  size_t   buflen[CAM_NUM_BUFFERS];
  uint32_t nbuffers;
  bool     streaming;
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void agent_camera_usage(void)
{
  printf("Usage: agent_camera [auto|low|high|<W>x<H>] "
         "[out=<path>] [n=<count>] [strict] [caps]\n"
         "  auto     first JPEG size the driver enumerates (default)\n"
         "  low      320x180, tool_camera.c's default request\n"
         "  high     1280x720, tool_camera.c's \"high\" request\n"
         "  strict   do not negotiate a refused geometry (pre-patch\n"
         "           tool_camera.c behaviour)\n"
         "  b64      print the first frame as base64 (retrieve without a\n"
         "           filesystem; see the file header)\n"
         "  caps     enumerate formats and sizes only\n");
}

/* Prints a fourcc the way the rest of the tree does, so log lines can be
 * grepped against the driver's own format tables.
 */

static void agent_camera_print_fourcc(uint32_t fourcc)
{
  printf("%c%c%c%c",
         (char)(fourcc & 0xff),
         (char)((fourcc >> 8) & 0xff),
         (char)((fourcc >> 16) & 0xff),
         (char)((fourcc >> 24) & 0xff));
}

/****************************************************************************
 * Name: agent_camera_enum
 *
 * Description:
 *   Enumerates the formats the device offers and, for each, its discrete
 *   frame sizes.  Collects the sizes belonging to want_fmt into sizes[].
 *
 *   This is the part tool_camera.c is missing: it hardcodes a geometry and
 *   gives up when VIDIOC_S_FMT rejects it, with no way to learn what the
 *   device would have accepted.  Everything the driver can do is already
 *   discoverable through these two ioctls.
 *
 * Returned Value:
 *   Number of sizes collected for want_fmt, or a negated errno.
 *
 ****************************************************************************/

static int agent_camera_enum(int fd, uint32_t want_fmt,
                            struct agent_camera_size_s *sizes,
                            size_t max_sizes, bool verbose)
{
  struct v4l2_fmtdesc fmtdesc;
  unsigned int nsizes = 0;
  unsigned int i;

  for (i = 0; ; i++)
    {
      memset(&fmtdesc, 0, sizeof(fmtdesc));
      fmtdesc.index = i;
      fmtdesc.type  = V4L2_BUF_TYPE_VIDEO_CAPTURE;

      if (ioctl(fd, VIDIOC_ENUM_FMT, (uintptr_t)&fmtdesc) < 0)
        {
          break;
        }

      if (verbose)
        {
          printf("agent_camera: format[%u] ", i);
          agent_camera_print_fourcc(fmtdesc.pixelformat);
          printf(" flags=0x%" PRIx32 " %s\n",
                 (uint32_t)fmtdesc.flags,
                 fmtdesc.description[0] ? (char *)fmtdesc.description : "");
        }

      /* Sizes are enumerated per format, so this inner loop has to run for
       * every format even when only one of them is wanted: the framesize
       * enumeration is indexed by (pixel_format, index) and the driver
       * returns EINVAL for an index that belongs to another format.
       */

      unsigned int j;

      for (j = 0; ; j++)
        {
          struct v4l2_frmsizeenum frmsize;

          memset(&frmsize, 0, sizeof(frmsize));
          frmsize.index        = j;
          frmsize.pixel_format = fmtdesc.pixelformat;
          frmsize.buf_type     = V4L2_BUF_TYPE_VIDEO_CAPTURE;

          if (ioctl(fd, VIDIOC_ENUM_FRAMESIZES, (uintptr_t)&frmsize) < 0)
            {
              break;
            }

          if (frmsize.type != V4L2_FRMSIZE_TYPE_DISCRETE)
            {
              continue;
            }

          if (verbose)
            {
              printf("agent_camera:   size[%u] %" PRIu32 "x%" PRIu32 "\n",
                     j,
                     (uint32_t)frmsize.discrete.width,
                     (uint32_t)frmsize.discrete.height);
            }

          if (fmtdesc.pixelformat == want_fmt && nsizes < max_sizes)
            {
              sizes[nsizes].width  = (uint16_t)frmsize.discrete.width;
              sizes[nsizes].height = (uint16_t)frmsize.discrete.height;
              nsizes++;
            }
        }
    }

  if (i == 0)
    {
      printf("agent_camera: VIDIOC_ENUM_FMT rejected index 0: %d\n", errno);
      return -errno;
    }

  return (int)nsizes;
}

/****************************************************************************
 * Name: agent_camera_try_format
 *
 * Description:
 *   Offers one (geometry, pixel format) pair to the driver.  Sets sizeimage
 *   the way tool_camera.c does, since for a compressed format that is what
 *   decides how large a buffer the framework allocates.
 *
 ****************************************************************************/

static int agent_camera_try_format(int fd, int width, int height,
                                  uint32_t pixelformat)
{
  struct v4l2_format fmt;

  memset(&fmt, 0, sizeof(fmt));
  fmt.type                = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  fmt.fmt.pix.width       = width;
  fmt.fmt.pix.height      = height;
  fmt.fmt.pix.pixelformat = pixelformat;
  fmt.fmt.pix.sizeimage   = CAM_BUF_SIZE;
  fmt.fmt.pix.field       = V4L2_FIELD_NONE;

  return ioctl(fd, VIDIOC_S_FMT, (uintptr_t)&fmt) < 0 ? ERROR : OK;
}

/****************************************************************************
 * Name: agent_camera_set_format
 *
 * Description:
 *   tool_camera.c's format step: ask for V4L2_PIX_FMT_JPEG and, if that is
 *   refused, ask for V4L2_PIX_FMT_ENTROPY.
 *
 *   When negotiate is true this also mirrors the geometry negotiation added
 *   to tool_camera.c by
 *   board/beken/boards/bk7258/bk7258-ap/ai_agent/0001-tool_camera-negotiate-frame-size.patch:
 *   a refused geometry is retried at the enumerated size whose pixel count
 *   is closest.  Same algorithm, same device, same driver, so `low` versus
 *   `low strict` is a direct before/after of that patch on this board.
 *
 *   Note that VIDIOC_ENUM_FRAMESIZES cannot be trusted to filter by pixel
 *   format: v4l2_cap.c's capture_enum_frmsize() indexes the imgsensor's
 *   frmsizes array and ignores f->pixel_format entirely, so every format
 *   enumerates every size.  VIDIOC_S_FMT is therefore the only authority on
 *   whether a (format, geometry) pair is real, which is why each candidate
 *   is put back through it rather than trusted because it was enumerated.
 *
 ****************************************************************************/

static int agent_camera_set_format(int fd, int *width, int *height,
                                  bool negotiate, uint32_t *out_fmt)
{
  static const uint32_t candidates[] =
  {
    V4L2_PIX_FMT_JPEG,
    V4L2_PIX_FMT_ENTROPY,
  };

  struct agent_camera_size_s sizes[MAX_ENUM_SIZES];
  size_t c;

  for (c = 0; c < sizeof(candidates) / sizeof(candidates[0]); c++)
    {
      if (agent_camera_try_format(fd, *width, *height, candidates[c]) == OK)
        {
          *out_fmt = candidates[c];
          return OK;
        }

      printf("agent_camera: S_FMT ");
      agent_camera_print_fourcc(candidates[c]);
      printf(" %dx%d failed: %d\n", *width, *height, errno);
    }

  if (!negotiate)
    {
      return -EINVAL;
    }

  for (c = 0; c < sizeof(candidates) / sizeof(candidates[0]); c++)
    {
      unsigned long want = (unsigned long)(*width) *
                           (unsigned long)(*height);
      unsigned long best_delta = ULONG_MAX;
      int best_w = 0;
      int best_h = 0;
      int nsizes;
      int i;

      nsizes = agent_camera_enum(fd, candidates[c], sizes,
                                 MAX_ENUM_SIZES, false);
      if (nsizes <= 0)
        {
          continue;
        }

      for (i = 0; i < nsizes; i++)
        {
          unsigned long area = (unsigned long)sizes[i].width *
                               (unsigned long)sizes[i].height;
          unsigned long delta = area > want ? area - want : want - area;

          if (delta < best_delta)
            {
              best_delta = delta;
              best_w = sizes[i].width;
              best_h = sizes[i].height;
            }
        }

      if (best_w == 0 || (best_w == *width && best_h == *height))
        {
          continue;
        }

      if (agent_camera_try_format(fd, best_w, best_h, candidates[c]) == OK)
        {
          printf("agent_camera: %dx%d refused, using enumerated %dx%d\n",
                 *width, *height, best_w, best_h);
          *width   = best_w;
          *height  = best_h;
          *out_fmt = candidates[c];
          return OK;
        }
    }

  return -EINVAL;
}

/****************************************************************************
 * Name: agent_camera_check_jpeg
 *
 * Description:
 *   Reports whether the frame is a structurally valid JPEG file.
 *
 *   This used to check only SOI at the front and EOI at the end, and that
 *   judgement was wrong in a way worth spelling out: a frame can pass it and
 *   still be undecodable.  The encoder on this board emits
 *   SOI / APP0 / SOF0 / DQT / DHT and then entropy data with **no SOS
 *   segment**, so libjpeg rejects the file outright ("Invalid JPEG file
 *   structure: missing SOS marker") while SOI and EOI are both present.  A
 *   check both the driver and this tool agreed on was simply not sufficient
 *   -- see docs/reference/camera.md 14.6.
 *
 *   So the segment chain is walked properly here.  Note that markers on this
 *   encoder are preceded by 0xFF fill bytes (three of them, e.g.
 *   ff ff ff ff db); the standard permits that, and a scanner that does not
 *   skip fill will desynchronise and report segments that do not exist.
 *
 ****************************************************************************/

static void agent_camera_check_jpeg(const uint8_t *data, size_t len)
{
  bool have_sof = false;
  bool have_sos = false;
  bool have_dqt = false;
  bool have_dht = false;
  size_t i = 0;

  if (len < 4)
    {
      printf("agent_camera: frame too short to inspect (%zu bytes)\n", len);
      return;
    }

  if (data[0] != 0xff || data[1] != 0xd8)
    {
      printf("agent_camera: no SOI: starts %02x %02x\n", data[0], data[1]);
      return;
    }

  /* Walk the segments.  Stops at SOS, which is where entropy data begins. */

  i = 2;
  while (i + 1 < len)
    {
      uint8_t marker;
      size_t seglen;

      if (data[i] != 0xff)
        {
          break;
        }

      /* Skip fill bytes: any number of 0xFF may precede a marker. */

      while (i + 1 < len && data[i + 1] == 0xff)
        {
          i++;
        }

      if (i + 1 >= len)
        {
          break;
        }

      marker = data[i + 1];

      if (marker == 0xd9)
        {
          break;
        }

      if (marker == 0xda)
        {
          have_sos = true;
          break;
        }

      if (i + 3 >= len)
        {
          break;
        }

      seglen = ((size_t)data[i + 2] << 8) | data[i + 3];
      if (seglen < 2)
        {
          break;
        }

      switch (marker)
        {
          case 0xc0:
          case 0xc1:
            have_sof = true;
            break;

          case 0xdb:
            have_dqt = true;
            break;

          case 0xc4:
            have_dht = true;
            break;

          default:
            break;
        }

      i += 2 + seglen;
    }

  printf("agent_camera: structure SOF=%s DQT=%s DHT=%s SOS=%s EOI=%s\n",
         have_sof ? "yes" : "NO", have_dqt ? "yes" : "NO",
         have_dht ? "yes" : "NO", have_sos ? "yes" : "NO",
         (data[len - 2] == 0xff && data[len - 1] == 0xd9) ? "yes" : "NO");

  if (!have_sos)
    {
      printf("agent_camera: NOT a decodable JPEG -- no SOS segment, so the "
             "entropy data has no scan header.  A Vision LLM (or any "
             "standard decoder) will reject this.  See "
             "docs/reference/camera.md 14.6\n");
    }
}

/****************************************************************************
 * Name: agent_camera_print_b64
 *
 * Description:
 *   Prints the frame as base64 between two fence lines, so it can be pulled
 *   off the board without a filesystem.
 *
 *   The alternative is `hexdump` plus docs/tools/hexdump2raw.py, which works
 *   (and is what first got a frame onto the host) but needs a mounted
 *   filesystem, costs 4.4 bytes of console per payload byte -- a 35KB frame
 *   became 170KB of serial output -- and has to be parsed with tolerance for
 *   block headers, ANSI escapes and the ASCII column.  base64 is 1.37 bytes
 *   per byte and `base64 -d` is exact, so there is nothing to get wrong on
 *   the host side.
 *
 *   Written straight to stdout in 57-byte groups (76 base64 chars per line)
 *   with no intermediate buffer, because a second copy of a 160KB frame does
 *   not fit in the AP's kernel heap.
 *
 ****************************************************************************/

/****************************************************************************
 * Name: agent_camera_fnv1a
 *
 * Description:
 *   32-bit FNV-1a over the frame, printed next to the base64 fence so the
 *   host can prove the retrieval was lossless before drawing any conclusion
 *   about the picture.
 *
 *   This exists because two frames came back decoding only 8 rows while a
 *   third decoded fully, and "the board produced junk" and "the console
 *   dropped bytes" predict exactly the same thing on the host.  Length
 *   agreement does not separate them: a dropped chunk replaced by an
 *   interleaved driver log line keeps the byte count plausible.  FNV-1a is
 *   enough here (we are detecting loss, not defending against tampering)
 *   and costs one pass with no buffer.
 *
 ****************************************************************************/

static uint32_t agent_camera_fnv1a(const uint8_t *data, size_t len)
{
  uint32_t hash = 2166136261u;
  size_t i;

  for (i = 0; i < len; i++)
    {
      hash ^= (uint32_t)data[i];
      hash *= 16777619u;
    }

  return hash;
}

static void agent_camera_print_b64(const uint8_t *data, size_t len)
{
  static const char tbl[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  size_t i;

  printf("agent_camera: payload len=%zu fnv1a=0x%08" PRIx32 "\n",
         len, agent_camera_fnv1a(data, len));

  printf("-----BEGIN AGENT_CAMERA JPEG %zu-----\n", len);

  for (i = 0; i < len; i += 3)
    {
      size_t remain = len - i;
      uint32_t v = (uint32_t)data[i] << 16;
      char quad[4];

      if (remain > 1)
        {
          v |= (uint32_t)data[i + 1] << 8;
        }

      if (remain > 2)
        {
          v |= data[i + 2];
        }

      quad[0] = tbl[(v >> 18) & 0x3f];
      quad[1] = tbl[(v >> 12) & 0x3f];
      quad[2] = remain > 1 ? tbl[(v >> 6) & 0x3f] : '=';
      quad[3] = remain > 2 ? tbl[v & 0x3f] : '=';

      fwrite(quad, 1, 4, stdout);

      if ((i / 3) % 19 == 18)
        {
          fputc('\n', stdout);
        }
    }

  fputc('\n', stdout);
  printf("-----END AGENT_CAMERA JPEG-----\n");
}

/****************************************************************************
 * Name: agent_camera_write_file
 ****************************************************************************/

static int agent_camera_write_file(const char *path,
                                   const uint8_t *data, size_t len)
{
  ssize_t nwritten;
  int fd;

  fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0)
    {
      printf("agent_camera: open %s failed: %d\n", path, errno);
      return -errno;
    }

  nwritten = write(fd, data, len);
  close(fd);

  if (nwritten < 0 || (size_t)nwritten != len)
    {
      printf("agent_camera: write %s short (%zd of %zu): %d\n",
             path, nwritten, len, errno);
      return -EIO;
    }

  printf("agent_camera: wrote %zu bytes to %s\n", len, path);
  return OK;
}

/****************************************************************************
 * Name: agent_camera_teardown
 *
 * Description:
 *   Unwinds in tool_camera.c's order: STREAMOFF, munmap, REQBUFS(0),
 *   close.  REQBUFS(0) is what releases the PSRAM the imgdata allocator
 *   handed out; skipping it leaks a buffer heap that is larger than the
 *   whole AP kernel heap.
 *
 ****************************************************************************/

static void agent_camera_teardown(struct agent_camera_s *cam)
{
  enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  struct v4l2_requestbuffers rel;
  uint32_t i;

  if (cam->fd < 0)
    {
      return;
    }

  if (cam->streaming)
    {
      ioctl(cam->fd, VIDIOC_STREAMOFF, (uintptr_t)&type);
      cam->streaming = false;
    }

  for (i = 0; i < cam->nbuffers; i++)
    {
      if (cam->bufs[i] != NULL && cam->bufs[i] != MAP_FAILED)
        {
          munmap(cam->bufs[i], cam->buflen[i]);
          cam->bufs[i] = NULL;
        }
    }

  memset(&rel, 0, sizeof(rel));
  rel.count  = 0;
  rel.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  rel.memory = V4L2_MEMORY_MMAP;
  ioctl(cam->fd, VIDIOC_REQBUFS, (uintptr_t)&rel);

  close(cam->fd);
  cam->fd = -1;
  cam->nbuffers = 0;
}

/****************************************************************************
 * Name: agent_camera_capture
 *
 * Description:
 *   The capture half of tool_camera.c's camera_v4l2_capture(), with the
 *   frame count made a parameter so a single session can be watched for
 *   more than one frame.  The tool takes exactly one.
 *
 ****************************************************************************/

static int agent_camera_capture(struct agent_camera_s *cam,
                                int width, int height,
                                unsigned int count, bool negotiate,
                                const char *out_path, bool b64)
{
  enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  struct v4l2_requestbuffers req;
  struct v4l2_capability cap;
  uint32_t chosen_fmt = 0;
  unsigned int frame;
  uint32_t i;
  int ret;

  memset(&cap, 0, sizeof(cap));
  if (ioctl(cam->fd, VIDIOC_QUERYCAP, (uintptr_t)&cap) < 0)
    {
      printf("agent_camera: VIDIOC_QUERYCAP failed: %d\n", errno);
      return -errno;
    }

  if ((cap.capabilities & V4L2_CAP_VIDEO_CAPTURE) == 0)
    {
      printf("agent_camera: device reports no capture capability "
             "(caps=0x%08" PRIx32 ")\n", (uint32_t)cap.capabilities);
      return -ENODEV;
    }

  ret = agent_camera_set_format(cam->fd, &width, &height, negotiate,
                                &chosen_fmt);
  if (ret < 0)
    {
      return ret;
    }

  printf("agent_camera: format ");
  agent_camera_print_fourcc(chosen_fmt);
  printf(" %dx%d sizeimage=%d\n", width, height, CAM_BUF_SIZE);

  memset(&req, 0, sizeof(req));
  req.count  = CAM_NUM_BUFFERS;
  req.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  req.memory = V4L2_MEMORY_MMAP;

  if (ioctl(cam->fd, VIDIOC_REQBUFS, (uintptr_t)&req) < 0)
    {
      printf("agent_camera: VIDIOC_REQBUFS failed: %d\n", errno);
      return -errno;
    }

  cam->nbuffers = req.count < CAM_NUM_BUFFERS ? req.count : CAM_NUM_BUFFERS;

  for (i = 0; i < cam->nbuffers; i++)
    {
      struct v4l2_buffer buf;

      memset(&buf, 0, sizeof(buf));
      buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
      buf.memory = V4L2_MEMORY_MMAP;
      buf.index  = i;

      if (ioctl(cam->fd, VIDIOC_QUERYBUF, (uintptr_t)&buf) < 0)
        {
          printf("agent_camera: VIDIOC_QUERYBUF(%" PRIu32 ") failed: %d\n",
                 i, errno);
          return -errno;
        }

      cam->buflen[i] = buf.length;
      cam->bufs[i] = mmap(NULL, buf.length, PROT_READ | PROT_WRITE,
                          MAP_SHARED, cam->fd, buf.m.offset);
      if (cam->bufs[i] == MAP_FAILED)
        {
          cam->bufs[i] = NULL;
          printf("agent_camera: mmap(%" PRIu32 ", %zu) failed: %d\n",
                 i, cam->buflen[i], errno);
          return -errno;
        }

      printf("agent_camera: buffer[%" PRIu32 "] len=%zu at %p\n",
             i, cam->buflen[i], cam->bufs[i]);
    }

  for (i = 0; i < cam->nbuffers; i++)
    {
      struct v4l2_buffer buf;

      memset(&buf, 0, sizeof(buf));
      buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
      buf.memory = V4L2_MEMORY_MMAP;
      buf.index  = i;

      if (ioctl(cam->fd, VIDIOC_QBUF, (uintptr_t)&buf) < 0)
        {
          printf("agent_camera: VIDIOC_QBUF(%" PRIu32 ") failed: %d\n",
                 i, errno);
          return -errno;
        }
    }

  if (ioctl(cam->fd, VIDIOC_STREAMON, (uintptr_t)&type) < 0)
    {
      printf("agent_camera: VIDIOC_STREAMON failed: %d\n", errno);
      return -errno;
    }

  cam->streaming = true;

  for (frame = 0; frame < count; frame++)
    {
      struct pollfd pfd;
      struct v4l2_buffer dqbuf;
      int nready;

      pfd.fd     = cam->fd;
      pfd.events = POLLIN;

      nready = poll(&pfd, 1, CAM_TIMEOUT_MS);
      if (nready < 0)
        {
          printf("agent_camera: poll failed: %d\n", errno);
          return -errno;
        }

      if (nready == 0)
        {
          printf("agent_camera: no frame within %d ms -- the capture path "
                 "produced nothing\n", CAM_TIMEOUT_MS);
          return -ETIMEDOUT;
        }

      memset(&dqbuf, 0, sizeof(dqbuf));
      dqbuf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
      dqbuf.memory = V4L2_MEMORY_MMAP;

      if (ioctl(cam->fd, VIDIOC_DQBUF, (uintptr_t)&dqbuf) < 0)
        {
          printf("agent_camera: VIDIOC_DQBUF failed: %d\n", errno);
          return -errno;
        }

      printf("agent_camera: frame %u: index=%" PRIu32
             " bytesused=%" PRIu32 " flags=0x%08" PRIx32 "\n",
             frame, (uint32_t)dqbuf.index, (uint32_t)dqbuf.bytesused,
             (uint32_t)dqbuf.flags);

      /* The driver completes a buffer with V4L2_BUF_FLAG_ERROR when its
       * watchdog fires rather than leaving DQBUF blocked forever (see
       * invariant 8 in docs/reference/camera.md).  Such a buffer carries no
       * usable frame, so say so instead of validating its contents.
       */

      if ((dqbuf.flags & V4L2_BUF_FLAG_ERROR) != 0)
        {
          printf("agent_camera: buffer flagged ERROR -- capture watchdog "
                 "or encoder fault, contents not usable\n");
        }
      else if (dqbuf.bytesused == 0)
        {
          printf("agent_camera: empty frame\n");
        }
      else if (dqbuf.index < cam->nbuffers)
        {
          const uint8_t *data = cam->bufs[dqbuf.index];

          agent_camera_check_jpeg(data, dqbuf.bytesused);

          /* Deliver the *last* frame of the session, not the first.
           *
           * This used to take frame 0, which is the one frame guaranteed to
           * be affected by the startup transient: the encoder is enabled at
           * an arbitrary phase of the continuously-streaming sensor, so the
           * driver reports err=3 resets=3 on every session (camera.md
           * §14.4).  Those frames still pass the marker checks, so nothing
           * in the log says the *contents* are wrong -- and because every
           * image ever retrieved with 'b64' or 'out=' was frame 0, the
           * spliced picture that came out was mistaken for the pipeline's
           * normal output.
           *
           * With count == 1 (the default, and what ai_agent's
           * camera_capture does) this is still frame 0.  Ask for more
           * frames to step past the transient.
           */

          if (frame + 1 == count)
            {
              if (out_path != NULL)
                {
                  agent_camera_write_file(out_path, data, dqbuf.bytesused);
                }

              if (b64)
                {
                  agent_camera_print_b64(data, dqbuf.bytesused);
                }
            }
        }

      if (ioctl(cam->fd, VIDIOC_QBUF, (uintptr_t)&dqbuf) < 0)
        {
          printf("agent_camera: re-QBUF failed: %d\n", errno);
          return -errno;
        }
    }

  return OK;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(int argc, FAR char *argv[])
{
  struct agent_camera_size_s sizes[MAX_ENUM_SIZES];
  struct agent_camera_s cam;
  const char *out_path = NULL;
  unsigned int count = 1;
  bool caps_only = false;
  bool auto_size = true;
  bool negotiate = true;
  bool b64 = false;
  int width = 0;
  int height = 0;
  int nsizes;
  int ret;
  int i;

  memset(&cam, 0, sizeof(cam));
  cam.fd = -1;

  for (i = 1; i < argc; i++)
    {
      const char *arg = argv[i];

      if (strcmp(arg, "auto") == 0)
        {
          auto_size = true;
        }
      else if (strcmp(arg, "low") == 0)
        {
          width     = TOOL_LOW_WIDTH;
          height    = TOOL_LOW_HEIGHT;
          auto_size = false;
        }
      else if (strcmp(arg, "high") == 0)
        {
          width     = TOOL_HIGH_WIDTH;
          height    = TOOL_HIGH_HEIGHT;
          auto_size = false;
        }
      else if (strcmp(arg, "caps") == 0)
        {
          caps_only = true;
        }
      else if (strcmp(arg, "strict") == 0)
        {
          negotiate = false;
        }
      else if (strcmp(arg, "b64") == 0)
        {
          b64 = true;
        }
      else if (strncmp(arg, "out=", 4) == 0)
        {
          out_path = arg + 4;
        }
      else if (strncmp(arg, "n=", 2) == 0)
        {
          count = (unsigned int)strtoul(arg + 2, NULL, 10);
          if (count == 0)
            {
              count = 1;
            }
        }
      else if (strchr(arg, 'x') != NULL)
        {
          char *endp;

          width = (int)strtol(arg, &endp, 10);
          if (endp == NULL || *endp != 'x')
            {
              agent_camera_usage();
              return EXIT_FAILURE;
            }

          height    = (int)strtol(endp + 1, NULL, 10);
          auto_size = false;

          if (width <= 0 || height <= 0)
            {
              agent_camera_usage();
              return EXIT_FAILURE;
            }
        }
      else
        {
          agent_camera_usage();
          return EXIT_FAILURE;
        }
    }

  cam.fd = open(CAM_DEV, O_RDWR);
  if (cam.fd < 0)
    {
      printf("agent_camera: open %s failed: %d\n", CAM_DEV, errno);
      return EXIT_FAILURE;
    }

  nsizes = agent_camera_enum(cam.fd, V4L2_PIX_FMT_JPEG, sizes,
                             MAX_ENUM_SIZES, true);
  if (nsizes < 0)
    {
      close(cam.fd);
      return EXIT_FAILURE;
    }

  printf("agent_camera: driver enumerates %d JPEG size(s)\n", nsizes);

  if (caps_only)
    {
      close(cam.fd);
      return EXIT_SUCCESS;
    }

  if (auto_size)
    {
      if (nsizes == 0)
        {
          printf("agent_camera: no JPEG size enumerated -- is the JPEG "
                 "format registered by the imgsensor driver?\n");
          close(cam.fd);
          return EXIT_FAILURE;
        }

      width  = sizes[0].width;
      height = sizes[0].height;
      printf("agent_camera: auto-selected %dx%d\n", width, height);
    }
  else
    {
      bool supported = false;

      for (i = 0; i < nsizes; i++)
        {
          if (sizes[i].width == width && sizes[i].height == height)
            {
              supported = true;
              break;
            }
        }

      if (!supported)
        {
          /* Not fatal on purpose: the point of `low` and `high` is to show
           * what tool_camera.c's hardcoded request does on this board, and
           * that means letting the request reach VIDIOC_S_FMT.
           */

          printf("agent_camera: %dx%d is not enumerated by this driver; "
                 "requesting it anyway (%s)\n",
                 width, height,
                 negotiate ? "will negotiate if refused" :
                             "strict: no negotiation");
        }
    }

  ret = agent_camera_capture(&cam, width, height, count, negotiate,
                             out_path, b64);
  agent_camera_teardown(&cam);

  if (ret < 0)
    {
      printf("agent_camera: FAILED (%d)\n", ret);
      return EXIT_FAILURE;
    }

  printf("agent_camera: OK\n");
  return EXIT_SUCCESS;
}
