/****************************************************************************
 * board/beken/boards/bk7258/bk7258-ap/src/bk7258_ramdisk.c
 *
 * PSRAM-backed RAM disk so that captured frames can be written to a real
 * file ("file mode" of apps/system/nxcamera's `output <path> [nframes]`).
 *
 * Why this exists: the only writable filesystem this board had was tmpfs,
 * whose storage comes from the kernel heap.  CONFIG_RAM_SIZE is 344064
 * bytes and at the time the SRAM heap held roughly 300KB of that, while one
 * 640x480 YUYV frame is 614400 bytes -- so `output /tmp/frame.yuv 1` could
 * never succeed, the write() always failed with ENOMEM.  This board does,
 * however, have 16MB of PSRAM, so a RAM disk carved out of PSRAM gives the
 * filesystem enough room for several full frames without touching the SRAM
 * heap at all.
 *
 * Since "fix(psram): give the system heap the unused tail of PSRAM_SECTION"
 * the system heap itself reaches into PSRAM (6.4MB arena), so tmpfs is no
 * longer categorically too small.  This ramdisk is still the better place for
 * frame-sized files: it does not compete with task stacks and allocations for
 * the same free blocks, and its 2MB is bounded and predictable.
 *
 * Usage after boot (the ramdisk is registered but intentionally left
 * unformatted, since its contents are volatile anyway):
 *
 *   mkfatfs /dev/ram0
 *   mount -t vfat /dev/ram0 /mnt
 *   nxcamera
 *     input /dev/video0
 *     output /mnt/frame.yuv 1     <- exactly one frame, then stream stops
 *     stream 640 480 30 YUYV
 *     q
 *   ls -l /mnt
 *   hexdump /mnt/frame.yuv count=64
 *
 * The .yuv file is raw YUYV (YUV422), so on a host it converts with e.g.
 *   ffmpeg -f rawvideo -pix_fmt yuyv422 -s 640x480 -i frame.yuv frame.png
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdint.h>
#include <stdio.h>
#include <errno.h>

#include <nuttx/drivers/ramdisk.h>

#include "bk7258_psram.h"
#include "bk7258_ramdisk.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define BK7258_RAMDISK_MINOR      0
#define BK7258_RAMDISK_SECTOR     512u

/* 2MB: enough for three 640x480 YUYV frames plus FAT overhead, out of the
 * 2.9MB AP PSRAM heap (BK7258_AP_PSRAM_HEAP_*, bk7258_psram.c).  The V4L2
 * capture buffers do not come from this heap -- they are allocated from the
 * separate PSRAM DISPLAY pool by bk7258_camera_imgdata.c -- so the two do
 * not compete.
 */

#define BK7258_RAMDISK_BYTES      (2u * 1024u * 1024u)
#define BK7258_RAMDISK_SECTORS    (BK7258_RAMDISK_BYTES / BK7258_RAMDISK_SECTOR)

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int bk7258_ramdisk_initialize(void)
{
#ifdef CONFIG_BK7258_PSRAM
  FAR uint8_t *buffer;
  int ret;

  if (!bk7258_psram_is_online())
    {
      printf("ramdisk: PSRAM offline, /dev/ram0 not registered\n");
      return -ENODEV;
    }

  buffer = bk7258_psram_memalign(BK7258_RAMDISK_SECTOR,
                                 BK7258_RAMDISK_BYTES);
  if (buffer == NULL)
    {
      printf("ramdisk: PSRAM allocation of %u bytes failed\n",
             (unsigned int)BK7258_RAMDISK_BYTES);
      return -ENOMEM;
    }

  ret = ramdisk_register(BK7258_RAMDISK_MINOR, buffer,
                         BK7258_RAMDISK_SECTORS, BK7258_RAMDISK_SECTOR,
                         RDFLAG_WRENABLED);
  if (ret < 0)
    {
      printf("ramdisk: ramdisk_register failed, error=%d\n", ret);
      bk7258_psram_free(buffer);
      return ret;
    }

  printf("ramdisk: /dev/ram0 registered, %u KB at %p (PSRAM); format with "
         "\"mkfatfs /dev/ram0\" then \"mount -t vfat /dev/ram0 /mnt\"\n",
         (unsigned int)(BK7258_RAMDISK_BYTES / 1024u), buffer);

  return OK;
#else
  return -ENOTSUP;
#endif
}
