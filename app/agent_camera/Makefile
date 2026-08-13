############################################################################
# ai_agent camera-path bench - Makefile
############################################################################

include $(APPDIR)/Make.defs

PROGNAME  = agent_camera
PRIORITY  = SCHED_PRIORITY_DEFAULT

# Only V4L2 ioctls and a marker check; no conversion and no staging buffer.

STACKSIZE = 4096
MODULE    = $(CONFIG_LVX_USE_DEMO_CONTEST2026_264_AGENT_CAMERA)

MAINSRC = agent_camera_main.c

include $(APPDIR)/Application.mk
