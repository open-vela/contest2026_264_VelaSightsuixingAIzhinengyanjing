/****************************************************************************
 * board/beken/chips/bk7258/include/bk7258_qspi.h
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef __VENDOR_BEKEN_CHIPS_BK7258_INCLUDE_BK7258_QSPI_H
#define __VENDOR_BEKEN_CHIPS_BK7258_INCLUDE_BK7258_QSPI_H

#include <stdint.h>

/* Minimal QSPI0 command-channel driver.  Supports issuing a single
 * indirect command (opcode + optional up to 4 data bytes) and polling for
 * completion.  Does not implement DMA-backed bulk transfer or the receive
 * (cmd_d) channel's multi-word read path beyond what bk7258_qspi_read_id
 * needs.
 */

void bk7258_qspi0_init(void);
bool bk7258_qspi0_send_cmd(uint8_t cmd, const uint8_t *data, uint8_t data_len);
uint32_t bk7258_qspi0_read_id(void);

/* Pushes one full frame of pixel data to a GC9-series panel: holds CS low,
 * sends the {0x02, 0x00, RAMWR, 0x00} header, switches the data lines to
 * the memory-mapped window and streams 'len' bytes from 'frame'.  'len'
 * must be a multiple of 4.  Returns false if the header transfer times out
 * or the arguments are invalid.  Caller is responsible for having set the
 * drawing window (CASET/RASET) beforehand.
 */

bool bk7258_qspi1_lcd_write_frame(FAR const void *frame, size_t len);

/* Number of frames pushed since boot, for diagnostics. */

uint32_t bk7258_qspi1_lcd_frame_count(void);

#endif /* __VENDOR_BEKEN_CHIPS_BK7258_INCLUDE_BK7258_QSPI_H */
