# BK7258 V4L2 M2M JPEG encoder test
if(CONFIG_LVX_USE_DEMO_CONTEST2026_264_JPEG_TEST)
  nuttx_add_application(
    NAME jpeg_test
    SRCS jpeg_test_main.c
    STACKSIZE 4096)
endif()
