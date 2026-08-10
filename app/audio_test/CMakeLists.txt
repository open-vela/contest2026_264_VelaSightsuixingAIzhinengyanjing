# BK7258 internal-DAC audio bring-up test

if(CONFIG_LVX_USE_DEMO_CONTEST2026_264_AUDIO_TEST)
  nuttx_add_application(
    NAME audio_test
    SRCS audio_test_main.c
    STACKSIZE 4096)
endif()
