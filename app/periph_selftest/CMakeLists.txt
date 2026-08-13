# BK7258 peripheral self-test
if(CONFIG_LVX_USE_DEMO_CONTEST2026_264_PERIPH_SELFTEST)
  nuttx_add_application(
    NAME periph_selftest
    SRCS periph_selftest_main.c
    STACKSIZE 4096)
endif()
