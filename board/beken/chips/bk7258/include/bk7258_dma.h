/****************************************************************************
 * board/beken/chips/bk7258/include/bk7258_dma.h
 *
 * Minimal single-channel (channel 0), single-shot memory-to-memory DMA
 * driver, sized for copying YUV_BUF line batches into a frame buffer.
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef __VENDOR_BEKEN_CHIPS_BK7258_INCLUDE_BK7258_DMA_H
#define __VENDOR_BEKEN_CHIPS_BK7258_INCLUDE_BK7258_DMA_H

#include <stdbool.h>
#include <stdint.h>

/* Not a general DMA subsystem: single fixed channel, no channel
 * allocation pool, no ring-buffer/REPEAT mode, no security-attribute
 * configuration.  bk7258_dma_configure()+bk7258_dma_start() may be
 * called repeatedly (e.g. once per YUV_BUF line-batch-done interrupt)
 * to issue a new transfer once the previous one has completed.
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
