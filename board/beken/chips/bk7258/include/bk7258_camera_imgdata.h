/****************************************************************************
 * board/beken/chips/bk7258/include/bk7258_camera_imgdata.h
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef __VENDOR_BEKEN_CHIPS_BK7258_INCLUDE_BK7258_CAMERA_IMGDATA_H
#define __VENDOR_BEKEN_CHIPS_BK7258_INCLUDE_BK7258_CAMERA_IMGDATA_H

#include <nuttx/video/imgdata.h>

/****************************************************************************
 * Name: bk7258_camera_imgdata_initialize
 *
 * Description:
 *   Returns the singleton imgdata_s instance implementing the BK7258
 *   YUV_BUF/DMA capture platform driver (imgdata_ops_s), for use with
 *   capture_register().  Does not itself touch hardware; the returned
 *   instance's ops->init() is what performs YUV_BUF/DMA controller
 *   initialization, invoked by the V4L2 core when the /dev/videoN node
 *   this is registered under is opened.
 *
 ****************************************************************************/

FAR struct imgdata_s *bk7258_camera_imgdata_initialize(void);

#endif /* __VENDOR_BEKEN_CHIPS_BK7258_INCLUDE_BK7258_CAMERA_IMGDATA_H */
