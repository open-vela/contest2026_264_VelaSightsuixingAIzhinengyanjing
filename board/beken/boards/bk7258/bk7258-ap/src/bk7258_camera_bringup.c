/****************************************************************************
 * board/beken/boards/bk7258/bk7258-ap/src/bk7258_camera_bringup.c
 *
 * Registration glue: wires the imgdata (bk7258_camera_imgdata_initialize(),
 * chip-layer) and imgsensor (bk7258_camera_imgsensor_initialize(),
 * board-layer) halves together and registers them under /dev/video0 via
 * the standard V4L2 capture_register() API.
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#include <nuttx/config.h>

#include <stddef.h>
#include <stdio.h>

#include <nuttx/video/imgdata.h>
#include <nuttx/video/imgsensor.h>
#include <nuttx/video/v4l2_cap.h>

#include "bk7258_camera_imgdata.h"
#include "bk7258_camera_bringup.h"

FAR struct imgsensor_s *bk7258_camera_imgsensor_initialize(void);

int bk7258_camera_initialize(void)
{
  FAR struct imgdata_s *imgdata;
  FAR struct imgsensor_s *imgsensor;
  FAR struct imgsensor_s *sensors[1];
  int ret;

  imgdata = bk7258_camera_imgdata_initialize();
  imgsensor = bk7258_camera_imgsensor_initialize();

  sensors[0] = imgsensor;

  ret = capture_register("/dev/video0", imgdata, sensors, 1);
  printf("bk7258_camera_bringup: capture_register(\"/dev/video0\") "
         "returned %d\n", ret);

  return ret;
}
