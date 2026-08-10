/****************************************************************************
 * board/beken/chips/bk7258/bk7258_camera_imgdata.c
 *
 * BK7258 GC2145 camera platform (imgdata) driver: NuttX V4L2
 * imgdata_ops_s implementation for the YUV_BUF whole-frame direct-capture
 * path.  This is the platform data interface half of the driver split
 * described in the openvela Camera Driver Framework guide; sensor-specific
 * I2C register programming lives in the imgsensor half
 * (board/beken/boards/bk7258/bk7258-ap/src/bk7258_camera_imgsensor.c).
 *
 * Capture model
 * -------------
 * YUV_BUF writes a complete YUV422 frame straight into the V4L2 frame
 * buffer (which imgdata_ops_s.alloc() takes from PSRAM) and raises the
 * YUV_ARV interrupt when the frame is done.  That is the reference
 * implementation's pure-YUV path: bk_avdk_smp
 * ap/components/bk_dvp/src/bk_dvp.c dvp_camera_yuv_mode() points
 * em_base_addr at the frame buffer, and dvp_camera_yuv_eof_handler()
 * (registered on YUV_BUF_YUV_ARV) hands the finished frame up and
 * re-points em_base_addr at the next one.  There is no line-batch
 * ping-pong buffer and no DMA copy in this path -- the previous revision
 * of this driver reconstructed frames from ~60 SM0_WR/SM1_WR line-done
 * interrupts per frame plus one DMA transfer each, which is what the
 * reference only does for the combined YUV+encode formats.
 *
 * Interrupt-context rules
 * -----------------------
 * v4l2_cap.c's complete_capture() runs in interrupt context and, from
 * there, calls back into IMGDATA_SET_BUF() (to re-arm the next buffer)
 * and IMGDATA_STOP_CAPTURE()/IMGSENSOR_STOP_CAPTURE() (when it runs out
 * of vacant containers).  So set_buf() and stop_capture(), not just the
 * frame callback, must be interrupt-safe: no printf(), no blocking, no
 * allocation.  Diagnostics in those functions are guarded by
 * up_interrupt_context().
 *
 * Capture watchdog
 * ----------------
 * VIDIOC_DQBUF blocks in nxsem_wait_uninterruptible() with no timeout
 * (v4l2_cap.c capture_dqbuf()), so if the hardware never completes a
 * frame the caller's dequeue thread never returns -- which is exactly how
 * `nxcamera stream` ended up unable to process its own `q` command
 * (nxcamera_stop() posts VIDEO_MSG_STOP and then blocks in pthread_join()
 * on that thread).  This driver therefore runs a watchdog while capturing
 * and reports an error frame if no YUV_ARV arrives within
 * BK7258_CAMERA_WATCHDOG_MS: complete_capture() completes the buffer with
 * V4L2_BUF_FLAG_ERROR and posts dqbuf_wait_flg, so DQBUF returns, the
 * application loop regains control and a stop request can be honoured.
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#include <nuttx/config.h>
#include <nuttx/arch.h>
#include <nuttx/clock.h>
#include <nuttx/irq.h>
#include <nuttx/spinlock.h>
#include <nuttx/wdog.h>

#include <sys/time.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <errno.h>

#include <nuttx/video/imgdata.h>

#include "bk7258_yuv_buf.h"
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
  (BK7258_CAMERA_WIDTH * BK7258_CAMERA_HEIGHT * 2u) /* UYVY, 2 bytes/px */

/* YUV_BUF writes frames over the PSRAM bus; a frame buffer outside PSRAM
 * would be silently dropped (or corrupt unrelated memory), so set_buf()
 * rejects one.  PSRAM is mapped at SOC_PSRAM_DATA_BASE, 16MB.
 */

#define BK7258_CAMERA_PSRAM_BASE  0x60000000u
#define BK7258_CAMERA_PSRAM_END   0x61000000u

/* Upper bound on how long VIDIOC_DQBUF may block before this driver
 * reports an error frame instead of letting the caller hang forever.
 * 500ms is ~15 frame periods at 30fps: long enough never to fire during
 * healthy streaming, short enough that a `q` keystroke feels responsive.
 */

#define BK7258_CAMERA_WATCHDOG_MS 500

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct bk7258_camera_imgdata_s
{
  struct imgdata_s data;          /* Must be first: base-pointer cast. */
  FAR uint8_t *frame_buf;          /* Set by set_buf(); NULL until then. */
  uint32_t frame_buf_size;
  volatile bool capturing;
  imgdata_capture_t capture_cb;
  FAR void *capture_cb_arg;
  struct wdog_s watchdog;
  volatile uint32_t frames_done;   /* Frames handed to the framework. */
  volatile uint32_t frames_at_arm; /* frames_done when watchdog last armed */
  volatile uint32_t timeouts;      /* Watchdog error reports. */
  volatile uint32_t set_buf_calls;
  volatile uint32_t rejected_bufs;
  clock_t start_ticks;             /* For the measured frame rate. */
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
static void bk7258_camera_watchdog_expiry(wdparm_t arg);

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
 * Name: bk7258_camera_now
 *
 * Description:
 *   Interrupt-safe timestamp for the capture callback: derived from the
 *   system tick counter rather than from clock_gettime()/gettimeofday(),
 *   neither of which is meant to be called from an interrupt handler.
 *
 ****************************************************************************/

static void bk7258_camera_now(FAR struct timeval *ts)
{
  clock_t ticks = clock_systime_ticks();

  ts->tv_sec  = ticks / TICK_PER_SEC;
  ts->tv_usec = (ticks % TICK_PER_SEC) * (USEC_PER_SEC / TICK_PER_SEC);
}

/****************************************************************************
 * Name: bk7258_camera_watchdog_arm
 *
 * Description:
 *   (Re)starts the capture watchdog and records the frame count it is
 *   measuring against.  Interrupt-safe.
 *
 ****************************************************************************/

static void bk7258_camera_watchdog_arm(
    FAR struct bk7258_camera_imgdata_s *priv)
{
  priv->frames_at_arm = priv->frames_done;
  wd_start(&priv->watchdog, MSEC2TICK(BK7258_CAMERA_WATCHDOG_MS),
           bk7258_camera_watchdog_expiry, (wdparm_t)priv);
}

/****************************************************************************
 * Name: bk7258_camera_frame_done
 *
 * Description:
 *   YUV_BUF frame-done (YUV_ARV) callback -- interrupt context, one call
 *   per completed frame already written into priv->frame_buf by the
 *   hardware.  Reports the frame to the V4L2 framework, which re-arms the
 *   next buffer through set_buf().  Must stay print-free.
 *
 ****************************************************************************/

static void bk7258_camera_frame_done(FAR void *arg)
{
  FAR struct bk7258_camera_imgdata_s *priv = arg;
  struct timeval ts;

  if (!priv->capturing || priv->frame_buf == NULL)
    {
      return;
    }

  priv->frames_done++;

  if (priv->capture_cb != NULL)
    {
      bk7258_camera_now(&ts);
      priv->capture_cb(0, priv->frame_buf_size, &ts, priv->capture_cb_arg);
    }

  if (priv->capturing)
    {
      bk7258_camera_watchdog_arm(priv);
    }
}

/****************************************************************************
 * Name: bk7258_camera_watchdog_expiry
 *
 * Description:
 *   No frame arrived within BK7258_CAMERA_WATCHDOG_MS.  Report an error
 *   frame so a blocked VIDIOC_DQBUF returns (with V4L2_BUF_FLAG_ERROR set
 *   on the buffer) instead of hanging the caller forever, then keep
 *   watching -- the hardware is left running so a late first frame is
 *   still captured.  Interrupt context: no printf().
 *
 ****************************************************************************/

static void bk7258_camera_watchdog_expiry(wdparm_t arg)
{
  FAR struct bk7258_camera_imgdata_s *priv =
      (FAR struct bk7258_camera_imgdata_s *)arg;
  struct timeval ts;

  if (!priv->capturing)
    {
      return;
    }

  if (priv->frames_done == priv->frames_at_arm)
    {
      priv->timeouts++;

      if (priv->capture_cb != NULL)
        {
          bk7258_camera_now(&ts);

          /* Non-zero result: complete_capture() marks the buffer
           * V4L2_BUF_FLAG_ERROR, completes it and posts dqbuf_wait_flg.
           */

          priv->capture_cb(EIO, 0, &ts, priv->capture_cb_arg);
        }
    }

  if (priv->capturing)
    {
      bk7258_camera_watchdog_arm(priv);
    }
}

static int bk7258_camera_imgdata_init(FAR struct imgdata_s *data)
{
  FAR struct bk7258_camera_imgdata_s *priv =
      (FAR struct bk7258_camera_imgdata_s *)data;

  printf("bk7258_camera_imgdata: init: entry\n");

  priv->capturing      = false;
  priv->frames_done    = 0;
  priv->frames_at_arm  = 0;
  priv->timeouts       = 0;
  priv->set_buf_calls  = 0;
  priv->rejected_bufs  = 0;

  bk7258_yuv_buf_init();
  bk7258_yuv_buf_set_frame_callback(bk7258_camera_frame_done, priv);

  printf("bk7258_camera_imgdata: init: complete\n");

  return OK;
}

static int bk7258_camera_imgdata_uninit(FAR struct imgdata_s *data)
{
  FAR struct bk7258_camera_imgdata_s *priv =
      (FAR struct bk7258_camera_imgdata_s *)data;
  FAR const uint8_t *p = priv->frame_buf;

  priv->capturing = false;
  wd_cancel(&priv->watchdog);
  bk7258_yuv_buf_stop();
  bk7258_yuv_buf_set_frame_callback(NULL, NULL);

  /* Task context (VIDIOC close path): safe to print, and this is the one
   * place that reports the whole session's outcome.  The first bytes of
   * the last frame buffer are dumped here because this board has no
   * filesystem big enough to save a 614400-byte frame (tmpfs lives in the
   * 336KB kernel heap), so a hexdump is the only way to confirm real
   * pixel data was captured rather than an all-zero buffer.
   */

  printf("bk7258_camera_imgdata: uninit: frames=%u timeouts=%u "
         "set_buf=%u rejected=%u\n",
         (unsigned int)priv->frames_done, (unsigned int)priv->timeouts,
         (unsigned int)priv->set_buf_calls,
         (unsigned int)priv->rejected_bufs);

  bk7258_yuv_buf_dump_status("uninit");

  if (p != NULL && priv->frames_done > 0)
    {
      printf("bk7258_camera_imgdata: uninit: frame[0..31] = "
             "%02x %02x %02x %02x %02x %02x %02x %02x "
             "%02x %02x %02x %02x %02x %02x %02x %02x "
             "%02x %02x %02x %02x %02x %02x %02x %02x "
             "%02x %02x %02x %02x %02x %02x %02x %02x\n",
             p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7],
             p[8], p[9], p[10], p[11], p[12], p[13], p[14], p[15],
             p[16], p[17], p[18], p[19], p[20], p[21], p[22], p[23],
             p[24], p[25], p[26], p[27], p[28], p[29], p[30], p[31]);
    }

  return OK;
}

static int bk7258_camera_imgdata_set_buf(FAR struct imgdata_s *data,
                                          uint8_t nr_datafmts,
                                          FAR imgdata_format_t *datafmts,
                                          uint8_t *addr, uint32_t size)
{
  FAR struct bk7258_camera_imgdata_s *priv =
      (FAR struct bk7258_camera_imgdata_s *)data;
  uint32_t phys = (uint32_t)(uintptr_t)addr;

  /* Interrupt context is possible here (v4l2_cap.c complete_capture()
   * re-arms the next buffer), so only report through counters unless
   * this is a task-level call.
   */

  if (addr == NULL || size < BK7258_CAMERA_FRAME_BYTES ||
      phys < BK7258_CAMERA_PSRAM_BASE ||
      phys + BK7258_CAMERA_FRAME_BYTES > BK7258_CAMERA_PSRAM_END)
    {
      priv->rejected_bufs++;

      if (!up_interrupt_context())
        {
          printf("bk7258_camera_imgdata: set_buf: rejected addr=%p "
                 "size=%u (need >=%u bytes inside PSRAM "
                 "0x%08x-0x%08x)\n",
                 addr, (unsigned int)size,
                 (unsigned int)BK7258_CAMERA_FRAME_BYTES,
                 (unsigned int)BK7258_CAMERA_PSRAM_BASE,
                 (unsigned int)BK7258_CAMERA_PSRAM_END);
        }

      return -EINVAL;
    }

  priv->frame_buf = addr;
  priv->frame_buf_size = BK7258_CAMERA_FRAME_BYTES;
  priv->set_buf_calls++;

  /* Point the hardware's frame writer at this buffer.  Done on every
   * call, which is also how the next frame gets re-armed from the
   * frame-done path (reference: dvp_camera_yuv_eof_handler()'s
   * bk_yuv_buf_set_em_base_addr()).
   */

  bk7258_yuv_buf_set_frame_buffer(phys);

  if (!up_interrupt_context())
    {
      printf("bk7258_camera_imgdata: set_buf: addr=%p frame_bytes=%u "
             "(buffer size given=%u)\n",
             addr, (unsigned int)priv->frame_buf_size, (unsigned int)size);
    }

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
      datafmts[IMGDATA_FMT_MAIN].pixelformat != IMGDATA_PIX_FMT_UYVY)
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

  printf("bk7258_camera_imgdata: start_capture: entry, frame_buf=%p "
         "frame_buf_size=%u\n", priv->frame_buf,
         (unsigned int)priv->frame_buf_size);

  ret = bk7258_camera_imgdata_validate_frame_setting(data, nr_datafmts,
                                                      datafmts, interval);
  if (ret < 0)
    {
      printf("bk7258_camera_imgdata: start_capture: validate failed, "
             "ret=%d\n", ret);
      return ret;
    }

  if (priv->frame_buf == NULL)
    {
      printf("bk7258_camera_imgdata: start_capture: frame_buf is NULL, "
             "-EINVAL\n");
      return -EINVAL;
    }

  priv->capture_cb = callback;
  priv->capture_cb_arg = arg;
  priv->frames_done = 0;
  priv->timeouts = 0;

  bk7258_yuv_buf_configure(BK7258_CAMERA_WIDTH, BK7258_CAMERA_HEIGHT);
  bk7258_yuv_buf_set_frame_buffer((uint32_t)(uintptr_t)priv->frame_buf);

  priv->capturing = true;
  priv->start_ticks = clock_systime_ticks();
  bk7258_yuv_buf_start();
  bk7258_camera_watchdog_arm(priv);

  bk7258_yuv_buf_dump_status("start_capture: started");

  printf("bk7258_camera_imgdata: start_capture: complete, watchdog=%dms\n",
         BK7258_CAMERA_WATCHDOG_MS);

  return OK;
}

static int bk7258_camera_imgdata_stop_capture(FAR struct imgdata_s *data)
{
  FAR struct bk7258_camera_imgdata_s *priv =
      (FAR struct bk7258_camera_imgdata_s *)data;

  priv->capturing = false;
  wd_cancel(&priv->watchdog);
  bk7258_yuv_buf_stop();

  /* Interrupt context is possible here: complete_capture() calls
   * IMGDATA_STOP_CAPTURE() when no vacant container is left.
   */

  if (!up_interrupt_context())
    {
      uint32_t ms = TICK2MSEC(clock_systime_ticks() - priv->start_ticks);

      /* Measured frame rate, not the requested one: the only way to see
       * whether the sensor's programmed rate is what the hardware
       * actually delivers.
       */

      printf("bk7258_camera_imgdata: stop_capture: frames=%u timeouts=%u "
             "elapsed=%ums measured=%u.%02u fps\n",
             (unsigned int)priv->frames_done,
             (unsigned int)priv->timeouts, (unsigned int)ms,
             (unsigned int)(ms ? priv->frames_done * 1000u / ms : 0u),
             (unsigned int)(ms ? priv->frames_done * 100000u / ms % 100u
                               : 0u));
    }

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
 * bk7258_media_pool_alloc()/_free() instead.  YUV_BUF also requires the
 * frame buffer to be in PSRAM (see set_buf()).
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
