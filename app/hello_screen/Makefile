############################################################################
# Pen-stroke boot greeting - Makefile
############################################################################

include $(APPDIR)/Make.defs

PROGNAME  = hello
PRIORITY  = SCHED_PRIORITY_DEFAULT
STACKSIZE = 4096
MODULE    = $(CONFIG_LVX_USE_DEMO_CONTEST2026_264_HELLO_SCREEN)

CSRCS   = hello_paint.c hello_font.c
MAINSRC = hello_screen_main.c

include $(APPDIR)/Application.mk
