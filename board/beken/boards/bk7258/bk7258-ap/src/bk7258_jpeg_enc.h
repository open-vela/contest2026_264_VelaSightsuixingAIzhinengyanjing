/****************************************************************************
 * vendor/beken/boards/bk7258/bk7258-ap/src/bk7258_jpeg_enc.h
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef __VENDOR_BEKEN_BOARDS_BK7258_BK7258_AP_SRC_BK7258_JPEG_ENC_H
#define __VENDOR_BEKEN_BOARDS_BK7258_BK7258_AP_SRC_BK7258_JPEG_ENC_H

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

#ifdef __cplusplus
extern "C"
{
#endif

/****************************************************************************
 * Name: bk7258_jpeg_enc_initialize
 *
 * Description:
 *   Register the V4L2 M2M JPEG encoder at CONFIG_BK7258_JPEG_ENC_DEV_PATH.
 *
 *   Call this after PSRAM is up: the encoder's buffers and its de-interleave
 *   scratch both come from the PSRAM heap, because a single 640x480 I420
 *   frame is larger than the AP's entire malloc heap.  Registration itself
 *   allocates nothing, so an early call would not fail outright -- it would
 *   fail later at VIDIOC_REQBUFS, which is much harder to diagnose.
 *
 * Returned Value:
 *   Zero on success, a negated errno on failure.
 *
 ****************************************************************************/

int bk7258_jpeg_enc_initialize(void);

#ifdef __cplusplus
}
#endif

#endif /* __VENDOR_BEKEN_BOARDS_BK7258_BK7258_AP_SRC_BK7258_JPEG_ENC_H */
