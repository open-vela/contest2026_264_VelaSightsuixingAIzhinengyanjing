# BK7258 UART Ctrl-C test
if(CONFIG_LVX_USE_DEMO_CONTEST2026_264_CTRLC_TEST)
  nuttx_add_application(
    NAME ctrlc_test
    SRCS hello_app_main.c
    STACKSIZE 2048)
endif()
