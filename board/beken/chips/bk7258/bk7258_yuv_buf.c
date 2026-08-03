/****************************************************************************
 * board/beken/chips/bk7258/bk7258_yuv_buf.c
 *
 * BK7258 YUV_BUF controller thin port, YUV direct-capture mode only.
 * Register layout source: bk_avdk_smp release/v3.1.1
 * ap/middleware/soc/bk7258_ap/soc/yuv_buf_struct.h and
 * ap/middleware/soc/bk7258_ap/hal/yuv_buf_ll.h.
 *
 * Register offsets (byte offset from BK7258_YUV_BUF_BASE, word index in
 * yuv_buf_hw_t from yuv_buf_struct.h in parentheses):
 *   global_ctrl  0x08 (REG_0x02) - bit[0] soft_reset, bit[1] clk_gate_bypass
 *   ctrl         0x10 (REG_0x04) - bit[0] yuv_mode, bit[12] h264_mode
 *   pixel        0x14 (REG_0x05) - bit[0:7] x_pixel, bit[8:15] y_pixel,
 *                                  bit[16:31] frame_blk
 *   em_base_addr 0x20 (REG_0x08)
 *   int_en       0x24 (REG_0x09) - bit[1] yuv_arv_int_en, bit[2]
 *                                  sm0_wr_int_en, bit[3] sm1_wr_int_en
 *   int_status   0x28 (REG_0x0a) - bit[1] yuv_arv_int, bit[2] sm0_wr_int,
 *                                  bit[3] sm1_wr_int
 *   emr_base_addr 0x30 (REG_0x0c)
 *
 * Pixel unit: x_pixel/y_pixel registers do not perform any unit
 * conversion themselves (yuv_buf_hal_set_config_common() writes
 * config->x_pixel/y_pixel directly).  The /8 conversion happens at the
 * application layer in bk_dvp.c ("yuv_mode_config.x_pixel =
 * config->width / 8"), so this driver's bk7258_yuv_buf_configure()
 * performs that same /8 conversion internally, taking real pixel
 * width/height as its parameters.  frame_blk = x_pixel * y_pixel / 2
 * (using the already-/8 x_pixel/y_pixel values).
 *
 * PSRAM line buffer: YUV direct-capture mode hard-codes
 * em_base_addr/emr_base_addr to SOC_PSRAM_DATA_BASE
 * (ap/middleware/soc/common/hal/yuv_buf_hal.c
 * yuv_buf_hal_set_yuv_mode_config(): "yuv_mode set em_base_addr as
 * psram"), not an application-supplied address.  This driver writes that
 * fixed address to both registers and exposes it via
 * bk7258_yuv_buf_get_line_buf_addr() for the caller to use as the DMA
 * source address.
 *
 * Interrupt: single hardware interrupt line (INT_SRC_YUVB, EXTIRQ=58,
 * NuttX IRQ=74) multiplexes several events via int_status bits; this
 * driver only handles sm0_wr_int/sm1_wr_int (line-batch-done, fired every
 * 8 lines as the hardware alternates between its two ping-pong PSRAM
 * regions).  Interrupt status is cleared by writing the read value back
 * (write-1-to-clear), per yuv_buf_ll_clear_interrupt_status().
 *
 * Full-frame assembly (across the ~60 line-batch-done events needed for
 * a 640x480 frame) is NOT implemented in this file -- this driver only
 * reports which ping-pong bank (SM0/SM1) fired, per batch, via
 * bk7258_yuv_buf_line_cb_t.  Tracking the running frame offset, copying
 * each batch out via DMA before the hardware overwrites that bank with
 * the next occurrence, and detecting frame completion is the caller's
 * responsibility -- see board/beken/chips/bk7258/bk7258_camera_imgdata.c,
 * which implements that logic as part of the imgdata_ops_s.start_capture
 * path.
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

#define BK7258_YUV_BUF_BASE        0x48020000u  /* SOC_YUV_BUF_REG_BASE */

#define YUV_BUF_REG(word_off)     (BK7258_YUV_BUF_BASE + (word_off) * 4u)

#define YUV_BUF_GLOBAL_CTRL        YUV_BUF_REG(2)
#define YUV_BUF_GLOBAL_CTRL_SOFT_RESET  (1u << 0)

#define YUV_BUF_CTRL               YUV_BUF_REG(4)
#define YUV_BUF_CTRL_YUV_MODE      (1u << 0)
#define YUV_BUF_CTRL_H264_MODE     (1u << 12)

#define YUV_BUF_PIXEL              YUV_BUF_REG(5)
#define YUV_BUF_PIXEL_X_SHIFT      0u
#define YUV_BUF_PIXEL_Y_SHIFT      8u
#define YUV_BUF_PIXEL_FRAME_BLK_SHIFT 16u

#define YUV_BUF_EM_BASE_ADDR       YUV_BUF_REG(8)
#define YUV_BUF_INT_EN             YUV_BUF_REG(9)
#define YUV_BUF_INT_EN_SM0_WR      (1u << 2)
#define YUV_BUF_INT_EN_SM1_WR      (1u << 3)

#define YUV_BUF_INT_STATUS         YUV_BUF_REG(10)
#define YUV_BUF_INT_STATUS_SM0_WR  (1u << 2)
#define YUV_BUF_INT_STATUS_SM1_WR  (1u << 3)

#define YUV_BUF_EMR_BASE_ADDR      YUV_BUF_REG(12)

/* SOC_PSRAM_DATA_BASE, ap/include/soc/bk7258/reg_base.h:43 */
#define YUV_BUF_PSRAM_LINE_BUF_ADDR 0x60000000u

static bk7258_yuv_buf_line_cb_t g_line_cb;
static void *g_line_cb_arg;
static uint32_t g_line_batch_bytes;

static int bk7258_yuv_buf_isr(int irq, void *context, void *arg)
{
  uint32_t status = getreg32(YUV_BUF_INT_STATUS);

  /* Write-1-to-clear: write back exactly the bits that were read as set,
   * per yuv_buf_ll_clear_interrupt_status(). */
  putreg32(status, YUV_BUF_INT_STATUS);

  /* SM0_WR and SM1_WR are reported as separate bits and, per the
   * hardware's alternating ping-pong behavior, are not expected to both
   * be set in the same status read; handle them independently rather
   * than assuming mutual exclusion, so a spurious simultaneous report
   * (if it ever occurs) still drains both banks instead of silently
   * dropping one. */
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

void bk7258_yuv_buf_init(void)
{
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
  uint32_t pixel_reg;

  /* One ping-pong bank holds 8 lines of YUV422 (2 bytes/pixel) data,
   * matching bk_dvp.c's "yuv_config.yuv_pingpong_length = config->width
   * * 8 * 2".  Saved here so bk7258_yuv_buf_get_line_batch_bytes() can
   * report it without callers needing to duplicate this formula. */
  g_line_batch_bytes = (uint32_t)width * 8u * 2u;

  pixel_reg = (x_pixel << YUV_BUF_PIXEL_X_SHIFT) |
              (y_pixel << YUV_BUF_PIXEL_Y_SHIFT) |
              (frame_blk << YUV_BUF_PIXEL_FRAME_BLK_SHIFT);
  putreg32(pixel_reg, YUV_BUF_PIXEL);

  /* YUV direct-capture mode uses a fixed PSRAM line buffer address, not
   * an application-supplied one; see file header comment. */
  putreg32(YUV_BUF_PSRAM_LINE_BUF_ADDR, YUV_BUF_EM_BASE_ADDR);
  putreg32(YUV_BUF_PSRAM_LINE_BUF_ADDR, YUV_BUF_EMR_BASE_ADDR);

  putreg32(YUV_BUF_INT_EN_SM0_WR | YUV_BUF_INT_EN_SM1_WR, YUV_BUF_INT_EN);
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
  /* YUV direct-capture mode start = ctrl.yuv_mode=1, ctrl.h264_mode=0.
   * No separate "rencode start" trigger is needed (that bit is only used
   * by the JPEG/H264 encoder paths); per
   * yuv_buf_hal_start_yuv_mode(): "yuv_buf_ll_enable_yuv_buf_mode(hw);
   * yuv_buf_ll_disable_h264_encode_mode(hw);". */
  modifyreg32(YUV_BUF_CTRL, YUV_BUF_CTRL_H264_MODE, YUV_BUF_CTRL_YUV_MODE);
}

void bk7258_yuv_buf_stop(void)
{
  modifyreg32(YUV_BUF_CTRL, YUV_BUF_CTRL_YUV_MODE, 0);
}
