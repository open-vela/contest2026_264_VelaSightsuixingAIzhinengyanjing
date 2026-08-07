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

/* YUV_BUF's PSRAM line buffer is two adjacent, fixed-size ping-pong
 * regions, each bk7258_yuv_buf_get_line_batch_bytes() bytes, both
 * anchored at bk7258_yuv_buf_get_line_buf_addr().  The hardware
 * alternates filling one region with the next 8 lines of YUV422 data
 * and firing that region's write-done interrupt (SM0_WR / SM1_WR),
 * regardless of whether the previous occupant has been drained yet.
 * Callers (see board/beken/chips/bk7258/bk7258_camera_imgdata.c) must
 * copy each region out via DMA before its next occurrence is written,
 * or the data is silently overwritten.
 */

typedef enum bk7258_yuv_buf_bank_e
{
  BK7258_YUV_BUF_BANK_SM0 = 0,
  BK7258_YUV_BUF_BANK_SM1 = 1,
} bk7258_yuv_buf_bank_t;

typedef void (*bk7258_yuv_buf_line_cb_t)(bk7258_yuv_buf_bank_t bank,
                                          void *arg);

/* Powers on the video pipeline, enables YUV_BUF's clock gate, performs
 * the module soft-reset pulse, and attaches/enables its interrupt.
 * Must be called once before bk7258_yuv_buf_configure()/_start().
 */

void bk7258_yuv_buf_init(void);

/* Programs YUV_BUF for direct YUV422 capture at the given resolution:
 * pixel/resize_pixel dimensions, format/sync/mclk-divider ctrl fields,
 * and the fixed PSRAM line-buffer base address.  Must be called before
 * bk7258_yuv_buf_start().
 */

void bk7258_yuv_buf_configure(uint16_t width, uint16_t height);

/* Fixed PSRAM base address of the ping-pong line buffer (SOC_PSRAM_
 * DATA_BASE); use as the DMA source address together with the bank
 * reported to bk7258_yuv_buf_line_cb_t.
 */

uint32_t bk7258_yuv_buf_get_line_buf_addr(void);

/* Size in bytes of one ping-pong region (one 8-line batch at the
 * resolution passed to the most recent bk7258_yuv_buf_configure()
 * call).
 */

uint32_t bk7258_yuv_buf_get_line_batch_bytes(void);

/* Registers the callback invoked from interrupt context for every
 * line-batch-done event, identifying which bank (SM0/SM1) just
 * finished.  Pass cb == NULL to unregister.
 */

void bk7258_yuv_buf_set_line_callback(bk7258_yuv_buf_line_cb_t cb,
                                       void *arg);

/* Enables YUV direct-capture mode (ctrl.yuv_mode=1, ctrl.h264_mode=0).
 */

void bk7258_yuv_buf_start(void);

/* Disables YUV direct-capture mode. */

void bk7258_yuv_buf_stop(void);

#endif /* __VENDOR_BEKEN_CHIPS_BK7258_INCLUDE_BK7258_YUV_BUF_H */
