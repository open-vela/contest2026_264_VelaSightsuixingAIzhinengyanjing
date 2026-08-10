set(SRCS
    bk7258_boot.c
    bk7258_bringup.c
    bk7258_buttons.c
    bk7258_leds.c
    bk7258_motor.c
    bk7258_appinit.c
    bk7258_gc9d01.c
    bk7258_ramdisk.c
    bk7258_camera_imgsensor.c
    bk7258_camera_bringup.c)

if(CONFIG_BK7258_AUDIO)
  list(APPEND SRCS bk7258_audio_bringup.c bk7258_audio_dev.c)
endif()

if(CONFIG_BK7258_GC9D01_FB)
  list(APPEND SRCS bk7258_gc9d01_fb.c)
endif()

target_sources(board PRIVATE ${SRCS})
target_include_directories(board PRIVATE
  ${NUTTX_BOARD_ABS_DIR}/include
  ${NUTTX_CHIP_ABS_DIR}
  ${NUTTX_CHIP_ABS_DIR}/include
  ${NUTTX_CHIP_ABS_DIR}/hardware)
