/****************************************************************************
 * board/beken/boards/bk7258/bk7258-ap/src/bk7258_camera_imgsensor.c
 *
 * GC2145 sensor-side (imgsensor) driver: standard NuttX V4L2
 * imgsensor_ops_s implementation.  This is the "sensor device layer"
 * half of the driver split described in
 * docs/zh-cn/device_dev_guide/media/camera/Camera_Driver.md -- it owns
 * I2C register programming, power/reset sequencing, and DVP pinmux; the
 * platform (DMA/YUV_BUF/full-frame-assembly) half lives in
 * board/beken/chips/bk7258/bk7258_camera_imgdata.c.
 *
 * Register tables (g_gc2145_init_regs: 585 entries, g_gc2145_640_480_regs:
 * 40 entries) and the power/reset/pinmux GPIO assignments below are
 * copied verbatim from this repo's earlier bring-up driver
 * (bk7258_gc2145.c), which in turn ported them from bk_avdk_smp
 * release/v3.1.1 ap/components/bk_peripheral/src/dvp/dvp_gc2145.c and
 * validated the power-enable/reset-pin/I2C-bus schematic corrections
 * against AIDK_AI玩具开发板_原理图.pdf (see bk7258_gc2145.c's file header
 * and DVP_RESET_PIN/DVP_POWER_PIN comments for the full evidence chain --
 * not repeated here to avoid duplicating that history; this file only
 * repeats the resulting pin numbers and table contents, not the
 * discovery narrative).  Only the resulting register content and pin
 * assignments are ported here; that older file is kept as an
 * independent NSH smoke-test entry point (see bk7258_bringup.c) rather
 * than being deleted or having its logic shared via a common helper,
 * since it intentionally does not depend on the V4L2/imgdata/imgsensor
 * framework and should keep working even if this driver's registration
 * with capture_register() fails for framework-level reasons.
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#include <nuttx/config.h>
#include <nuttx/arch.h>

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <errno.h>

#include <nuttx/video/imgsensor.h>
#include <sys/videoio.h>

#include "arm_internal.h"
#include "bk7258_gpio.h"
#include "bk7258_gc2145_i2c_bitbang.h"
#include "hardware/bk7258_gpio.h"
#include <nuttx/clock.h>

#define GC2145_I2C_ADDR      0x3Cu

/* DVP_MCLK_PIN's pinmux function index -- see this file's
 * bk7258_gc2145_mclk_enable() comment for the full evidence chain on
 * why this is 1 (GPIO_DEV_CLK_AUXS_CIS), not 0
 * (GPIO_DEV_JPEG_MCLK/DVP_PINMUX_FUNCTION as used for the other DVP
 * pins below). */
#define DVP_MCLK_PINMUX_FUNCTION  1u

#define DVP_MCLK_PIN         27u
#define DVP_PCLK_PIN         29u
#define DVP_HSYNC_PIN        30u
#define DVP_VSYNC_PIN        31u
#define DVP_DATA_PIN(i)      (32u + (i))
#define DVP_PINMUX_FUNCTION  0u

#define DVP_RESET_PIN        28u
#define DVP_POWER_PIN        49u

/* System clock controller (SOC_SYS_REG_BASE) register offsets/fields
 * for the AUXS_CIS clock path that actually drives the GC2145's MCLK
 * input pin (GPIO27) -- ported from bk_avdk_smp's authoritative
 * register layout (ap/middleware/soc/bk7258_ap/soc/sys_struct.h,
 * ap/middleware/soc/bk7258_ap/hal/sys_ll.h) and its
 * dvp_camera_mclk_enable() (ap/components/bk_dvp/src/dvp_common.c)
 * reference sequence.
 *
 * ROOT CAUSE (see this file's header comment for the full account of
 * how this was found): this repo's DVP pinmux setup only ever selected
 * GPIO27's function index 0 (GPIO_DEV_JPEG_MCLK) and never touched any
 * system-clock register, so GPIO27 was never actually driven with a
 * real MCLK waveform -- it was left as whatever default/floating state
 * that pinmux selection produces, with no oscillating clock output.
 * GC2145's datasheet (GC2145 CSP Datasheet release V1.0, section 7.2.1
 * "Power On Sequence") lists "t3: From AVDD to MCLK applied" and
 * "t4: From MCLK applied to Sensor enable" as mandatory power-up
 * stages before the sensor's internal logic (including its SCCB/I2C
 * slave interface) becomes responsive -- without a real MCLK input,
 * the sensor's I2C bus never comes alive, which fully explains why
 * every I2C write NACKed on the very first (address) byte regardless
 * of how correct this driver's bit-bang timing/protocol/pin-assignment
 * turned out to be under exhaustive cross-checking. The reference
 * implementation's dvp_camera_mclk_enable() (called unconditionally by
 * bk_dvp_detect() before any I2C traffic) is the only place in
 * bk_avdk_smp that actually starts this clock, via:
 *   1. gpio_dev_map(GPIO_27, GPIO_DEV_CLK_AUXS_CIS) -- pinmux function
 *      index 1 in GPIO27's per-pin function array
 *      (bk_avdk_smp/ap/middleware/soc/bk7258_ap/soc/gpio_map.h's
 *      {GPIO_DEV_JPEG_MCLK, GPIO_DEV_CLK_AUXS_CIS, ...} list), NOT
 *      index 0 as this driver's other DVP pins correctly use.
 *   2. sys_drv_set_auxs_cis(cksel, ckdiv) -- selects clock source and
 *      divider (default MCLK_24M case: cksel=3, ckdiv=19).
 *   3. sys_drv_set_cis_auxs_clk_en(1) -- the actual clock-gate enable
 *      bit, without which steps 1-2 configure the divider but never
 *      let the clock signal reach the pin.
 */
#define BK7258_SYS_REG_BASE            0x44010000u

/* reg0x0a (SOC_SYS_REG_BASE + 0xa*4): cksel_auxs_cis at bit[15:16],
 * ckdiv_auxs_cis at bit[17:21]. */
#define BK7258_SYS_REG_0X0A            (BK7258_SYS_REG_BASE + (0xau << 2))
#define BK7258_SYS_CKSEL_AUXS_CIS_SHIFT 15u
#define BK7258_SYS_CKSEL_AUXS_CIS_MASK  (0x3u << BK7258_SYS_CKSEL_AUXS_CIS_SHIFT)
#define BK7258_SYS_CKDIV_AUXS_CIS_SHIFT 17u
#define BK7258_SYS_CKDIV_AUXS_CIS_MASK  (0x1fu << BK7258_SYS_CKDIV_AUXS_CIS_SHIFT)

/* reg0x0d (SOC_SYS_REG_BASE + 0xd*4): cis_auxs_cken at bit[9]. */
#define BK7258_SYS_REG_0X0D            (BK7258_SYS_REG_BASE + (0xdu << 2))
#define BK7258_SYS_CIS_AUXS_CKEN       (1u << 9)

/* MCLK_24M case from dvp_camera_mclk_enable(): cksel=3, ckdiv=19.
 * Matches this repo's other DVP timing choices (no runtime MCLK
 * frequency selection is implemented; GC2145's init register table
 * was ported assuming this same 24MHz convention the reference SDK
 * itself defaults to). */
#define GC2145_MCLK_CKSEL              3u
#define GC2145_MCLK_CKDIV              19u

#define GC2145_CAPTURE_WIDTH   640u
#define GC2145_CAPTURE_HEIGHT  480u

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct gc2145_reg
{
  uint8_t reg;
  uint8_t value;
};

static const struct gc2145_reg g_gc2145_init_regs[] =
{
  { 0xFE, 0xF0 },
  { 0xFE, 0xF0 },
  { 0xFE, 0xF0 },
  { 0xFC, 0x06 },
  { 0xF6, 0x00 },
  { 0xF7, 0x1D },
  { 0xF8, 0x84 },
  { 0xFA, 0x00 },
  { 0xF9, 0xFE },
  { 0xF2, 0x00 },
  { 0xFE, 0x00 },
  { 0x03, 0x04 },
  { 0x04, 0xE2 },
  { 0x09, 0x00 },
  { 0x0A, 0x00 },
  { 0x0B, 0x00 },
  { 0x0C, 0x00 },
  { 0x0D, 0x04 },
  { 0x0E, 0xC0 },
  { 0x0F, 0x06 },
  { 0x10, 0x52 },
  { 0x12, 0x2E },
  { 0x17, 0x14 },
  { 0x18, 0x22 },
  { 0x19, 0x0E },
  { 0x1A, 0x01 },
  { 0x1B, 0x4B },
  { 0x1C, 0x07 },
  { 0x1D, 0x10 },
  { 0x1E, 0x88 },
  { 0x1F, 0x78 },
  { 0x20, 0x03 },
  { 0x21, 0x40 },
  { 0x22, 0xA0 },
  { 0x24, 0x16 },
  { 0x25, 0x01 },
  { 0x26, 0x10 },
  { 0x2D, 0x60 },
  { 0x30, 0x01 },
  { 0x31, 0x90 },
  { 0x33, 0x06 },
  { 0x34, 0x01 },
  { 0xFE, 0x00 },
  { 0x80, 0x7F },
  { 0x81, 0x26 },
  { 0x82, 0xFA },
  { 0x83, 0x00 },
  { 0x84, 0x02 },
  { 0x86, 0x03 },
  { 0x88, 0x03 },
  { 0x89, 0x03 },
  { 0x85, 0x08 },
  { 0x8A, 0x00 },
  { 0x8B, 0x00 },
  { 0xB0, 0x55 },
  { 0xC3, 0x00 },
  { 0xC4, 0x80 },
  { 0xC5, 0x90 },
  { 0xC6, 0x3B },
  { 0xC7, 0x46 },
  { 0xEC, 0x06 },
  { 0xED, 0x04 },
  { 0xEE, 0x60 },
  { 0xEF, 0x90 },
  { 0xB6, 0x01 },
  { 0x90, 0x01 },
  { 0x91, 0x00 },
  { 0x92, 0x00 },
  { 0x93, 0x00 },
  { 0x94, 0x00 },
  { 0x95, 0x04 },
  { 0x96, 0xB0 },
  { 0x97, 0x06 },
  { 0x98, 0x40 },
  { 0xFE, 0x00 },
  { 0x40, 0x42 },
  { 0x41, 0x00 },
  { 0x43, 0x5B },
  { 0x5E, 0x00 },
  { 0x5F, 0x00 },
  { 0x60, 0x00 },
  { 0x61, 0x00 },
  { 0x62, 0x00 },
  { 0x63, 0x00 },
  { 0x64, 0x00 },
  { 0x65, 0x00 },
  { 0x66, 0x20 },
  { 0x67, 0x20 },
  { 0x68, 0x20 },
  { 0x69, 0x20 },
  { 0x76, 0x00 },
  { 0x6A, 0x08 },
  { 0x6B, 0x08 },
  { 0x6C, 0x08 },
  { 0x6D, 0x08 },
  { 0x6E, 0x08 },
  { 0x6F, 0x08 },
  { 0x70, 0x08 },
  { 0x71, 0x08 },
  { 0x76, 0x00 },
  { 0x72, 0xF0 },
  { 0x7E, 0x3C },
  { 0x7F, 0x00 },
  { 0xFE, 0x02 },
  { 0x48, 0x15 },
  { 0x49, 0x00 },
  { 0x4B, 0x0B },
  { 0xFE, 0x00 },
  { 0xFE, 0x01 },
  { 0x01, 0x04 },
  { 0x02, 0xC0 },
  { 0x03, 0x04 },
  { 0x04, 0x90 },
  { 0x05, 0x30 },
  { 0x06, 0x90 },
  { 0x07, 0x30 },
  { 0x08, 0x80 },
  { 0x09, 0x00 },
  { 0x0A, 0x82 },
  { 0x0B, 0x11 },
  { 0x0C, 0x10 },
  { 0x11, 0x10 },
  { 0x13, 0x7B },
  { 0x17, 0x00 },
  { 0x1C, 0x11 },
  { 0x1E, 0x61 },
  { 0x1F, 0x35 },
  { 0x20, 0x40 },
  { 0x22, 0x40 },
  { 0x23, 0x20 },
  { 0xFE, 0x02 },
  { 0x0F, 0x04 },
  { 0xFE, 0x01 },
  { 0x12, 0x35 },
  { 0x15, 0xB0 },
  { 0x10, 0x31 },
  { 0x3E, 0x28 },
  { 0x3F, 0xB0 },
  { 0x40, 0x90 },
  { 0x41, 0x0F },
  { 0xFE, 0x02 },
  { 0x90, 0x6C },
  { 0x91, 0x03 },
  { 0x92, 0xCB },
  { 0x94, 0x33 },
  { 0x95, 0x84 },
  { 0x97, 0x65 },
  { 0xA2, 0x11 },
  { 0xFE, 0x00 },
  { 0xFE, 0x02 },
  { 0x80, 0xC1 },
  { 0x81, 0x08 },
  { 0x82, 0x05 },
  { 0x83, 0x08 },
  { 0x84, 0x0A },
  { 0x86, 0xF0 },
  { 0x87, 0x50 },
  { 0x88, 0x15 },
  { 0x89, 0xB0 },
  { 0x8A, 0x30 },
  { 0x8B, 0x10 },
  { 0xFE, 0x01 },
  { 0x21, 0x04 },
  { 0xFE, 0x02 },
  { 0xA3, 0x50 },
  { 0xA4, 0x20 },
  { 0xA5, 0x40 },
  { 0xA6, 0x80 },
  { 0xAB, 0x40 },
  { 0xAE, 0x0C },
  { 0xB3, 0x46 },
  { 0xB4, 0x64 },
  { 0xB6, 0x38 },
  { 0xB7, 0x01 },
  { 0xB9, 0x2B },
  { 0x3C, 0x04 },
  { 0x3D, 0x15 },
  { 0x4B, 0x06 },
  { 0x4C, 0x20 },
  { 0xFE, 0x00 },
  { 0xFE, 0x02 },
  { 0x10, 0x09 },
  { 0x11, 0x0D },
  { 0x12, 0x13 },
  { 0x13, 0x19 },
  { 0x14, 0x27 },
  { 0x15, 0x37 },
  { 0x16, 0x45 },
  { 0x17, 0x53 },
  { 0x18, 0x69 },
  { 0x19, 0x7D },
  { 0x1A, 0x8F },
  { 0x1B, 0x9D },
  { 0x1C, 0xA9 },
  { 0x1D, 0xBD },
  { 0x1E, 0xCD },
  { 0x1F, 0xD9 },
  { 0x20, 0xE3 },
  { 0x21, 0xEA },
  { 0x22, 0xEF },
  { 0x23, 0xF5 },
  { 0x24, 0xF9 },
  { 0x25, 0xFF },
  { 0xFE, 0x00 },
  { 0xC6, 0x20 },
  { 0xC7, 0x2B },
  { 0xFE, 0x02 },
  { 0x26, 0x0F },
  { 0x27, 0x14 },
  { 0x28, 0x19 },
  { 0x29, 0x1E },
  { 0x2A, 0x27 },
  { 0x2B, 0x33 },
  { 0x2C, 0x3B },
  { 0x2D, 0x45 },
  { 0x2E, 0x59 },
  { 0x2F, 0x69 },
  { 0x30, 0x7C },
  { 0x31, 0x89 },
  { 0x32, 0x98 },
  { 0x33, 0xAE },
  { 0x34, 0xC0 },
  { 0x35, 0xCF },
  { 0x36, 0xDA },
  { 0x37, 0xE2 },
  { 0x38, 0xE9 },
  { 0x39, 0xF3 },
  { 0x3A, 0xF9 },
  { 0x3B, 0xFF },
  { 0xFE, 0x02 },
  { 0xD1, 0x32 },
  { 0xD2, 0x32 },
  { 0xD3, 0x40 },
  { 0xD6, 0xF0 },
  { 0xD7, 0x10 },
  { 0xD8, 0xDA },
  { 0xDD, 0x14 },
  { 0xDE, 0x86 },
  { 0xED, 0x80 },
  { 0xEE, 0x00 },
  { 0xEF, 0x3F },
  { 0xD8, 0xD8 },
  { 0xFE, 0x01 },
  { 0x9F, 0x40 },
  { 0xFE, 0x01 },
  { 0xC2, 0x14 },
  { 0xC3, 0x0D },
  { 0xC4, 0x0C },
  { 0xC8, 0x15 },
  { 0xC9, 0x0D },
  { 0xCA, 0x0A },
  { 0xBC, 0x24 },
  { 0xBD, 0x10 },
  { 0xBE, 0x0B },
  { 0xB6, 0x25 },
  { 0xB7, 0x16 },
  { 0xB8, 0x15 },
  { 0xC5, 0x00 },
  { 0xC6, 0x00 },
  { 0xC7, 0x00 },
  { 0xCB, 0x00 },
  { 0xCC, 0x00 },
  { 0xCD, 0x00 },
  { 0xBF, 0x07 },
  { 0xC0, 0x00 },
  { 0xC1, 0x00 },
  { 0xB9, 0x00 },
  { 0xBA, 0x00 },
  { 0xBB, 0x00 },
  { 0xAA, 0x01 },
  { 0xAB, 0x01 },
  { 0xAC, 0x00 },
  { 0xAD, 0x05 },
  { 0xAE, 0x06 },
  { 0xAF, 0x0E },
  { 0xB0, 0x0B },
  { 0xB1, 0x07 },
  { 0xB2, 0x06 },
  { 0xB3, 0x17 },
  { 0xB4, 0x0E },
  { 0xB5, 0x0E },
  { 0xD0, 0x09 },
  { 0xD1, 0x00 },
  { 0xD2, 0x00 },
  { 0xD6, 0x08 },
  { 0xD7, 0x00 },
  { 0xD8, 0x00 },
  { 0xD9, 0x00 },
  { 0xDA, 0x00 },
  { 0xDB, 0x00 },
  { 0xD3, 0x0A },
  { 0xD4, 0x00 },
  { 0xD5, 0x00 },
  { 0xA4, 0x00 },
  { 0xA5, 0x00 },
  { 0xA6, 0x77 },
  { 0xA7, 0x77 },
  { 0xA8, 0x77 },
  { 0xA9, 0x77 },
  { 0xA1, 0x80 },
  { 0xA2, 0x80 },
  { 0xFE, 0x01 },
  { 0xDF, 0x0D },
  { 0xDC, 0x25 },
  { 0xDD, 0x30 },
  { 0xE0, 0x77 },
  { 0xE1, 0x80 },
  { 0xE2, 0x77 },
  { 0xE3, 0x90 },
  { 0xE6, 0x90 },
  { 0xE7, 0xA0 },
  { 0xE8, 0x90 },
  { 0xE9, 0xA0 },
  { 0xFE, 0x00 },
  { 0xFE, 0x01 },
  { 0x4F, 0x00 },
  { 0x4F, 0x00 },
  { 0x4B, 0x01 },
  { 0x4F, 0x00 },
  { 0x4C, 0x01 },
  { 0x4D, 0x71 },
  { 0x4E, 0x01 },
  { 0x4C, 0x01 },
  { 0x4D, 0x91 },
  { 0x4E, 0x01 },
  { 0x4C, 0x01 },
  { 0x4D, 0x70 },
  { 0x4E, 0x01 },
  { 0x4C, 0x01 },
  { 0x4D, 0x90 },
  { 0x4E, 0x02 },
  { 0x4C, 0x01 },
  { 0x4D, 0xB0 },
  { 0x4E, 0x02 },
  { 0x4C, 0x01 },
  { 0x4D, 0x8F },
  { 0x4E, 0x02 },
  { 0x4C, 0x01 },
  { 0x4D, 0x6F },
  { 0x4E, 0x02 },
  { 0x4C, 0x01 },
  { 0x4D, 0xAF },
  { 0x4E, 0x02 },
  { 0x4C, 0x01 },
  { 0x4D, 0xD0 },
  { 0x4E, 0x02 },
  { 0x4C, 0x01 },
  { 0x4D, 0xF0 },
  { 0x4E, 0x02 },
  { 0x4C, 0x01 },
  { 0x4D, 0xCF },
  { 0x4E, 0x02 },
  { 0x4C, 0x01 },
  { 0x4D, 0xEF },
  { 0x4E, 0x02 },
  { 0x4C, 0x01 },
  { 0x4D, 0x6E },
  { 0x4E, 0x03 },
  { 0x4C, 0x01 },
  { 0x4D, 0x8E },
  { 0x4E, 0x03 },
  { 0x4C, 0x01 },
  { 0x4D, 0xAE },
  { 0x4E, 0x03 },
  { 0x4C, 0x01 },
  { 0x4D, 0xCE },
  { 0x4E, 0x03 },
  { 0x4C, 0x01 },
  { 0x4D, 0x4D },
  { 0x4E, 0x03 },
  { 0x4C, 0x01 },
  { 0x4D, 0x6D },
  { 0x4E, 0x03 },
  { 0x4C, 0x01 },
  { 0x4D, 0x8D },
  { 0x4E, 0x03 },
  { 0x4C, 0x01 },
  { 0x4D, 0xAD },
  { 0x4E, 0x03 },
  { 0x4C, 0x01 },
  { 0x4D, 0xCD },
  { 0x4E, 0x03 },
  { 0x4C, 0x01 },
  { 0x4D, 0x4C },
  { 0x4E, 0x03 },
  { 0x4C, 0x01 },
  { 0x4D, 0x6C },
  { 0x4E, 0x03 },
  { 0x4C, 0x01 },
  { 0x4D, 0x8C },
  { 0x4E, 0x03 },
  { 0x4C, 0x01 },
  { 0x4D, 0xAC },
  { 0x4E, 0x03 },
  { 0x4C, 0x01 },
  { 0x4D, 0xCC },
  { 0x4E, 0x03 },
  { 0x4C, 0x01 },
  { 0x4D, 0xCB },
  { 0x4E, 0x03 },
  { 0x4C, 0x01 },
  { 0x4D, 0x4B },
  { 0x4E, 0x03 },
  { 0x4C, 0x01 },
  { 0x4D, 0x6B },
  { 0x4E, 0x03 },
  { 0x4C, 0x01 },
  { 0x4D, 0x8B },
  { 0x4E, 0x03 },
  { 0x4C, 0x01 },
  { 0x4D, 0xAB },
  { 0x4E, 0x03 },
  { 0x4C, 0x01 },
  { 0x4D, 0x8A },
  { 0x4E, 0x04 },
  { 0x4C, 0x01 },
  { 0x4D, 0xAA },
  { 0x4E, 0x04 },
  { 0x4C, 0x01 },
  { 0x4D, 0xCA },
  { 0x4E, 0x04 },
  { 0x4C, 0x01 },
  { 0x4D, 0xCA },
  { 0x4E, 0x04 },
  { 0x4C, 0x01 },
  { 0x4D, 0xC9 },
  { 0x4E, 0x04 },
  { 0x4C, 0x01 },
  { 0x4D, 0x8A },
  { 0x4E, 0x04 },
  { 0x4C, 0x01 },
  { 0x4D, 0x89 },
  { 0x4E, 0x04 },
  { 0x4C, 0x01 },
  { 0x4D, 0xA9 },
  { 0x4E, 0x04 },
  { 0x4C, 0x02 },
  { 0x4D, 0x0B },
  { 0x4E, 0x05 },
  { 0x4C, 0x02 },
  { 0x4D, 0x0A },
  { 0x4E, 0x05 },
  { 0x4C, 0x01 },
  { 0x4D, 0xEB },
  { 0x4E, 0x05 },
  { 0x4C, 0x01 },
  { 0x4D, 0xEA },
  { 0x4E, 0x05 },
  { 0x4C, 0x02 },
  { 0x4D, 0x09 },
  { 0x4E, 0x05 },
  { 0x4C, 0x02 },
  { 0x4D, 0x29 },
  { 0x4E, 0x05 },
  { 0x4C, 0x02 },
  { 0x4D, 0x2A },
  { 0x4E, 0x05 },
  { 0x4C, 0x02 },
  { 0x4D, 0x4A },
  { 0x4E, 0x05 },
  { 0x4C, 0x02 },
  { 0x4D, 0x8A },
  { 0x4E, 0x06 },
  { 0x4C, 0x02 },
  { 0x4D, 0x49 },
  { 0x4E, 0x06 },
  { 0x4C, 0x02 },
  { 0x4D, 0x69 },
  { 0x4E, 0x06 },
  { 0x4C, 0x02 },
  { 0x4D, 0x89 },
  { 0x4E, 0x06 },
  { 0x4C, 0x02 },
  { 0x4D, 0xA9 },
  { 0x4E, 0x06 },
  { 0x4C, 0x02 },
  { 0x4D, 0x48 },
  { 0x4E, 0x06 },
  { 0x4C, 0x02 },
  { 0x4D, 0x68 },
  { 0x4E, 0x06 },
  { 0x4C, 0x02 },
  { 0x4D, 0x69 },
  { 0x4E, 0x06 },
  { 0x4C, 0x02 },
  { 0x4D, 0xCA },
  { 0x4E, 0x07 },
  { 0x4C, 0x02 },
  { 0x4D, 0xC9 },
  { 0x4E, 0x07 },
  { 0x4C, 0x02 },
  { 0x4D, 0xE9 },
  { 0x4E, 0x07 },
  { 0x4C, 0x03 },
  { 0x4D, 0x09 },
  { 0x4E, 0x07 },
  { 0x4C, 0x02 },
  { 0x4D, 0xC8 },
  { 0x4E, 0x07 },
  { 0x4C, 0x02 },
  { 0x4D, 0xE8 },
  { 0x4E, 0x07 },
  { 0x4C, 0x02 },
  { 0x4D, 0xA7 },
  { 0x4E, 0x07 },
  { 0x4C, 0x02 },
  { 0x4D, 0xC7 },
  { 0x4E, 0x07 },
  { 0x4C, 0x02 },
  { 0x4D, 0xE7 },
  { 0x4E, 0x07 },
  { 0x4C, 0x03 },
  { 0x4D, 0x07 },
  { 0x4E, 0x07 },
  { 0x4F, 0x01 },
  { 0x50, 0x80 },
  { 0x51, 0xA8 },
  { 0x52, 0x47 },
  { 0x53, 0x38 },
  { 0x54, 0xC7 },
  { 0x56, 0x0E },
  { 0x58, 0x08 },
  { 0x5B, 0x00 },
  { 0x5C, 0x74 },
  { 0x5D, 0x8B },
  { 0x61, 0xDB },
  { 0x62, 0xB8 },
  { 0x63, 0x86 },
  { 0x64, 0xC0 },
  { 0x65, 0x04 },
  { 0x67, 0xA8 },
  { 0x68, 0xB0 },
  { 0x69, 0x00 },
  { 0x6A, 0xA8 },
  { 0x6B, 0xB0 },
  { 0x6C, 0xAF },
  { 0x6D, 0x8B },
  { 0x6E, 0x50 },
  { 0x6F, 0x18 },
  { 0x73, 0xF0 },
  { 0x70, 0x0D },
  { 0x71, 0x60 },
  { 0x72, 0x80 },
  { 0x74, 0x01 },
  { 0x75, 0x01 },
  { 0x7F, 0x0C },
  { 0x76, 0x70 },
  { 0x77, 0x58 },
  { 0x78, 0xA0 },
  { 0x79, 0x5E },
  { 0x7A, 0x54 },
  { 0x7B, 0x58 },
  { 0xFE, 0x00 },
  { 0xFE, 0x02 },
  { 0xC0, 0x01 },
  { 0xC1, 0x44 },
  { 0xC2, 0xFD },
  { 0xC3, 0x04 },
  { 0xC4, 0xF0 },
  { 0xC5, 0x48 },
  { 0xC6, 0xFD },
  { 0xC7, 0x46 },
  { 0xC8, 0xFD },
  { 0xC9, 0x02 },
  { 0xCA, 0xE0 },
  { 0xCB, 0x45 },
  { 0xCC, 0xEC },
  { 0xCD, 0x48 },
  { 0xCE, 0xF0 },
  { 0xCF, 0xF0 },
  { 0xE3, 0x0C },
  { 0xE4, 0x4B },
  { 0xE5, 0xE0 },
  { 0xFE, 0x01 },
  { 0x9F, 0x40 },
  { 0xFE, 0x00 },
  { 0xFE, 0x00 },
  { 0xF2, 0x0F },
  { 0xFE, 0x02 },
  { 0x40, 0xBF },
  { 0x46, 0xCF },
  { 0xFE, 0x00 },
  { 0xFE, 0x00 },
  { 0x24, 0xFF },
  { 0xFE, 0x00 },
};

#define GC2145_INIT_REG_COUNT \
  (sizeof(g_gc2145_init_regs) / sizeof(g_gc2145_init_regs[0]))

static const struct gc2145_reg g_gc2145_640_480_regs[] =
{
  { 0xFE, 0x00 },
  { 0x05, 0x01 },
  { 0x06, 0x56 },
  { 0x07, 0x00 },
  { 0x08, 0x32 },
  { 0xFE, 0x01 },
  { 0x25, 0x00 },
  { 0x26, 0xFA },
  { 0x27, 0x04 },
  { 0x28, 0xE2 },
  { 0x29, 0x04 },
  { 0x2A, 0xE2 },
  { 0x2B, 0x04 },
  { 0x2C, 0xE2 },
  { 0x2D, 0x04 },
  { 0x2E, 0xE2 },
  { 0xFE, 0x00 },
  { 0xFE, 0x00 },
  { 0xFE, 0x00 },
  { 0xF8, 0x85 },
  { 0xFA, 0x00 },
  { 0xFE, 0x00 },
  { 0x09, 0x00 },
  { 0x0A, 0x78 },
  { 0x0B, 0x00 },
  { 0x0C, 0xA0 },
  { 0x0D, 0x03 },
  { 0x0E, 0xD0 },
  { 0x0F, 0x05 },
  { 0x10, 0x10 },
  { 0xFD, 0x01 },
  { 0x90, 0x01 },
  { 0x91, 0x00 },
  { 0x92, 0x00 },
  { 0x93, 0x00 },
  { 0x94, 0x00 },
  { 0x95, 0x01 },
  { 0x96, 0xE0 },
  { 0x97, 0x02 },
  { 0x98, 0x80 },
};

#define GC2145_640_480_REG_COUNT \
  (sizeof(g_gc2145_640_480_regs) / sizeof(g_gc2145_640_480_regs[0]))


struct bk7258_gc2145_dev_s
{
  struct imgsensor_s sensor;    /* Must be first, see imgdata's identical
                                 * base-pointer convention. */
  bool initialized;
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static bool bk7258_gc2145_is_available(FAR struct imgsensor_s *sensor);
static int  bk7258_gc2145_init(FAR struct imgsensor_s *sensor);
static int  bk7258_gc2145_uninit(FAR struct imgsensor_s *sensor);
static FAR const char *
bk7258_gc2145_get_driver_name(FAR struct imgsensor_s *sensor);
static int  bk7258_gc2145_validate_frame_setting(
    FAR struct imgsensor_s *sensor, imgsensor_stream_type_t type,
    uint8_t nr_datafmts, FAR imgsensor_format_t *datafmts,
    FAR imgsensor_interval_t *interval);
static int  bk7258_gc2145_start_capture(
    FAR struct imgsensor_s *sensor, imgsensor_stream_type_t type,
    uint8_t nr_datafmts, FAR imgsensor_format_t *datafmts,
    FAR imgsensor_interval_t *interval);
static int  bk7258_gc2145_stop_capture(FAR struct imgsensor_s *sensor,
                                        imgsensor_stream_type_t type);

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const struct imgsensor_ops_s g_bk7258_gc2145_ops =
{
  .is_available           = bk7258_gc2145_is_available,
  .init                   = bk7258_gc2145_init,
  .uninit                 = bk7258_gc2145_uninit,
  .get_driver_name        = bk7258_gc2145_get_driver_name,
  .validate_frame_setting = bk7258_gc2145_validate_frame_setting,
  .start_capture          = bk7258_gc2145_start_capture,
  .stop_capture           = bk7258_gc2145_stop_capture,
};

/* Static capability descriptors, per the framework's "静态能力定义"
 * convention (docs/zh-cn/device_dev_guide/media/camera/Camera_Driver.md
 * section 7.3): only one discrete resolution/format is advertised,
 * matching this driver's single register table (no runtime resolution
 * switching -- see this file's header comment). */

static const struct v4l2_fmtdesc g_bk7258_gc2145_fmtdescs[] =
{
  {
    .index = 0,
    .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
    .pixelformat = V4L2_PIX_FMT_YUYV,
  },
};

static const struct v4l2_frmsizeenum g_bk7258_gc2145_frmsizes[] =
{
  {
    .index = 0,
    .pixel_format = V4L2_PIX_FMT_YUYV,
    .type = V4L2_FRMSIZE_TYPE_DISCRETE,
    .discrete =
      {
        .width = GC2145_CAPTURE_WIDTH,
        .height = GC2145_CAPTURE_HEIGHT,
      },
  },
};

static struct bk7258_gc2145_dev_s g_bk7258_gc2145 =
{
  .sensor =
    {
      .ops = &g_bk7258_gc2145_ops,
      .fmtdescs_num = 1,
      .fmtdescs = g_bk7258_gc2145_fmtdescs,
      .frmsizes_num = 1,
      .frmsizes = g_bk7258_gc2145_frmsizes,
      .frmintervals_num = 0,
      .frmintervals = NULL,
    },
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void bk7258_gc2145_power_on(void)
{
  uint32_t before;
  uint32_t after;
  clock_t t0;
  clock_t t1;

  before = getreg32(BK7258_GPIO_CFG(DVP_POWER_PIN));
  bk7258_gpio_output(DVP_POWER_PIN, true);
  after = getreg32(BK7258_GPIO_CFG(DVP_POWER_PIN));
  printf("bk7258_camera_imgsensor: power_on GPIO%u GPIO_CFG before=0x%08x "
         "after=0x%08x (OUTPUT bit %s)\n",
         DVP_POWER_PIN, (unsigned int)before, (unsigned int)after,
         (after & BK7258_GPIO_OUTPUT) ? "SET" : "CLEAR");

  /* 120ms LDO settle delay, matching bk7258_gc2145.c's convention (see
   * that file's bk7258_gc2145_power_on() comment for the cross-checked
   * rationale, not repeated here).  Measured against clock_systime_ticks()
   * (SysTick-backed, CONFIG_USEC_PER_TICK=1000 -> 1 tick = 1ms,
   * independent of the up_udelay() busy-loop calibration this call
   * itself depends on) so the *actual* elapsed wall time of this delay
   * is directly visible in the log, rather than trusting the
   * CONFIG_BOARD_LOOPSPERMSEC estimate. */
  t0 = clock_systime_ticks();
  up_udelay(120000);
  t1 = clock_systime_ticks();
  printf("bk7258_camera_imgsensor: power_on 120ms delay done, measured "
         "%lu ticks (~%lu ms, 1 tick=%dus)\n",
         (unsigned long)(t1 - t0), (unsigned long)(t1 - t0),
         (int)(USEC_PER_TICK));
}

static void bk7258_gc2145_reset(void)
{
  uint32_t after_low;
  uint32_t after_high;
  clock_t t0;
  clock_t t1;

  bk7258_gpio_output(DVP_RESET_PIN, false);
  after_low = getreg32(BK7258_GPIO_CFG(DVP_RESET_PIN));
  printf("bk7258_camera_imgsensor: reset GPIO%u driven LOW, GPIO_CFG="
         "0x%08x (OUTPUT bit %s)\n",
         DVP_RESET_PIN, (unsigned int)after_low,
         (after_low & BK7258_GPIO_OUTPUT) ? "SET(!)" : "CLEAR(ok)");
  t0 = clock_systime_ticks();
  up_udelay(120000);
  t1 = clock_systime_ticks();
  printf("bk7258_camera_imgsensor: reset LOW-phase delay measured %lu "
         "ticks (~%lu ms)\n", (unsigned long)(t1 - t0),
         (unsigned long)(t1 - t0));

  bk7258_gpio_output(DVP_RESET_PIN, true);
  after_high = getreg32(BK7258_GPIO_CFG(DVP_RESET_PIN));
  printf("bk7258_camera_imgsensor: reset GPIO%u driven HIGH, GPIO_CFG="
         "0x%08x (OUTPUT bit %s)\n",
         DVP_RESET_PIN, (unsigned int)after_high,
         (after_high & BK7258_GPIO_OUTPUT) ? "SET(ok)" : "CLEAR(!)");
  t0 = clock_systime_ticks();
  up_udelay(120000);
  t1 = clock_systime_ticks();
  printf("bk7258_camera_imgsensor: reset HIGH-phase delay measured %lu "
         "ticks (~%lu ms)\n", (unsigned long)(t1 - t0),
         (unsigned long)(t1 - t0));
  printf("bk7258_camera_imgsensor: reset sequence complete\n");
}

static void bk7258_gc2145_dvp_pinmux(void)
{
  uint32_t i;

  /* GPIO27 (MCLK) uses a different pinmux function index than the
   * other DVP pins -- see this file's BK7258_SYS_REG_0X0A/0X0D comment
   * block for the full evidence chain.  DVP_PINMUX_FUNCTION (index 0,
   * GPIO_DEV_JPEG_MCLK) would NOT actually drive an oscillating clock
   * signal onto this pin; DVP_MCLK_PINMUX_FUNCTION (index 1,
   * GPIO_DEV_CLK_AUXS_CIS) is required, together with
   * bk7258_gc2145_mclk_enable() actually enabling that clock path's
   * gate. */
  bk7258_gpio_set_function(DVP_MCLK_PIN, DVP_MCLK_PINMUX_FUNCTION);
  bk7258_gpio_set_function(DVP_PCLK_PIN, DVP_PINMUX_FUNCTION);
  bk7258_gpio_set_function(DVP_HSYNC_PIN, DVP_PINMUX_FUNCTION);
  bk7258_gpio_set_function(DVP_VSYNC_PIN, DVP_PINMUX_FUNCTION);

  for (i = 0; i < 8; i++)
    {
      bk7258_gpio_set_function(DVP_DATA_PIN(i), DVP_PINMUX_FUNCTION);
    }
}

/* Enables the AUXS_CIS system clock path that supplies GC2145's MCLK
 * input (GPIO27), matching bk_avdk_smp's dvp_camera_mclk_enable()
 * MCLK_24M case exactly (cksel=3, ckdiv=19) -- see this file's
 * BK7258_SYS_REG_0X0A/0X0D comment block for the full rationale.  Must
 * be called after bk7258_gc2145_dvp_pinmux() has already selected
 * GPIO27's GPIO_DEV_CLK_AUXS_CIS function (pinmux alone does not start
 * the clock; this register sequence is the actual clock-source-select
 * + divider + gate-enable that makes the signal appear on the pin),
 * and before any I2C traffic (GC2145's datasheet power-up sequence
 * requires MCLK to be applied before the sensor's internal logic,
 * including its I2C/SCCB slave interface, becomes responsive). */
static void bk7258_gc2145_mclk_enable(void)
{
  uint32_t reg;

  reg = getreg32(BK7258_SYS_REG_0X0A);
  reg &= ~(BK7258_SYS_CKSEL_AUXS_CIS_MASK | BK7258_SYS_CKDIV_AUXS_CIS_MASK);
  reg |= (GC2145_MCLK_CKSEL << BK7258_SYS_CKSEL_AUXS_CIS_SHIFT) &
         BK7258_SYS_CKSEL_AUXS_CIS_MASK;
  reg |= (GC2145_MCLK_CKDIV << BK7258_SYS_CKDIV_AUXS_CIS_SHIFT) &
         BK7258_SYS_CKDIV_AUXS_CIS_MASK;
  putreg32(reg, BK7258_SYS_REG_0X0A);

  reg = getreg32(BK7258_SYS_REG_0X0D);
  reg |= BK7258_SYS_CIS_AUXS_CKEN;
  putreg32(reg, BK7258_SYS_REG_0X0D);

  printf("bk7258_camera_imgsensor: MCLK (GPIO%u) AUXS_CIS clock enabled, "
         "reg0x0a=0x%08x reg0x0d=0x%08x\n",
         DVP_MCLK_PIN, (unsigned int)getreg32(BK7258_SYS_REG_0X0A),
         (unsigned int)getreg32(BK7258_SYS_REG_0X0D));
}

static bool bk7258_gc2145_write_reg_table(const struct gc2145_reg *table,
                                           size_t count)
{
  size_t i;

  for (i = 0; i < count; i++)
    {
      if (!bk7258_i2c1_write_reg(GC2145_I2C_ADDR, table[i].reg,
                                 table[i].value))
        {
          return false;
        }
    }

  return true;
}

static bool bk7258_gc2145_is_available(FAR struct imgsensor_s *sensor)
{
  /* No readable chip-ID register is used by this driver's ported
   * register tables (see bk7258_gc2145.c's bk7258_gc9d01_test()-style
   * rationale for GC9D01: no RDID path was ever wired up for GC2145
   * either).  Report available unconditionally; the actual proof this
   * hardware exists and responds is whether bk7258_gc2145_init()'s
   * register writes succeed (return false there is what causes
   * capture_register() callers to see a failed device, not this
   * function). */
  return true;
}

static int bk7258_gc2145_init(FAR struct imgsensor_s *sensor)
{
  FAR struct bk7258_gc2145_dev_s *priv =
      (FAR struct bk7258_gc2145_dev_s *)sensor;

  printf("bk7258_camera_imgsensor: gc2145 power/reset/pinmux/i2c1 init\n");
  bk7258_gc2145_power_on();
  bk7258_gc2145_reset();
  bk7258_gc2145_dvp_pinmux();
  bk7258_gc2145_mclk_enable();

  printf("bk7258_camera_imgsensor: pre-i2c-init snapshot: GPIO%u(power) "
         "CFG=0x%08x GPIO%u(reset) CFG=0x%08x GPIO42(scl) CFG=0x%08x "
         "GPIO43(sda) CFG=0x%08x\n",
         DVP_POWER_PIN, (unsigned int)getreg32(BK7258_GPIO_CFG(DVP_POWER_PIN)),
         DVP_RESET_PIN, (unsigned int)getreg32(BK7258_GPIO_CFG(DVP_RESET_PIN)),
         (unsigned int)getreg32(BK7258_GPIO_CFG(42u)),
         (unsigned int)getreg32(BK7258_GPIO_CFG(43u)));

  bk7258_i2c1_init();

  printf("bk7258_camera_imgsensor: post-i2c-init snapshot: GPIO42(scl) "
         "CFG=0x%08x GPIO43(sda) CFG=0x%08x\n",
         (unsigned int)getreg32(BK7258_GPIO_CFG(42u)),
         (unsigned int)getreg32(BK7258_GPIO_CFG(43u)));

  if (!bk7258_i2c1_sda_idle_diag())
    {
      printf("bk7258_camera_imgsensor: WARNING: idle-bus SDA read-back "
             "was NOT reliably high (no clock/slave activity involved) "
             "-- ACK sampling is unreliable, any NACK below cannot be "
             "trusted as sensor behavior; suspect missing/broken "
             "pull-up on GPIO43 or the pin stuck low\n");
    }

  printf("bk7258_camera_imgsensor: writing %u init registers\n",
         (unsigned int)GC2145_INIT_REG_COUNT);
  if (!bk7258_gc2145_write_reg_table(g_gc2145_init_regs,
                                     GC2145_INIT_REG_COUNT))
    {
      printf("bk7258_camera_imgsensor: init register table write failed\n");
      return -EIO;
    }

  printf("bk7258_camera_imgsensor: writing %u resolution registers\n",
         (unsigned int)GC2145_640_480_REG_COUNT);
  if (!bk7258_gc2145_write_reg_table(g_gc2145_640_480_regs,
                                     GC2145_640_480_REG_COUNT))
    {
      printf("bk7258_camera_imgsensor: resolution register table write "
             "failed\n");
      return -EIO;
    }

  printf("bk7258_camera_imgsensor: gc2145 init complete\n");
  priv->initialized = true;
  return OK;
}

static int bk7258_gc2145_uninit(FAR struct imgsensor_s *sensor)
{
  FAR struct bk7258_gc2145_dev_s *priv =
      (FAR struct bk7258_gc2145_dev_s *)sensor;

  priv->initialized = false;
  return OK;
}

static FAR const char *
bk7258_gc2145_get_driver_name(FAR struct imgsensor_s *sensor)
{
  return "GC2145";
}

static int bk7258_gc2145_validate_frame_setting(
    FAR struct imgsensor_s *sensor, imgsensor_stream_type_t type,
    uint8_t nr_datafmts, FAR imgsensor_format_t *datafmts,
    FAR imgsensor_interval_t *interval)
{
  if (nr_datafmts < 1)
    {
      return -EINVAL;
    }

  /* Only 640x480 YUYV is supported -- this driver's register tables do
   * not implement any other resolution (see this file's header
   * comment). */
  if (datafmts[IMGSENSOR_FMT_MAIN].width != GC2145_CAPTURE_WIDTH ||
      datafmts[IMGSENSOR_FMT_MAIN].height != GC2145_CAPTURE_HEIGHT ||
      datafmts[IMGSENSOR_FMT_MAIN].pixelformat != IMGSENSOR_PIX_FMT_YUYV)
    {
      return -EINVAL;
    }

  return OK;
}

static int bk7258_gc2145_start_capture(
    FAR struct imgsensor_s *sensor, imgsensor_stream_type_t type,
    uint8_t nr_datafmts, FAR imgsensor_format_t *datafmts,
    FAR imgsensor_interval_t *interval)
{
  int ret;

  ret = bk7258_gc2145_validate_frame_setting(sensor, type, nr_datafmts,
                                              datafmts, interval);
  if (ret < 0)
    {
      return ret;
    }

  /* GC2145 has no separate streaming-enable register write in this
   * driver's ported tables -- the sensor begins outputting DVP data
   * continuously as soon as its init sequence completes in
   * bk7258_gc2145_init().  Capture start/stop is therefore entirely a
   * matter of whether the imgdata half (YUV_BUF/DMA) is listening, not
   * anything this function needs to tell the sensor. */
  return OK;
}

static int bk7258_gc2145_stop_capture(FAR struct imgsensor_s *sensor,
                                       imgsensor_stream_type_t type)
{
  return OK;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

FAR struct imgsensor_s *bk7258_camera_imgsensor_initialize(void)
{
  return &g_bk7258_gc2145.sensor;
}
