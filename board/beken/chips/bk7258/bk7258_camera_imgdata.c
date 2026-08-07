/****************************************************************************
 * board/beken/chips/bk7258/bk7258_camera_imgdata.c
 *
 * BK7258 GC2145 camera platform (imgdata) driver: NuttX V4L2
 * imgdata_ops_s implementation for the YUV_BUF + generic-DMA capture
 * path.  This is the platform data interface half of the driver split
 * described in the openvela Camera Driver Framework guide -- it owns
 * the frame buffer, the ping-pong-bank-to-DMA-copy plumbing, and
 * full-frame assembly across the line-batch-done interrupts needed to
 * fill one frame.  Sensor-specific I2C register programming lives in
 * the imgsensor half
 * (board/beken/boards/bk7258/bk7258-ap/src/bk7258_camera_imgsensor.c).
 *
 * Full-frame assembly:
 *   YUV_BUF's PSRAM line buffer is two adjacent ping-pong regions
 *   (bk7258_yuv_buf_get_line_buf_addr(), each
 *   bk7258_yuv_buf_get_line_batch_bytes() bytes), holding 8 lines of
 *   YUYV data each.  The hardware alternates writing into the two
 *   regions and fires SM0_WR/SM1_WR accordingly.  On each bank-done
 *   callback, this driver issues one DMA copy from that bank's fixed
 *   PSRAM address to (frame_buf + frame_offset), then advances
 *   frame_offset by one batch.  Once frame_offset reaches the frame
 *   size, the imgdata_capture_t callback reports one complete frame
 *   and frame_offset resets to 0.
 *
 * Each DMA copy must complete before the hardware's next occurrence of
 * that same bank arrives; this driver does not block waiting for DMA
 * completion in the bank-done callback (mirroring the reference
 * implementation's non-blocking DMA start pattern).
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#include <nuttx/config.h>
#include <nuttx/arch.h>

#include <sys/time.h>
#include <stdbool.h>
#include <stdint.h>
#include <errno.h>

#include <nuttx/video/imgdata.h>

#include "bk7258_yuv_buf.h"
#include "bk7258_dma.h"
#include "bk7258_camera_imgdata.h"
#include "bk7258_psram.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* This driver only supports the one resolution/format the GC2145
 * register tables in the imgsensor half produce.
 */

#define BK7258_CAMERA_WIDTH   640u
#define BK7258_CAMERA_HEIGHT  480u
#define BK7258_CAMERA_FRAME_BYTES \
  (BK7258_CAMERA_WIDTH * BK7258_CAMERA_HEIGHT * 2u) /* YUYV, 2 bytes/px */

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct bk7258_camera_imgdata_s
{
  struct imgdata_s data;          /* Must be first: base-pointer cast. */
  FAR uint8_t *frame_buf;          /* Set by set_buf(); NULL until then. */
  uint32_t frame_buf_size;
  uint32_t frame_offset;           /* Bytes assembled so far. */
  bool capturing;
  imgdata_capture_t capture_cb;
  FAR void *capture_cb_arg;
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

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
static void *bk7258_camera_imgdata_alloc(FAR struct imgdata_s *data,
                                          uint32_t align_size,
                                          uint32_t size);
static void bk7258_camera_imgdata_free(FAR struct imgdata_s *data,
                                        void *addr);

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const struct imgdata_ops_s g_bk7258_camera_imgdata_ops =
{
  .init                   = bk7258_camera_imgdata_init,
  .uninit                 = bk7258_camera_imgdata_uninit,
  .set_buf                = bk7258_camera_imgdata_set_buf,
  .validate_frame_setting = bk7258_camera_imgdata_validate_frame_setting,
  .start_capture          = bk7258_camera_imgdata_start_capture,
  .stop_capture           = bk7258_camera_imgdata_stop_capture,
  .alloc                  = bk7258_camera_imgdata_alloc,
  .free                   = bk7258_camera_imgdata_free,
};

static struct bk7258_camera_imgdata_s g_bk7258_camera_imgdata =
{
  .data = { &g_bk7258_camera_imgdata_ops },
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: bk7258_camera_line_batch_done
 *
 * Description:
 *   YUV_BUF ping-pong bank-done callback (ISR context, one call per
 *   8-line batch).  Issues one DMA copy of that bank's fixed PSRAM
 *   contents into the frame buffer at the current running offset, then
 *   advances the offset.  Reports frame completion once a full frame
 *   has been assembled.
 *
 ****************************************************************************/

static void bk7258_camera_line_batch_done(bk7258_yuv_buf_bank_t bank,
                                           FAR void *arg)
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
       * agreed on BK7258_CAMERA_FRAME_BYTES; drop this batch and
       * resynchronize at the next frame boundary rather than writing
       * past the buffer.
       */

      priv->frame_offset = 0;
      return;
    }

  src_addr = line_buf_addr;
  if (bank == BK7258_YUV_BUF_BANK_SM1)
    {
      src_addr += batch_bytes;
    }

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
          priv->capture_cb(OK, priv->frame_buf_size, &ts,
                            priv->capture_cb_arg);
        }
    }
}

static int bk7258_camera_imgdata_init(FAR struct imgdata_s *data)
{
  FAR struct bk7258_camera_imgdata_s *priv =
      (FAR struct bk7258_camera_imgdata_s *)data;

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

/* V4L2_REQBUFS_COUNT_MAX (3) capture buffers at one 640x480 YUYV frame
 * (614400 bytes) exceed this board's internal SRAM heap
 * (CONFIG_RAM_SIZE=344064 bytes), so the framework's default
 * kumm_memalign()-backed allocation cannot hold the buffer pool.  This
 * board has a separate 16MB PSRAM region not mapped into the kernel's
 * malloc arena; imgdata_ops_s.alloc/free (the framework's documented
 * mechanism for drivers whose buffers must come from a non-default
 * memory pool) routes allocation to bk7258_psram.c's
 * bk7258_media_pool_alloc()/_free() instead.
 */

static void *bk7258_camera_imgdata_alloc(FAR struct imgdata_s *data,
                                          uint32_t align_size,
                                          uint32_t size)
{
  return bk7258_media_pool_alloc(BK7258_PSRAM_POOL_DISPLAY,
                                  align_size, size);
}

static void bk7258_camera_imgdata_free(FAR struct imgdata_s *data,
                                        void *addr)
{
  bk7258_media_pool_free(BK7258_PSRAM_POOL_DISPLAY, addr);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

FAR struct imgdata_s *bk7258_camera_imgdata_initialize(void)
{
  return &g_bk7258_camera_imgdata.data;
}
