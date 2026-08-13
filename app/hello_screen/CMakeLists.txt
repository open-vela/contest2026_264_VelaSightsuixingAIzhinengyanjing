# Pen-stroke boot greeting ("hello" written the way Apple's boot screen does)
if(CONFIG_LVX_USE_DEMO_CONTEST2026_264_HELLO_SCREEN)
  nuttx_add_application(
    NAME hello
    SRCS hello_screen_main.c
    STACKSIZE 4096)
endif()
