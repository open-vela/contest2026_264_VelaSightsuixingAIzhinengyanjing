# SPDX-License-Identifier: Apache-2.0

include $(TOPDIR)/Make.defs

CFLAGS += -I$(TOPDIR)/arch/arm/src/chip/include
CFLAGS += -I$(TOPDIR)/arch/arm/src/chip/hardware

CSRCS = bk7258_boot.c
CSRCS += bk7258_bringup.c
CSRCS += bk7258_buttons.c
CSRCS += bk7258_motor.c
CSRCS += bk7258_appinit.c
CSRCS += bk7258_leds.c
CSRCS += bk7258_gc9d01.c
CSRCS += bk7258_gc2145.c
CSRCS += bk7258_camera_imgsensor.c
CSRCS += bk7258_camera_bringup.c

include $(TOPDIR)/boards/Board.mk
