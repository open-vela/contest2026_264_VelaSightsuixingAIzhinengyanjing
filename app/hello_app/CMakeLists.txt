# BK7258 peripheral self-test.
#
# Transitional: this directory is the contest scaffold's `hello_app`, and it
# still hosts periph_selftest only because moving that file is a change to
# somebody else's work and goes in its own commit.  See app/periph_selftest/.
if(CONFIG_LVX_USE_DEMO_CONTEST2026_264_PERIPH_SELFTEST)
  nuttx_add_application(
    NAME periph_selftest
    SRCS periph_selftest_main.c
    STACKSIZE 4096)
endif()
