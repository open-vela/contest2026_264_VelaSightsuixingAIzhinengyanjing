/****************************************************************************
 * board/beken/chips/bk7258/include/bk7258_yuv_buf.h
 *
 * BK7258 YUV_BUF controller, YUV direct-capture mode only.
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef __VENDOR_BEKEN_CHIPS_BK7258_INCLUDE_BK7258_YUV_BUF_H
#define __VENDOR_BEKEN_CHIPS_BK7258_INCLUDE_BK7258_YUV_BUF_H

#include <stdint.h>

#include <nuttx/compiler.h>

/* Whole-frame direct capture, per the reference implementation's pure-YUV
 * path (bk_avdk_smp ap/components/bk_dvp/src/bk_dvp.c
 * dvp_camera_yuv_mode(): em_base_addr is set to the frame buffer itself,
 * and frame completion is reported by the YUV_ARV interrupt).  The
 * hardware writes one complete YUV422 frame into the buffer handed to
 * bk7258_yuv_buf_set_frame_buffer() and raises YUV_ARV when the frame is
 * done -- no line-batch ping-pong buffer and no CPU/DMA copy is involved.
 *
 * The SM0_WR/SM1_WR line-batch interrupts this driver used to consume are
 * deliberately NOT used: the reference only registers those for the
 * combined YUV+encode formats ("(format & IMAGE_YUV) && format !=
 * IMAGE_YUV", bk_dvp.c dvp_camera_register_isr_function()), where the
 * line buffer feeds the JPEG/H264 encoder.  For pure YUV capture they
 * would fire ~60 times per frame (1800/s at 30fps) for no benefit.
 */

/* Called from interrupt context once per completed frame (YUV_ARV).
 * MUST be interrupt-safe: no printf(), no blocking, no allocation.
 */

typedef void (*bk7258_yuv_buf_frame_cb_t)(FAR void *arg);

/* Interrupt/event counters, for task-level diagnostics.  Snapshot with
 * bk7258_yuv_buf_get_stats(); all counters are cleared by
 * bk7258_yuv_buf_configure().
 */

struct bk7258_yuv_buf_stats_s
{
  uint32_t isr_count;        /* Total ISR invocations. */
  uint32_t frame_count;      /* YUV_ARV (one completed frame) events. */
  uint32_t vsync_count;      /* VSYNC negedge events. */
  uint32_t err_count;        /* fifo-full/resolution-err/enc-slow/h264-err */
  uint32_t last_status;      /* int_status of the most recent ISR. */
  uint32_t err_status;       /* OR of all error bits seen so far. */
};

/* Powers on the video pipeline, enables YUV_BUF's clock gate, releases
 * the module from soft reset, bypasses its clock gate, and
 * attaches/enables its interrupt.  Must be called once before
 * bk7258_yuv_buf_configure()/_start().  Task context only (prints
 * diagnostics).
 */

void bk7258_yuv_buf_init(void);

/* Programs YUV_BUF for direct YUV422 whole-frame capture at the given
 * resolution: pixel/resize_pixel dimensions, format/sync/mclk-divider
 * ctrl fields, error masks and the interrupt enables.  Clears the
 * statistics counters.  Must be called before bk7258_yuv_buf_start(),
 * and the frame buffer must be installed separately with
 * bk7258_yuv_buf_set_frame_buffer().  Task context only (prints
 * diagnostics).
 */

void bk7258_yuv_buf_configure(uint16_t width, uint16_t height);

/* Points the hardware's frame writer at 'addr' (em_base_addr and
 * emr_base_addr, which the reference always sets to the same value for
 * a full frame -- bk_yuv_buf_set_em_base_addr()).  'addr' must be a
 * PSRAM address holding at least width*height*2 bytes.  Interrupt-safe:
 * this is called from the V4L2 framework's capture-done callback
 * (nuttx/drivers/video/v4l2_cap.c complete_capture() -> IMGDATA_SET_BUF)
 * to re-arm the next buffer, which runs in interrupt context.
 */

void bk7258_yuv_buf_set_frame_buffer(uint32_t addr);

/* Address most recently installed by bk7258_yuv_buf_set_frame_buffer(),
 * or 0 if none.  Interrupt-safe.
 */

uint32_t bk7258_yuv_buf_get_frame_buffer(void);

/* Registers the frame-done (YUV_ARV) callback.  Pass cb == NULL to
 * unregister.
 */

void bk7258_yuv_buf_set_frame_callback(bk7258_yuv_buf_frame_cb_t cb,
                                       FAR void *arg);

/* Enables YUV direct-capture mode: arms the frame interrupts and sets
 * ctrl.yuv_mode=1 / ctrl.h264_mode=0.  Interrupt-safe.
 */

void bk7258_yuv_buf_start(void);

/* Same as bk7258_yuv_buf_start() except that the module's own frame writer
 * stays off, because the JPEG encoder is consuming the pixel stream instead.
 * Encoded-frame completion arrives on the JPEG block's EOF interrupt; the
 * VSYNC interrupt is still enabled here because the capture watchdog uses it
 * as the "sensor is alive" marker.
 */

void bk7258_yuv_buf_start_jpeg(void);

/* Called on every VSYNC negedge, i.e. at each frame boundary.  Runs in
 * interrupt context.  The JPEG path uses it for error recovery, since in
 * JPEG mode the frame-done callback never fires.
 */

void bk7258_yuv_buf_set_vsync_callback(bk7258_yuv_buf_frame_cb_t cb,
                                      FAR void *arg);

/* Pulses the module's global soft reset (0 = held, 1 = released), leaving
 * the clock gate bypassed.  Print-free; callable from interrupt context.
 */

void bk7258_yuv_buf_soft_reset(void);

/* Disables YUV direct-capture mode and silences the interrupts again.
 * Interrupt-safe: the V4L2 framework calls IMGDATA_STOP_CAPTURE from
 * complete_capture() (interrupt context) when it runs out of vacant
 * buffers, so this must not print or block.
 */

void bk7258_yuv_buf_stop(void);

/* Snapshot of the interrupt/event counters.  Interrupt-safe. */

void bk7258_yuv_buf_get_stats(FAR struct bk7258_yuv_buf_stats_s *stats);

/* Debug: prints the module identity registers, global_ctrl/ctrl/int_en/
 * int_status/em_base_addr and the event counters, tagged with the given
 * label.  TASK CONTEXT ONLY -- it uses printf().  Reads raw registers,
 * so it stays meaningful even if the interrupt path is broken: a zero
 * int_status across repeated calls means the hardware produced no event
 * at all, as opposed to an event that was never delivered to the CPU.
 */

void bk7258_yuv_buf_dump_status(FAR const char *tag);

#endif /* __VENDOR_BEKEN_CHIPS_BK7258_INCLUDE_BK7258_YUV_BUF_H */
