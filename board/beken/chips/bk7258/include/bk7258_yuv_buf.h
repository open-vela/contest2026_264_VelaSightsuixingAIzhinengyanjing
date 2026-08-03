/****************************************************************************
 * board/beken/chips/bk7258/include/bk7258_yuv_buf.h
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef __VENDOR_BEKEN_CHIPS_BK7258_INCLUDE_BK7258_YUV_BUF_H
#define __VENDOR_BEKEN_CHIPS_BK7258_INCLUDE_BK7258_YUV_BUF_H

#include <stdint.h>

/* Minimal YUV_BUF driver: YUV direct-capture mode only (ctrl.yuv_mode=1,
 * ctrl.h264_mode=0), no JPEG/H264 hardware encoder path.
 *
 * Ping-pong semantics (per bk_avdk_smp release/v3.1.1
 * ap/components/bk_dvp/src/bk_dvp.c yuv_sm0_line_done()/
 * yuv_sm1_line_done()): the hardware's PSRAM line buffer is two adjacent
 * regions of bk7258_yuv_buf_get_line_batch_bytes() bytes each, both
 * starting at bk7258_yuv_buf_get_line_buf_addr().  SM0_WR fires when the
 * *first* region (offset 0) has just been filled with the next 8 lines;
 * SM1_WR fires when the *second* region (offset
 * +line_batch_bytes) has been filled.  The hardware keeps alternating
 * between the two regions on every successive batch of 8 lines,
 * regardless of whether software has finished draining the previous one
 * -- callers must copy out each region promptly (via DMA) before the
 * hardware writes the *next* occurrence of that same region, or the data
 * is silently overwritten.  This driver's callback signature exposes
 * which region (SM0 or SM1) fired so callers can compute the correct DMA
 * source address without needing to know the ping-pong length
 * themselves (see bk7258_yuv_buf_get_line_batch_bytes()).
 */

typedef enum bk7258_yuv_buf_bank_e
{
  BK7258_YUV_BUF_BANK_SM0 = 0,
  BK7258_YUV_BUF_BANK_SM1 = 1,
} bk7258_yuv_buf_bank_t;

typedef void (*bk7258_yuv_buf_line_cb_t)(bk7258_yuv_buf_bank_t bank,
                                          void *arg);

void bk7258_yuv_buf_init(void);
void bk7258_yuv_buf_configure(uint16_t width, uint16_t height);
uint32_t bk7258_yuv_buf_get_line_buf_addr(void);
uint32_t bk7258_yuv_buf_get_line_batch_bytes(void);
void bk7258_yuv_buf_set_line_callback(bk7258_yuv_buf_line_cb_t cb, void *arg);
void bk7258_yuv_buf_start(void);
void bk7258_yuv_buf_stop(void);

#endif /* __VENDOR_BEKEN_CHIPS_BK7258_INCLUDE_BK7258_YUV_BUF_H */
