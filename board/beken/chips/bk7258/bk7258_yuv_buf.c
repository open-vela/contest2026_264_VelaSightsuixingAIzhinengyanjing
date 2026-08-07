/****************************************************************************
 * board/beken/chips/bk7258/bk7258_yuv_buf.c
 *
 * BK7258 YUV_BUF controller, YUV direct-capture mode only.  Register
 * layout source: bk_avdk_smp release/v3.1.1
 * ap/middleware/soc/bk7258_ap/soc/yuv_buf_struct.h and
 * ap/middleware/soc/bk7258_ap/hal/yuv_buf_ll.h.
 *
 * Register map (byte offset from BK7258_YUV_BUF_BASE):
 *   global_ctrl   0x08 - bit[0] soft_reset, bit[1] clk_gate_bypass
 *   ctrl          0x10 - bit[0] yuv_mode, bit[2:3] yuv_fmt_sel,
 *                         bit[6] hsync_rev, bit[7] vsync_rev,
 *                         bit[9] sync_edge_dect_en, bit[10:11] mclk_div,
 *                         bit[12] h264_mode, bit[13] bps_cis,
 *                         bit[14] memrev, bit[19] soi_hsync
 *   pixel         0x14 - bit[0:7] x_pixel, bit[8:15] y_pixel,
 *                         bit[16:31] frame_blk
 *   em_base_addr  0x20
 *   int_en        0x24 - bit[2] sm0_wr_int_en, bit[3] sm1_wr_int_en
 *   int_status    0x28 - bit[2] sm0_wr_int, bit[3] sm1_wr_int
 *                         (write-1-to-clear)
 *   emr_base_addr 0x30
 *   resize_pixel  0x34 - bit[1:8] x_pixel, bit[9:16] y_pixel
 *
 * x_pixel/y_pixel are width/height divided by 8 (matching bk_dvp.c's
 * "yuv_mode_config.x_pixel = config->width / 8"); frame_blk = x_pixel *
 * y_pixel / 2.  YUV direct-capture mode hard-codes em_base_addr/
 * emr_base_addr to SOC_PSRAM_DATA_BASE rather than an
 * application-supplied address (ap/middleware/soc/common/hal/
 * yuv_buf_hal.c yuv_buf_hal_set_yuv_mode_config()).
 *
 * Power/clock: enabling this module requires clearing the video
 * pipeline's shared power-down bit and setting its own clock gate,
 * both in the system controller's register space
 * (ap/middleware/driver/yuv_buf/yuv_buf_driver.c
 * yuv_buf_init_common()) -- neither is part of YUV_BUF's own register
 * block.  bk_avdk_smp enables the H264 clock gate alongside YUV_BUF's
 * even for pure YUV direct-capture mode; both are set here to match.
 *
 * Interrupt: single line (INT_SRC_YUVB / BK7258_IRQ_YUVB) multiplexing
 * several event bits in int_status; only sm0_wr/sm1_wr (line-batch-done,
 * fired every 8 lines as the hardware alternates between its two
 * ping-pong PSRAM regions) are consumed here.  Full-frame assembly
 * across the ~60 line-batch-done events needed for one frame is the
 * caller's responsibility -- see bk7258_camera_imgdata.c.
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#include <nuttx/config.h>
#include <nuttx/arch.h>
#include <nuttx/irq.h>
#include <stdint.h>

#include "arm_internal.h"
#include "hardware/bk7258_memorymap.h"
#include "irq.h"
#include "bk7258_yuv_buf.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define BK7258_YUV_BUF_BASE        0x48020000u  /* SOC_YUV_BUF_REG_BASE */

#define YUV_BUF_REG(word_off)      (BK7258_YUV_BUF_BASE + (word_off) * 4u)

#define YUV_BUF_GLOBAL_CTRL              YUV_BUF_REG(2)
#define YUV_BUF_GLOBAL_CTRL_SOFT_RESET   (1u << 0)

#define YUV_BUF_CTRL                     YUV_BUF_REG(4)
#define YUV_BUF_CTRL_YUV_MODE            (1u << 0)
#define YUV_BUF_CTRL_YUV_FMT_SEL_SHIFT   2u
#define YUV_BUF_CTRL_YUV_FMT_SEL_MASK    (0x3u << YUV_BUF_CTRL_YUV_FMT_SEL_SHIFT)
#define YUV_BUF_CTRL_HSYNC_REV           (1u << 6)
#define YUV_BUF_CTRL_VSYNC_REV           (1u << 7)
#define YUV_BUF_CTRL_SYNC_EDGE_DECT_EN   (1u << 9)
#define YUV_BUF_CTRL_MCLK_DIV_SHIFT      10u
#define YUV_BUF_CTRL_MCLK_DIV_MASK       (0x3u << YUV_BUF_CTRL_MCLK_DIV_SHIFT)
#define YUV_BUF_CTRL_H264_MODE           (1u << 12)
#define YUV_BUF_CTRL_BPS_CIS             (1u << 13)
#define YUV_BUF_CTRL_MEMREV              (1u << 14)
#define YUV_BUF_CTRL_SOI_HSYNC           (1u << 19)

#define YUV_BUF_FMT_YUYV                 0u
#define YUV_BUF_MCLK_DIV_3               3u

#define YUV_BUF_PIXEL                    YUV_BUF_REG(5)
#define YUV_BUF_PIXEL_X_SHIFT            0u
#define YUV_BUF_PIXEL_Y_SHIFT            8u
#define YUV_BUF_PIXEL_FRAME_BLK_SHIFT    16u

#define YUV_BUF_EM_BASE_ADDR             YUV_BUF_REG(8)
#define YUV_BUF_INT_EN                   YUV_BUF_REG(9)
#define YUV_BUF_INT_EN_ALL               0x1FFu

#define YUV_BUF_INT_STATUS               YUV_BUF_REG(10)
#define YUV_BUF_INT_STATUS_SM0_WR        (1u << 2)
#define YUV_BUF_INT_STATUS_SM1_WR        (1u << 3)

#define YUV_BUF_EMR_BASE_ADDR            YUV_BUF_REG(12)

#define YUV_BUF_RESIZE_PIXEL             YUV_BUF_REG(13)
#define YUV_BUF_RESIZE_PIXEL_X_SHIFT     1u
#define YUV_BUF_RESIZE_PIXEL_Y_SHIFT     9u

/* SOC_PSRAM_DATA_BASE, ap/include/soc/bk7258/reg_base.h. */

#define YUV_BUF_PSRAM_LINE_BUF_ADDR      0x60000000u

/* System controller registers (SOC_SYS_REG_BASE) that gate the video
 * pipeline's power and this module's clock -- required before any of
 * YUV_BUF's own registers have real effect.  Ported from
 * yuv_buf_init_common(); see file header.
 */

#define BK7258_SYS_REG_BASE              0x44010000u
#define BK7258_SYS_REG_0X10               (BK7258_SYS_REG_BASE + (0x10u << 2))
#define BK7258_SYS_PWD_VIDP                (1u << 7)

#define BK7258_SYS_REG_0X0D               (BK7258_SYS_REG_BASE + (0xdu << 2))
#define BK7258_SYS_YUV_CKEN                 (1u << 3)
#define BK7258_SYS_H264_CKEN                (1u << 0)

/****************************************************************************
 * Private Data
 ****************************************************************************/

static bk7258_yuv_buf_line_cb_t g_line_cb;
static void *g_line_cb_arg;
static uint32_t g_line_batch_bytes;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static int bk7258_yuv_buf_isr(int irq, void *context, void *arg)
{
  uint32_t status = getreg32(YUV_BUF_INT_STATUS);

  /* Write-1-to-clear: write back exactly the bits read as set. */

  putreg32(status, YUV_BUF_INT_STATUS);

  if (g_line_cb != NULL)
    {
      if ((status & YUV_BUF_INT_STATUS_SM0_WR) != 0)
        {
          g_line_cb(BK7258_YUV_BUF_BANK_SM0, g_line_cb_arg);
        }

      if ((status & YUV_BUF_INT_STATUS_SM1_WR) != 0)
        {
          g_line_cb(BK7258_YUV_BUF_BANK_SM1, g_line_cb_arg);
        }
    }

  return 0;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

void bk7258_yuv_buf_init(void)
{
  uint32_t reg;

  /* Power on the video pipeline and enable YUV_BUF's (and H264's,
   * required alongside it even for direct-capture mode) clock gate
   * before touching this module's own registers.
   */

  reg = getreg32(BK7258_SYS_REG_0X10);
  reg &= ~BK7258_SYS_PWD_VIDP;
  putreg32(reg, BK7258_SYS_REG_0X10);

  reg = getreg32(BK7258_SYS_REG_0X0D);
  reg |= BK7258_SYS_H264_CKEN | BK7258_SYS_YUV_CKEN;
  putreg32(reg, BK7258_SYS_REG_0X0D);

  /* Soft-reset pulse. */

  putreg32(YUV_BUF_GLOBAL_CTRL_SOFT_RESET, YUV_BUF_GLOBAL_CTRL);
  up_udelay(10);
  putreg32(0, YUV_BUF_GLOBAL_CTRL);

  irq_attach(BK7258_IRQ_YUVB, bk7258_yuv_buf_isr, NULL);
  up_enable_irq(BK7258_IRQ_YUVB);
}

void bk7258_yuv_buf_configure(uint16_t width, uint16_t height)
{
  uint32_t x_pixel = width / 8u;
  uint32_t y_pixel = height / 8u;
  uint32_t frame_blk = (x_pixel * y_pixel) / 2u;
  uint32_t ctrl;

  g_line_batch_bytes = (uint32_t)width * 8u * 2u; /* 8 lines, YUV422 */

  putreg32((x_pixel << YUV_BUF_PIXEL_X_SHIFT) |
           (y_pixel << YUV_BUF_PIXEL_Y_SHIFT) |
           (frame_blk << YUV_BUF_PIXEL_FRAME_BLK_SHIFT),
           YUV_BUF_PIXEL);

  ctrl = getreg32(YUV_BUF_CTRL);
  ctrl &= ~(YUV_BUF_CTRL_YUV_FMT_SEL_MASK | YUV_BUF_CTRL_HSYNC_REV |
            YUV_BUF_CTRL_VSYNC_REV | YUV_BUF_CTRL_MCLK_DIV_MASK |
            YUV_BUF_CTRL_BPS_CIS | YUV_BUF_CTRL_MEMREV);
  ctrl |= (YUV_BUF_FMT_YUYV << YUV_BUF_CTRL_YUV_FMT_SEL_SHIFT);
  ctrl |= YUV_BUF_CTRL_SYNC_EDGE_DECT_EN;
  ctrl |= (YUV_BUF_MCLK_DIV_3 << YUV_BUF_CTRL_MCLK_DIV_SHIFT);
  ctrl |= YUV_BUF_CTRL_SOI_HSYNC;
  putreg32(ctrl, YUV_BUF_CTRL);

  putreg32((x_pixel << YUV_BUF_RESIZE_PIXEL_X_SHIFT) |
           (y_pixel << YUV_BUF_RESIZE_PIXEL_Y_SHIFT),
           YUV_BUF_RESIZE_PIXEL);

  putreg32(YUV_BUF_PSRAM_LINE_BUF_ADDR, YUV_BUF_EM_BASE_ADDR);
  putreg32(YUV_BUF_PSRAM_LINE_BUF_ADDR, YUV_BUF_EMR_BASE_ADDR);

  putreg32(YUV_BUF_INT_EN_ALL, YUV_BUF_INT_EN);
}

uint32_t bk7258_yuv_buf_get_line_buf_addr(void)
{
  return YUV_BUF_PSRAM_LINE_BUF_ADDR;
}

uint32_t bk7258_yuv_buf_get_line_batch_bytes(void)
{
  return g_line_batch_bytes;
}

void bk7258_yuv_buf_set_line_callback(bk7258_yuv_buf_line_cb_t cb, void *arg)
{
  g_line_cb = cb;
  g_line_cb_arg = arg;
}

void bk7258_yuv_buf_start(void)
{
  modifyreg32(YUV_BUF_CTRL, YUV_BUF_CTRL_H264_MODE, YUV_BUF_CTRL_YUV_MODE);
}

void bk7258_yuv_buf_stop(void)
{
  modifyreg32(YUV_BUF_CTRL, YUV_BUF_CTRL_YUV_MODE, 0);
}
