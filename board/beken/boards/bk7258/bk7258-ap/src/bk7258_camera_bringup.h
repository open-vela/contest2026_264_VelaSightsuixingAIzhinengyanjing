/****************************************************************************
 * board/beken/boards/bk7258/bk7258-ap/src/bk7258_camera_bringup.h
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef __BOARD_BEKEN_BOARDS_BK7258_BK7258_AP_SRC_BK7258_CAMERA_BRINGUP_H
#define __BOARD_BEKEN_BOARDS_BK7258_BK7258_AP_SRC_BK7258_CAMERA_BRINGUP_H

/****************************************************************************
 * Name: bk7258_camera_initialize
 *
 * Description:
 *   Wires the BK7258 GC2145 imgdata (platform) and imgsensor (device)
 *   halves together and registers them as /dev/video0 via
 *   capture_register(), so standard V4L2 tooling (cameratool, nxcamera)
 *   can drive the camera.
 *
 * Returned Value:
 *   OK on success, negated errno on failure (mirroring
 *   capture_register()'s return convention).
 *
 ****************************************************************************/

int bk7258_camera_initialize(void);

#endif /* __BOARD_BEKEN_BOARDS_BK7258_BK7258_AP_SRC_BK7258_CAMERA_BRINGUP_H */
