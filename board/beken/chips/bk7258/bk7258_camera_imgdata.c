/****************************************************************************
 * board/beken/chips/bk7258/bk7258_camera_imgdata.c
 *
 * BK7258 GC2145 camera platform (imgdata) driver: standard NuttX V4L2
 * imgdata_ops_s implementation for the YUV_BUF + generic-DMA capture
 * path.  This is the "platform data interface" half of the driver split
 * described in docs/zh-cn/device_dev_guide/media/camera/Camera_Driver.md
 * -- it owns the frame buffer, the ping-pong-bank-to-DMA-copy plumbing,
 * and full-frame assembly across the ~60 line-batch-done interrupts
 * needed to fill one 640x480 YUYV frame.  Sensor-specific I2C register
 * programming lives in the imgsensor half
 * (board/beken/boards/bk7258/bk7258-ap/src/bk7258_camera_imgsensor.c).
 *
 * Full-frame assembly algorithm:
 *   YUV_BUF's hardware line buffer in PSRAM is two adjacent
 *   line_batch_bytes-sized ping-pong regions (SM0 at offset 0, SM1 at
 *   offset +line_batch_bytes from bk7258_yuv_buf_get_line_buf_addr()),
 *   each holding 8 lines of YUYV data.  The hardware alternates writing
 *   into SM0/SM1 on every successive batch of 8 lines and fires
 *   SM0_WR/SM1_WR accordingly (bk7258_yuv_buf.h's ping-pong semantics
 *   comment, itself sourced from bk_avdk_smp's yuv_sm0_line_done()/
 *   yuv_sm1_line_done()).  This driver tracks a running frame_offset:
 *   each bank-done callback issues one DMA copy from that bank's fixed
 *   PSRAM address to (frame_buffer + frame_offset), then advances
 *   frame_offset by line_batch_bytes.  After 60 batches (640x480 YUYV:
 *   614400 total bytes / 10240 bytes per batch = 60), frame_offset
 *   reaches the frame size and this driver invokes the
 *   imgdata_capture_t callback to report one complete frame, then resets
 *   frame_offset to 0 for the next frame.
 *
 * Concurrency note: unlike the legacy smoke-test entry point
 * (bk7258_gc2145.c), this driver must keep up with the hardware's
 * continuous ping-pong alternation across an entire frame, not just a
 * single batch.  Each DMA copy (10240 bytes) must complete well within
 * the time it takes the DVP/YUV_BUF hardware to fill the *other* bank
 * with the next 8 lines, or that bank's data will be silently
 * overwritten before this driver drains it -- this driver does not wait
 * for DMA completion before returning from the bank-done callback (it
 * relies on the DMA channel completing asynchronously well before the
 * next bank-done interrupt for the *same* bank arrives, mirroring
 * bk_dvp.c's non-blocking bk_dma_start() call pattern); if that
 * assumption does not hold on real hardware, frames will show
 * corruption in the affected 8-line bands, which would need to be
 * diagnosed via the "busy" bookkeeping already present in
 * bk7258_dma_is_busy() during hardware validation.
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#include <nuttx/config.h>
#include <nuttx/arch.h>

#include <sys/time.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

#include <nuttx/video/imgdata.h>

#include "bk7258_yuv_buf.h"
#include "bk7258_dma.h"
#include "bk7258_camera_imgdata.h"

/* This driver only supports the one resolution/format GC2145's ported
 * register tables produce (see bk7258_camera_imgsensor.c); validated
 * here rather than trusting the caller, since V4L2_core calls
 * validate_frame_setting() with whatever the application requested. */
#define BK7258_CAMERA_WIDTH   640u
#define BK7258_CAMERA_HEIGHT  480u
#define BK7258_CAMERA_FRAME_BYTES \
  (BK7258_CAMERA_WIDTH * BK7258_CAMERA_HEIGHT * 2u) /* YUYV, 2 bytes/px */

struct bk7258_camera_imgdata_s
{
  struct imgdata_s data;         /* Must be first: imgdata_ops_s casts
                                   * FAR struct imgdata_s * back to this
                                   * type via the same base-pointer
                                   * convention used throughout the
                                   * imgdata/imgsensor framework (see
                                   * isx012_dev_t in isx012.c for the
                                   * upstream precedent). */
  uint8_t *frame_buf;             /* Caller-supplied buffer, set by
                                   * set_buf(); NULL until then. */
  uint32_t frame_buf_size;
  uint32_t frame_offset;          /* Bytes assembled into frame_buf so
                                   * far, for the frame currently being
                                   * captured. */
  bool capturing;
  imgdata_capture_t capture_cb;
  void *capture_cb_arg;
};

static int bk7258_camera_imgdata_init(FAR struct imgdata_s *data);
static int bk7258_camera_imgdata_uninit(FAR struct imgdata_s *data);
static int bk7258_camera_imgdata_set_buf(FAR struct imgdata_s *data,
                                          uint8_t nr_datafmts,
                                          FAR imgdata_format_t *datafmts,
                                          uint8_t *addr, uint32_t size);
static int bk7258_camera_imgdata_validate_frame_setting(
    FAR struct imgdata_s *data, uint8_t nr_datafmts,
    FAR imgdata_format_t *datafmts, FAR imgdata_interval_t *interval);
static int bk7258_camera_imgdata_start_capture(
    FAR struct imgdata_s *data, uint8_t nr_datafmts,
    FAR imgdata_format_t *datafmts, FAR imgdata_interval_t *interval,
    imgdata_capture_t callback, FAR void *arg);
static int bk7258_camera_imgdata_stop_capture(FAR struct imgdata_s *data);

static const struct imgdata_ops_s g_bk7258_camera_imgdata_ops =
{
  .init                   = bk7258_camera_imgdata_init,
  .uninit                 = bk7258_camera_imgdata_uninit,
  .set_buf                = bk7258_camera_imgdata_set_buf,
  .validate_frame_setting = bk7258_camera_imgdata_validate_frame_setting,
  .start_capture          = bk7258_camera_imgdata_start_capture,
  .stop_capture           = bk7258_camera_imgdata_stop_capture,
};

static struct bk7258_camera_imgdata_s g_bk7258_camera_imgdata =
{
  .data = { &g_bk7258_camera_imgdata_ops },
};

/****************************************************************************
 * Name: bk7258_camera_line_batch_done
 *
 * Description:
 *   YUV_BUF ping-pong bank-done callback (invoked from
 *   bk7258_yuv_buf.c's ISR context for every 8-line batch).  Issues one
 *   DMA copy of that bank's fixed PSRAM contents into the frame buffer
 *   at the current running offset, then advances the offset.  When the
 *   offset reaches a full frame's worth of bytes, invokes the
 *   imgdata_capture_t callback to report frame completion and resets
 *   for the next frame.
 *
 ****************************************************************************/

static void bk7258_camera_line_batch_done(bk7258_yuv_buf_bank_t bank,
                                           void *arg)
{
  FAR struct bk7258_camera_imgdata_s *priv = arg;
  uint32_t line_buf_addr = bk7258_yuv_buf_get_line_buf_addr();
  uint32_t batch_bytes = bk7258_yuv_buf_get_line_batch_bytes();
  uint32_t src_addr;
  struct timeval ts;

  if (!priv->capturing || priv->frame_buf == NULL)
    {
      return;
    }

  if (priv->frame_offset + batch_bytes > priv->frame_buf_size)
    {
      /* Should not happen once validate_frame_setting()/set_buf() have
       * agreed on BK7258_CAMERA_FRAME_BYTES, but guard against a
       * mismatched buffer size overrunning it -- drop this batch and
       * resynchronize at the next frame boundary rather than corrupting
       * memory past the buffer, mirroring bk_dvp.c's yuv_sm0_line_done()/
       * yuv_sm1_line_done() offset-overflow guard (which resets
       * yuv_data_offset to 0 and sets handle->error). */
      priv->frame_offset = 0;
      return;
    }

  /* SM0 reads from the ping-pong region at offset 0; SM1 reads from the
   * region at offset +batch_bytes -- matching yuv_sm0_line_done()'s
   * yuv_em_addr and yuv_sm1_line_done()'s "yuv_em_addr +
   * yuv_pingpong_length" source addresses. */
  src_addr = line_buf_addr;
  if (bank == BK7258_YUV_BUF_BANK_SM1)
    {
      src_addr += batch_bytes;
    }

  /* Not waiting for a previous DMA to finish before starting this one:
   * bk7258_dma_configure() unconditionally disables the channel first,
   * matching bk_dvp.c's bk_dma_stop() + reconfigure + bk_dma_start()
   * pattern in both line-done handlers -- see this file's header
   * comment for the timing assumption this depends on. */
  bk7258_dma_configure(src_addr,
                        (uint32_t)(uintptr_t)(priv->frame_buf +
                                               priv->frame_offset),
                        batch_bytes);
  bk7258_dma_start();

  priv->frame_offset += batch_bytes;

  if (priv->frame_offset >= priv->frame_buf_size)
    {
      priv->frame_offset = 0;

      if (priv->capture_cb != NULL)
        {
          gettimeofday(&ts, NULL);
          printf("bk7258_camera_imgdata: frame complete, %u bytes\n",
                 (unsigned int)priv->frame_buf_size);
          priv->capture_cb(OK, priv->frame_buf_size, &ts,
                            priv->capture_cb_arg);
        }
    }
}

static int bk7258_camera_imgdata_init(FAR struct imgdata_s *data)
{
  FAR struct bk7258_camera_imgdata_s *priv =
      (FAR struct bk7258_camera_imgdata_s *)data;

  printf("bk7258_camera_imgdata: init\n");
  bk7258_yuv_buf_init();
  bk7258_dma_init();
  bk7258_yuv_buf_set_line_callback(bk7258_camera_line_batch_done, priv);

  return OK;
}

static int bk7258_camera_imgdata_uninit(FAR struct imgdata_s *data)
{
  bk7258_yuv_buf_stop();
  bk7258_dma_stop();
  bk7258_yuv_buf_set_line_callback(NULL, NULL);

  return OK;
}

static int bk7258_camera_imgdata_set_buf(FAR struct imgdata_s *data,
                                          uint8_t nr_datafmts,
                                          FAR imgdata_format_t *datafmts,
                                          uint8_t *addr, uint32_t size)
{
  FAR struct bk7258_camera_imgdata_s *priv =
      (FAR struct bk7258_camera_imgdata_s *)data;

  if (size < BK7258_CAMERA_FRAME_BYTES)
    {
      return -EINVAL;
    }

  priv->frame_buf = addr;
  priv->frame_buf_size = BK7258_CAMERA_FRAME_BYTES;
  priv->frame_offset = 0;

  return OK;
}

static int bk7258_camera_imgdata_validate_frame_setting(
    FAR struct imgdata_s *data, uint8_t nr_datafmts,
    FAR imgdata_format_t *datafmts, FAR imgdata_interval_t *interval)
{
  if (nr_datafmts < 1)
    {
      return -EINVAL;
    }

  /* Only the single 640x480 YUYV configuration GC2145's ported register
   * tables produce is supported -- see design discussion in this
   * driver's development notes: adding other resolutions would require
   * porting additional sensor register tables that are not currently
   * available in this repo. */
  if (datafmts[IMGDATA_FMT_MAIN].width != BK7258_CAMERA_WIDTH ||
      datafmts[IMGDATA_FMT_MAIN].height != BK7258_CAMERA_HEIGHT ||
      datafmts[IMGDATA_FMT_MAIN].pixelformat != IMGDATA_PIX_FMT_YUYV)
    {
      return -EINVAL;
    }

  return OK;
}

static int bk7258_camera_imgdata_start_capture(
    FAR struct imgdata_s *data, uint8_t nr_datafmts,
    FAR imgdata_format_t *datafmts, FAR imgdata_interval_t *interval,
    imgdata_capture_t callback, FAR void *arg)
{
  FAR struct bk7258_camera_imgdata_s *priv =
      (FAR struct bk7258_camera_imgdata_s *)data;
  int ret;

  ret = bk7258_camera_imgdata_validate_frame_setting(data, nr_datafmts,
                                                      datafmts, interval);
  if (ret < 0)
    {
      return ret;
    }

  if (priv->frame_buf == NULL)
    {
      return -EINVAL;
    }

  priv->capture_cb = callback;
  priv->capture_cb_arg = arg;
  priv->frame_offset = 0;
  priv->capturing = true;

  printf("bk7258_camera_imgdata: start_capture (%ux%u)\n",
         BK7258_CAMERA_WIDTH, BK7258_CAMERA_HEIGHT);
  bk7258_yuv_buf_configure(BK7258_CAMERA_WIDTH, BK7258_CAMERA_HEIGHT);
  bk7258_yuv_buf_start();

  return OK;
}

static int bk7258_camera_imgdata_stop_capture(FAR struct imgdata_s *data)
{
  FAR struct bk7258_camera_imgdata_s *priv =
      (FAR struct bk7258_camera_imgdata_s *)data;

  priv->capturing = false;
  bk7258_yuv_buf_stop();
  bk7258_dma_stop();

  return OK;
}

FAR struct imgdata_s *bk7258_camera_imgdata_initialize(void)
{
  return &g_bk7258_camera_imgdata.data;
}
