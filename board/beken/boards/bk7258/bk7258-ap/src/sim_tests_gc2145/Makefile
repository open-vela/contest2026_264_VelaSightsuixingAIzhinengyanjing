# board/beken/boards/bk7258/bk7258-ap/sim_tests_gc2145/Makefile
#
# Desktop simulation integration test for bk7258_gc2145.c.  Reuses the
# mock register/IRQ infrastructure (sim_include/, sim_stubs.c) from
# board/beken/chips/bk7258/sim_tests/, plus the real production driver
# sources from both the chip layer (bk7258_gpio.c, bk7258_i2c_sim.c,
# bk7258_dma.c, bk7258_yuv_buf.c) and this board layer (bk7258_gc2145.c),
# all compiled completely unmodified.
#
# Usage:
#   make            # build and run the test
#   make clean      # remove build artifacts
#
# SPDX-License-Identifier: Apache-2.0

CC      := gcc
CFLAGS  := -std=c11 -Wall -Wextra -g -O0

CHIP_DIR       := $(CURDIR)/../../../../../chips/bk7258
CHIP_SIM_DIR   := $(CHIP_DIR)/sim_tests

CFLAGS  += -I$(CHIP_SIM_DIR)/sim_include
CFLAGS  += -I$(CHIP_SIM_DIR)
CFLAGS  += -I$(CHIP_DIR)
CFLAGS  += -I$(CHIP_DIR)/include
CFLAGS  += -I$(CHIP_DIR)/hardware

BUILD_DIR := build

STUBS_SRC := $(CHIP_SIM_DIR)/sim_stubs.c

DRIVER_SRCS := \
	$(CHIP_DIR)/bk7258_gpio.c \
	$(CHIP_DIR)/bk7258_i2c1.c \
	$(CHIP_DIR)/bk7258_dma.c \
	$(CHIP_DIR)/bk7258_yuv_buf.c \
	../bk7258_gc2145.c

.PHONY: all clean test_gc2145

all: test_gc2145

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

test_gc2145: $(BUILD_DIR)/test_gc2145
	@echo "=== Running test_gc2145 ==="
	@$(BUILD_DIR)/test_gc2145

$(BUILD_DIR)/test_gc2145: test_gc2145.c $(STUBS_SRC) $(DRIVER_SRCS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $@ $^

clean:
	rm -rf $(BUILD_DIR)
