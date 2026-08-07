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
#include <nuttx/irq.h>
#include <nuttx/spinlock.h>

#include <sys/time.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>

#include <nuttx/video/imgdata.h>

#include "arm_internal.h"
#include "bk7258_yuv_buf.h"
#include "bk7258_dma.h"
#include "bk7258_camera_imgdata.h"
#include "bk7258_psram.h"
#include "hardware/bk7258_gpio.h"

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
static void *bk7258_camera_imgdata_alloc(FAR struct imgdata_s *data,
                                          uint32_t align_size,
                                          uint32_t size);
static void bk7258_camera_imgdata_free(FAR struct imgdata_s *data,
                                        FAR void *addr);

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

  /* Diagnostic: poll int_status directly instead of relying on the ISR
   * actually firing, per this project's systematic-debugging rule of
   * stopping to re-verify architecture assumptions after 3+ same-
   * direction register-enable fixes (MCLK, YUV_BUF power/clock,
   * H264 clock gate, full ctrl field set) all failed to produce a
   * single "isr fired" log. This distinguishes two very different
   * failure modes that look identical from the ISR-fire-count alone:
   *   - int_status stays 0x00000000 across all three reads below ->
   *     the DVP data lines (PCLK/HSYNC/VSYNC/D0-7) never produced a
   *     signal YUV_BUF's hardware recognized as a valid line/frame
   *     event at all; the problem is upstream of YUV_BUF entirely
   *     (GC2145 not actually streaming, or a DVP-level enable this
   *     port never touched).
   *   - int_status shows nonzero bits (even though isr_fire_count
   *     stays 0) -> the hardware side is working correctly and the
   *     bug is purely in this driver's IRQ routing/NVIC setup, since
   *     the hardware is asserting the interrupt condition but the
   *     CPU is never taking the exception. */
  up_mdelay(200);
  bk7258_yuv_buf_dump_status();
  up_mdelay(200);
  bk7258_yuv_buf_dump_status();
  up_mdelay(200);
  bk7258_yuv_buf_dump_status();

  /* Further diagnostic: with int_status confirmed stuck at 0x00000000
   * across all three polls above (ruling out both the IRQ-routing
   * hypothesis and YUV_BUF's own ctrl/int_en configuration, both
   * already fixed and verified correct in prior rounds), the failure
   * must be upstream of YUV_BUF -- either the DVP pins are not
   * actually carrying the GC2145's signal, or GPIO pinmux for those
   * pins was silently overwritten/never took effect at runtime (the
   * exact same class of bug the MCLK root cause turned out to be:
   * software believed a pinmux write succeeded, but the pin's actual
   * runtime GPIO_CFG value told a different story).  Dump the live
   * GPIO_CFG register for every DVP signal pin (MCLK/PCLK/HSYNC/VSYNC/
   * D0-D7) the same way bk7258_camera_imgsensor.c's power_on/reset
   * logging already does for the sensor control pins, rather than
   * trusting that bk7258_gc2145_dvp_pinmux()'s writes are still in
   * effect by the time capture actually starts. */
  {
    static const uint32_t dvp_pins[] =
      {
        27, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39
      };
    static const char * const dvp_names[] =
      {
        "MCLK", "PCLK", "HSYNC", "VSYNC",
        "D0", "D1", "D2", "D3", "D4", "D5", "D6", "D7"
      };
    unsigned int i;

    for (i = 0; i < sizeof(dvp_pins) / sizeof(dvp_pins[0]); i++)
      {
        uint32_t pin = dvp_pins[i];
        uint32_t sys_reg = getreg32(BK7258_GPIO_SYS_CFG(pin));
        uint32_t func_sel = (sys_reg >> BK7258_GPIO_SYS_SHIFT(pin)) & 0xfu;

        printf("bk7258_camera_imgdata: DVP %-5s (GPIO%u) CFG=0x%08x "
               "SYS_CFG=0x%08x func_sel=%u\n",
               dvp_names[i], (unsigned int)pin,
               (unsigned int)getreg32(BK7258_GPIO_CFG(pin)),
               (unsigned int)sys_reg, (unsigned int)func_sel);
      }
  }

  /* Software "logic analyzer" fallback: no oscilloscope/logic analyzer is
   * available for this bring-up session, so this poll-based diagnostic is
   * the only way left to distinguish "GC2145 truly outputs no DVP signal
   * at all" from "signal exists but YUV_BUF's own sampling/edge-detect
   * config is wrong" -- the two failure modes that remain after this
   * project's systematic-debugging pass (register tables byte-for-byte
   * verified against bk_avdk_smp, YUV_BUF power/clock/ctrl fields all
   * confirmed matching reference, GPIO func_sel indices all confirmed
   * correct) both produce the exact same int_status=0x00000000 symptom
   * and cannot be told apart from register state alone.
   *
   * Temporarily reassigns PCLK (GPIO29) and VSYNC (GPIO31) away from
   * their DVP second-function mux back to plain GPIO input mode, then
   * busy-polls each pin's raw input bit as fast as possible for a fixed
   * number of iterations, counting how many times the read level
   * differs from the previous read (a "toggle").  This cannot capture a
   * real waveform (GC2145's PCLK is tens of MHz, far above what a
   * software polling loop can sample without aliasing), but it can
   * reliably distinguish the two cases that matter here:
   *   - toggle_count stays at 0 (or single-digit, i.e. noise) across
   *     10000 reads -> the pin is electrically stuck at a constant
   *     level; GC2145 is not driving any signal onto it at all, and
   *     the problem is entirely upstream of YUV_BUF (sensor not
   *     actually streaming, or the DVP bus never reaching the pin).
   *   - toggle_count is a large fraction of the read count -> the pin
   *     is genuinely switching states at a rate this polling loop can
   *     detect (even if aliased/undersampled relative to the real
   *     PCLK frequency), proving a real signal exists on the bus and
   *     narrowing the fault to YUV_BUF's own sampling/edge-detect
   *     configuration rather than the sensor/DVP-bus side.
   *
   * BUG FOUND AND FIXED (2026-08-07, before this diagnostic's first real
   * hardware run completed): the previous version of this block ran
   * with YUV_BUF already actively capturing (bk7258_yuv_buf_start() was
   * called earlier in this same function, confirmed by the "ctrl=
   * 0x00080e01" -- bit0 yuv_mode already set -- in the serial log this
   * diagnostic produced) and reassigned PCLK's pinmux out from under it
   * without first stopping the hardware module that was actively
   * depending on that pin as its sampling/edge-detect clock input.  The
   * board hung with no further serial output after the GPIO func_sel
   * dump (confirmed via live serial monitoring: the board was still
   * running -- not crashed/panicked -- but produced zero new output),
   * consistent with the YUV_BUF hardware state machine (sync_edge_dect_en
   * enabled, expecting a live PCLK edge stream) entering an unexpected
   * state when its clock input was yanked away, and/or the tight
   * uninterrupted 10000-iteration busy-read loop (no yield point) on
   * this single-core (CONFIG_SMP not set) board starving the mb_uart
   * worker thread that this file's own printf() calls depend on via
   * bk7258_syslog_write_force()'s 20ms bk7258_mbox_uart_flush() to make
   * any diagnostic output visible at all -- either mechanism alone
   * explains "board alive, log silent, forever" without invoking any
   * new hardware hypothesis. Fixed by: (1) stopping YUV_BUF before
   * reassigning the pins and restarting it afterward, so the hardware
   * module is never left depending on a pin this code has stolen; (2)
   * inserting a yield point every 256 iterations of the busy-read loop
   * so a single-core board's lower-priority threads (mb_uart worker in
   * particular) are not starved for the ~10000-iteration duration.
   *
   * Pins are restored to their original DVP second-function pinmux
   * (function index 0, GPIO_DEV_JPEG_PCLK/VSYNC per bk7258_camera_
   * imgsensor.c's DVP_PINMUX_FUNCTION) immediately afterward so this
   * diagnostic does not leave the capture path non-functional for any
   * subsequent frames, even though no frames have successfully
   * completed in any run so far. */
  {
    static const uint32_t poll_pins[] = { 29, 31 };
    static const char * const poll_names[] = { "PCLK", "VSYNC" };
    unsigned int p;

    /* Stop YUV_BUF before stealing pins it is actively sampling from --
     * see bug note above.  Restarted unconditionally after the poll
     * loop below, regardless of which iteration this loop is on. */
    bk7258_yuv_buf_stop();

    for (p = 0; p < sizeof(poll_pins) / sizeof(poll_pins[0]); p++)
      {
        uint32_t pin = poll_pins[p];
        uint32_t toggle_count = 0;
        uint32_t high_count = 0;
        uint32_t low_count = 0;
        uint32_t prev;
        uint32_t cur;
        uint32_t n;
        irqstate_t flags;

        /* Reassign to plain GPIO input (no second-function, no pull --
         * matches bk7258_gpio_input_pullup() minus the pull-up, since a
         * pulled-up idle-high reading could mask a genuinely
         * floating/undriven pin as "toggling" due to bus noise; leaving
         * it unpulled makes a truly-undriven pin more likely to read as
         * a stable rail-referenced level determined by whatever the
         * DVP transceiver leaves it at, not artificially pulled high). */
        flags = enter_critical_section();
        modifyreg32(BK7258_GPIO_CFG(pin),
                    BK7258_GPIO_OUTPUT | BK7258_GPIO_SECOND_FUNCTION |
                    BK7258_GPIO_PULL_ENABLE,
                    BK7258_GPIO_INPUT_ENABLE | BK7258_GPIO_OUTPUT_DISABLE);
        leave_critical_section(flags);

        prev = getreg32(BK7258_GPIO_CFG(pin)) & BK7258_GPIO_INPUT;
        if (prev != 0)
          {
            high_count++;
          }
        else
          {
            low_count++;
          }

        for (n = 0; n < 10000; n++)
          {
            cur = getreg32(BK7258_GPIO_CFG(pin)) & BK7258_GPIO_INPUT;
            if (cur != 0)
              {
                high_count++;
              }
            else
              {
                low_count++;
              }

            if (cur != prev)
              {
                toggle_count++;
              }

            prev = cur;

            /* Yield every 256 iterations so this single-core board's
             * other threads (mb_uart worker in particular) are not
             * starved for the whole ~10000-iteration duration -- see
             * bug note above.  256 is short enough that the resulting
             * gaps do not meaningfully change what this diagnostic can
             * detect (it was never able to resolve real PCLK-rate
             * edges to begin with; it only distinguishes "never
             * toggles" from "toggles a lot"). */
            if ((n & 0xffu) == 0xffu)
              {
                usleep(0);
              }
          }

        printf("bk7258_camera_imgdata: SW poll GPIO%u (%s): 10000 reads, "
               "toggle_count=%u high_count=%u low_count=%u "
               "(toggle_count near 0 => pin electrically stuck, no "
               "signal from sensor; toggle_count large => real signal "
               "present, fault is in YUV_BUF sampling not the sensor)\n",
               (unsigned int)pin, poll_names[p],
               (unsigned int)toggle_count, (unsigned int)high_count,
               (unsigned int)low_count);

        /* Restore DVP second-function pinmux (function index 0) so the
         * capture path is left in its normal configuration. */
        flags = enter_critical_section();
        modifyreg32(BK7258_GPIO_SYS_CFG(pin), BK7258_GPIO_SYS_MASK(pin),
                    (0u & 0xfu) << BK7258_GPIO_SYS_SHIFT(pin));
        modifyreg32(BK7258_GPIO_CFG(pin),
                    BK7258_GPIO_INPUT_ENABLE | BK7258_GPIO_OUTPUT |
                    BK7258_GPIO_PULL_ENABLE,
                    BK7258_GPIO_OUTPUT_DISABLE | BK7258_GPIO_SECOND_FUNCTION);
        leave_critical_section(flags);
      }

    /* Restart YUV_BUF now that both pins are back in their normal DVP
     * pinmux configuration, so capture continues (or at least resumes
     * its normal non-functional state) exactly as it would have without
     * this diagnostic block ever running. */
    bk7258_yuv_buf_start();
  }

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

/* V4L2_REQBUFS_COUNT_MAX (3) x one 640x480 YUYV frame (614400 bytes) is
 * ~1.8MB -- this repo's internal SRAM heap is only
 * CONFIG_RAM_SIZE=344064 bytes (336KB) total, so relying on the
 * framework's default kumm_memalign()-backed allocation (see
 * nuttx/drivers/video/v4l2_cap.c's capture_reqbufs()) for the capture
 * buffer pool always fails with -ENOMEM before a single frame can ever
 * be captured, regardless of anything sensor/timing/register related
 * (confirmed via nxcamera's "Error stream test: -12" during hardware
 * bring-up, once the unrelated MCLK and file-output-mode issues that
 * were previously masking this were fixed).  This board does have a
 * 16MB PSRAM region (see bk7258_psram.c's bk7258_psram_initialize()),
 * but bk7258_allocateheap.c's up_allocate_heap()/arm_addregion() never
 * add that region to the kernel's own malloc arena, so kumm_memalign()
 * can never reach it.
 *
 * Rather than modifying the shared allocateheap/addregion code (owned
 * by the board-level PSRAM subsystem, out of scope for this camera
 * driver), this uses the imgdata_ops_s.alloc/free optional interface
 * (nuttx/include/nuttx/video/imgdata.h: "This is a pair of user define
 * frame memory allocation interface... If both are NULL, just using
 * system memory operations") -- exactly the mechanism the camera
 * driver framework itself provides for drivers whose buffers need to
 * come from a non-default memory pool, routed to bk7258_psram.c's
 * already-existing bk7258_media_pool_alloc()/_free() public API
 * (BK7258_PSRAM_POOL_DISPLAY, the pool this codebase's own naming
 * convention designates for video/media frame buffers) instead of
 * duplicating PSRAM heap-management logic in this file. */

static void *bk7258_camera_imgdata_alloc(FAR struct imgdata_s *data,
                                          uint32_t align_size,
                                          uint32_t size)
{
  return bk7258_media_pool_alloc(BK7258_PSRAM_POOL_DISPLAY,
                                  align_size, size);
}

static void bk7258_camera_imgdata_free(FAR struct imgdata_s *data,
                                        FAR void *addr)
{
  bk7258_media_pool_free(BK7258_PSRAM_POOL_DISPLAY, addr);
}

FAR struct imgdata_s *bk7258_camera_imgdata_initialize(void)
{
  return &g_bk7258_camera_imgdata.data;
}
