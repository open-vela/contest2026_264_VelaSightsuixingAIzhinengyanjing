set(SRCS
    bk7258_boot.c
    bk7258_bringup.c
    bk7258_buttons.c
    bk7258_motor.c
    bk7258_appinit.c
    bk7258_gc9d01.c
    bk7258_gc2145.c
    bk7258_camera_imgsensor.c
    bk7258_camera_bringup.c)

target_sources(board PRIVATE ${SRCS})
target_include_directories(board PRIVATE
  ${NUTTX_BOARD_ABS_DIR}/include
  ${NUTTX_CHIP_ABS_DIR}
  ${NUTTX_CHIP_ABS_DIR}/include
  ${NUTTX_CHIP_ABS_DIR}/hardware)
