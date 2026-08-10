/****************************************************************************
 * board/beken/boards/bk7258/bk7258-ap/src/bk7258_ramdisk.h
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef __BOARDS_BEKEN_BK7258_AP_SRC_BK7258_RAMDISK_H
#define __BOARDS_BEKEN_BK7258_AP_SRC_BK7258_RAMDISK_H

/* Registers /dev/ram0, a 2MB PSRAM-backed RAM disk, so captured frames can
 * be written to a real file (the kernel-heap-backed tmpfs is far too small
 * for a 614400-byte 640x480 YUYV frame).  Returns OK, or a negated errno if
 * PSRAM is offline or the allocation/registration fails.  Must be called
 * after PSRAM is online.
 */

int bk7258_ramdisk_initialize(void);

#endif /* __BOARDS_BEKEN_BK7258_AP_SRC_BK7258_RAMDISK_H */
