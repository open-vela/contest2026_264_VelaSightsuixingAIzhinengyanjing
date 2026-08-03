/****************************************************************************
 * board/beken/chips/bk7258/include/bk7258_dma.h
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef __VENDOR_BEKEN_CHIPS_BK7258_INCLUDE_BK7258_DMA_H
#define __VENDOR_BEKEN_CHIPS_BK7258_INCLUDE_BK7258_DMA_H

#include <stdbool.h>
#include <stdint.h>

/* Minimal single-channel (channel 0), single-shot (SINGLE mode) memory-to-
 * memory DMA driver.  Modeled after bk_avdk_smp release/v3.1.1
 * ap/components/bk_dvp/src/bk_dvp.c encode_yuv_dma_cpy(): both source and
 * destination are plain memory addresses (DMA_DEV_DTCM-style), not a
 * hardware FIFO peripheral bound via req_mux.  The driver is intentionally
 * re-configurable: bk7258_dma_configure()+bk7258_dma_start() may be called
 * repeatedly (e.g. once per YUV_BUF line-done interrupt) to issue a new
 * transfer once the previous one has completed.
 *
 * Not a general DMA subsystem: no channel allocation pool, no REPEAT/
 * ADDR_LOOP ring-buffer mode (only needed by JPEG/H264 continuous-encode
 * paths), no security-attribute configuration.
 */

typedef void (*bk7258_dma_done_cb_t)(void *arg);

void bk7258_dma_init(void);
void bk7258_dma_configure(uint32_t src_addr, uint32_t dest_addr,
                           uint32_t transfer_len);
void bk7258_dma_set_done_callback(bk7258_dma_done_cb_t cb, void *arg);
void bk7258_dma_start(void);
void bk7258_dma_stop(void);
bool bk7258_dma_is_busy(void);
uint32_t bk7258_dma_get_remain_len(void);

#endif /* __VENDOR_BEKEN_CHIPS_BK7258_INCLUDE_BK7258_DMA_H */
