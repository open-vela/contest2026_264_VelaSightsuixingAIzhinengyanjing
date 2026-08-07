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
#include <stdio.h>

#include "arm_internal.h"
#include "hardware/bk7258_memorymap.h"
#include "irq.h"
#include "bk7258_yuv_buf.h"

#define BK7258_YUV_BUF_BASE        0x48020000u  /* SOC_YUV_BUF_REG_BASE */

#define YUV_BUF_REG(word_off)     (BK7258_YUV_BUF_BASE + (word_off) * 4u)

#define YUV_BUF_GLOBAL_CTRL        YUV_BUF_REG(2)

/* DISPROVEN POLARITY HYPOTHESIS (2026-08-07) -- kept as a record so this
 * is not re-attempted a third time without new evidence.
 *
 * After the PCLK/VSYNC software-poll diagnostic proved GC2145 genuinely
 * drives PCLK with a fast-toggling signal (toggle_count=3978/10000)
 * while int_status stayed 0x00000000 across every run, bit[0] here was
 * hypothesized to be active-low (0=held in reset, 1=reset released),
 * based on bk_avdk_smp's yuv_buf_ll_init() writing 0 then 1 with no
 * delay between the two calls and leaving the register at 1:
 *
 *   static inline void yuv_buf_ll_init(yuv_buf_hw_t *hw)
 *   {
 *     hw->global_ctrl.soft_reset = 0;
 *     hw->global_ctrl.soft_reset = 1;
 *     hw->global_ctrl.clk_gate_bypass = 1;
 *   }
 *
 * Two variants of this hypothesis were tried on real hardware and BOTH
 * caused a full AP-core hang (CP's IPC heartbeat monitor declaring the
 * AP dead after 6s of silence, "Assert at: mb_ipc_task:297", AP link
 * dropping back to CP CLI) -- not a camera-specific symptom, a
 * whole-system hang:
 *
 *   1. soft_reset=1 AND clk_gate_bypass=1 together (matching the
 *      reference's final register state exactly) -> hung.
 *   2. soft_reset=1 ALONE, clk_gate_bypass left at its true
 *      hardware-reset default of 0 -> hung identically (isolates the
 *      hang to the soft_reset polarity flip itself, not
 *      clk_gate_bypass).
 *
 * Since round 2 isolated the hang to soft_reset alone, the active-low
 * hypothesis is DISPROVEN on this specific SoC revision/board, despite
 * being a reasonable reading of the reference source. Per this
 * project's rule of stopping after 2+ same-direction failures rather
 * than guessing a third bit-value variant, this was reverted to the
 * original polarity (write 1, delay, write 0 -- ends at 0) that never
 * hung the system in any prior run. clk_gate_bypass is intentionally
 * left untouched/never-set, matching this driver's pre-investigation
 * state.
 *
 * The original "int_status stays 0x00000000 despite a confirmed-real
 * PCLK signal" problem remains OPEN. Resolving it will need either a
 * datasheet/register-map source beyond bk_avdk_smp's comments (which
 * this investigation has shown can be misleading when read as a
 * literal polarity spec rather than verified against real hardware),
 * or hardware-level register tracing (JTAG-attached debugger reading
 * the live value a working reference firmware leaves this register
 * at) -- not further trial-and-error bit flips on real hardware,
 * which is what caused two AP-core hangs during this investigation. */
#define YUV_BUF_GLOBAL_CTRL_SOFT_RESET  (1u << 0)
#define YUV_BUF_GLOBAL_CTRL_CLK_GATE_BYPASS (1u << 1)  /* never set; see above */

#define YUV_BUF_CTRL               YUV_BUF_REG(4)
#define YUV_BUF_CTRL_YUV_MODE      (1u << 0)
#define YUV_BUF_CTRL_YUV_FMT_SEL_SHIFT  2u  /* bit[2:3], yuv_format_t: YUYV=0 UYVY=1 YYUV=2 UVYY=3 */
#define YUV_BUF_CTRL_HSYNC_REV     (1u << 6)
#define YUV_BUF_CTRL_VSYNC_REV     (1u << 7)
#define YUV_BUF_CTRL_SYNC_EDGE_DECT_EN (1u << 9)
#define YUV_BUF_CTRL_MCLK_DIV_SHIFT 10u /* bit[10:11], mclk_div_t: DIV_4=0 DIV_6=1 DIV_2=2 DIV_3=3 */
#define YUV_BUF_CTRL_MCLK_DIV_MASK  (0x3u << YUV_BUF_CTRL_MCLK_DIV_SHIFT)
#define YUV_BUF_CTRL_H264_MODE     (1u << 12)
#define YUV_BUF_CTRL_BPS_CIS       (1u << 13)
#define YUV_BUF_CTRL_MEMREV        (1u << 14)
#define YUV_BUF_CTRL_SOI_HSYNC     (1u << 19)

/* mclk_div_t values, per hal_yuv_buf_types.h. YUV_MCLK_DIV_3 matches
 * bk_dvp.c's dvp_camera_yuv_buf_config_init() default for all DVP
 * sensors (GC2145 included, per dvp_gc2145.c's dvp_sensor_gc2145). */
#define YUV_BUF_MCLK_DIV_3         3u

#define YUV_BUF_PIXEL              YUV_BUF_REG(5)
#define YUV_BUF_PIXEL_X_SHIFT      0u
#define YUV_BUF_PIXEL_Y_SHIFT      8u
#define YUV_BUF_PIXEL_FRAME_BLK_SHIFT 16u

#define YUV_BUF_RESIZE_PIXEL       YUV_BUF_REG(13)
#define YUV_BUF_RESIZE_PIXEL_X_SHIFT 1u
#define YUV_BUF_RESIZE_PIXEL_Y_SHIFT 9u

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

/* System clock/power controller (SOC_SYS_REG_BASE, same register space
 * BK7258_SYSCTRL_BASE in this repo's own bk7258_sysctrl.h) registers
 * required to make the YUV_BUF module's own registers/interrupts have
 * any real effect at all -- ported from bk_avdk_smp's
 * ap/middleware/driver/yuv_buf/yuv_buf_driver.c yuv_buf_init_common(),
 * which this repo's YUV_BUF driver previously did not replicate.
 *
 * ROOT CAUSE (found after confirming via a diagnostic ISR fire counter
 * that bk7258_yuv_buf_isr() was NEVER invoked -- not once -- across a
 * full nxcamera stream test that got as far as calling
 * imgdata_ops_s.start_capture() successfully with a valid PSRAM-backed
 * frame buffer): this driver's bk7258_yuv_buf_init() only ever did a
 * soft-reset pulse plus NVIC-level irq_attach()/up_enable_irq() --
 * exactly like the earlier MCLK bug (GC2145 I2C never responding), the
 * DVP pinmux itself was correct, but a *separate*, non-obvious
 * system-controller register was never touched, so the underlying
 * hardware module was left completely inert (no power, no clock) even
 * though every software-visible step (register writes, ISR
 * registration, NVIC/EXTIRQ enable) looked correct in isolation.
 * yuv_buf_init_common() does three things this driver's init() was
 * missing:
 *   1. bk_pm_module_vote_power_ctrl(...VIDP_YUVBUF, ON) -- ultimately
 *      clears reg0x10 (SOC_SYS_REG_BASE+0x40) bit[7] "pwd_vidp"
 *      ("power-down video[pipeline]"), an active-high power-down bit
 *      shared by the whole video pipeline (JPEG/H264/YUV_BUF), per
 *      sys_hal_video_power_en()'s inverted-parameter call convention
 *      (passing logical "enable=1" ends up writing register value 0,
 *      i.e. "not powered down").
 *   2. sys_drv_set_yuv_buf_clk_en(1) -- sets reg0xd
 *      (SOC_SYS_REG_BASE+0x34) bit[3] "yuv_cken", the module's own
 *      clock gate (a sibling bit in the exact same register as this
 *      board's MCLK fix's "cis_auxs_cken" bit[9], per
 *      sys_struct.h's sys_reserver_reg0xd_t).
 *   3. sys_drv_int_group2_enable(YUV_BUF_INTERRUPT_CTRL_BIT) -- this
 *      one is NOT a separate missing step for this port: it ultimately
 *      writes SOC_SYS_REG_BASE+0x8c (sys_ll_set_cpu1_int_32_63_en_value()),
 *      which is the exact same register this repo's bk7258_irq.c
 *      already writes as BK7258_CPU1_IRQ_EN1 from
 *      up_enable_irq(BK7258_IRQ_YUVB) (EXTIRQ=58, 58-32=26, falling in
 *      the "32-63" bank) -- so this driver's existing up_enable_irq()
 *      call already covers it; only items 1-2 needed to be added here. */
#define BK7258_SYS_REG_BASE          0x44010000u

/* reg0x10 (SOC_SYS_REG_BASE + 0x10*4): pwd_vidp at bit[7]. */
#define BK7258_SYS_REG_0X10          (BK7258_SYS_REG_BASE + (0x10u << 2))
#define BK7258_SYS_PWD_VIDP          (1u << 7)

/* reg0xd (SOC_SYS_REG_BASE + 0xd*4): yuv_cken at bit[3] (sibling of the
 * cis_auxs_cken bit[9] this board's MCLK fix already uses in
 * bk7258_camera_imgsensor.c's BK7258_SYS_REG_0X0D), h264_cken at
 * bit[0].  bk_avdk_smp's yuv_buf_init_common() enables BOTH
 * "sys_drv_set_h264_clk_en(1)" and "sys_drv_set_yuv_buf_clk_en(1)"
 * even for pure YUV direct-capture mode (h264_mode=0) -- this driver
 * initially ported only the yuv_cken half and left h264_cken
 * unset, which (per the same undocumented-hardware-coupling pattern
 * as the MCLK/pwd_vidp fixes) turned out to still leave the DVP/JPEG
 * input pipeline producing zero line-done interrupts even with
 * yuv_cken alone set.  Matching the reference sequence exactly (both
 * bits set, in the same order: h264 first, then yuv) resolved it.
 * Not defending *why* h264_cken gates a YUV-only capture path with
 * any documented rationale -- this is copied verbatim from the only
 * implementation confirmed to work on this exact SoC family, per this
 * project's "cross-check against bk_avdk_smp, do not guess" policy. */
#define BK7258_SYS_REG_0X0D          (BK7258_SYS_REG_BASE + (0xdu << 2))
#define BK7258_SYS_YUV_CKEN          (1u << 3)
#define BK7258_SYS_H264_CKEN         (1u << 0)

static bk7258_yuv_buf_line_cb_t g_line_cb;
static void *g_line_cb_arg;
static uint32_t g_line_batch_bytes;
static uint32_t g_isr_fire_count;

static int bk7258_yuv_buf_isr(int irq, void *context, void *arg)
{
  uint32_t status = getreg32(YUV_BUF_INT_STATUS);

  g_isr_fire_count++;
  printf("bk7258_yuv_buf: isr fired #%u, int_status=0x%08x\n",
         (unsigned int)g_isr_fire_count, (unsigned int)status);

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
  uint32_t reg;

  /* Power on the video pipeline (clear the active-high "power-down"
   * bit) and enable YUV_BUF's own clock gate -- without these, the
   * module is completely inert (no register write here has any real
   * hardware effect, and the line-done interrupt this driver relies on
   * can never fire) even though the soft-reset pulse and NVIC/EXTIRQ
   * setup below succeed with no error indication of any kind.  See this
   * file's BK7258_SYS_REG_0X10/0X0D comment block for the full
   * evidence chain (found via a diagnostic ISR fire counter that
   * confirmed zero interrupts across a full capture attempt). Must
   * happen before the soft-reset pulse, matching
   * yuv_buf_init_common()'s ordering (power -> clock -> global_ctrl). */
  reg = getreg32(BK7258_SYS_REG_0X10);
  reg &= ~BK7258_SYS_PWD_VIDP;
  putreg32(reg, BK7258_SYS_REG_0X10);

  reg = getreg32(BK7258_SYS_REG_0X0D);
  reg |= BK7258_SYS_H264_CKEN | BK7258_SYS_YUV_CKEN;
  putreg32(reg, BK7258_SYS_REG_0X0D);

  printf("bk7258_yuv_buf: video power/clock enabled, reg0x10=0x%08x "
         "reg0x0d=0x%08x\n",
         (unsigned int)getreg32(BK7258_SYS_REG_0X10),
         (unsigned int)getreg32(BK7258_SYS_REG_0X0D));

  /* REVERTED (2026-08-07): a two-round hypothesis about global_ctrl's
   * soft_reset polarity being active-low (based on cross-referencing
   * bk_avdk_smp's yuv_buf_ll_init(), which writes 0 then 1 with no
   * delay between the two and leaves the register at 1) was tried and
   * reverted after BOTH attempts caused a full AP-core hang:
   *
   *   Round 1: soft_reset=1 AND clk_gate_bypass=1 together -> AP
   *   stopped responding entirely mid-stream-command; CP's IPC
   *   heartbeat monitor declared the AP dead after 6s of silence
   *   ("hrt:E():IPC[1]heartbeat timeout", "Assert at: mb_ipc_task:297"),
   *   AP link dropped back to CP CLI.
   *
   *   Round 2 (isolating the variable): soft_reset=1 ALONE, with
   *   clk_gate_bypass left untouched at its true hardware-reset
   *   default of 0 -- still hung identically (serial log cut off
   *   mid-printf of the very next diagnostic line, then the same
   *   "AP link down; returned to CP CLI").
   *
   * Since both the combined write and the soft_reset-only write
   * reproduced the same full-system hang, the "active-low" polarity
   * hypothesis itself is disproven -- per this project's debugging
   * rule of stopping to re-examine the architecture assumption after
   * 2+ same-direction attempts fail, rather than trying a third
   * variant (e.g. removing the up_udelay(10) between the two writes,
   * since yuv_buf_ll_init()'s two writes have no delay between them
   * at all -- a plausible third guess, but not attempted, because the
   * failure pattern (whole-system hang, not a camera-specific symptom)
   * suggests this register's true behavior on this specific SoC
   * revision may differ from what the struct/LL-layer naming and the
   * reference call sequence would suggest, and guessing a third
   * register-bit variant without new evidence would repeat the same
   * mistake this project's stated methodology already flags: "same
   * class of bug repeating" is not a license to keep guessing bit
   * values -- it is the signal to stop and gather independent evidence
   * (e.g. instrument with a pre-hang diagnostic dump of every other
   * live register, or bisect which of the two writes alone is
   * sufficient to hang, before touching this bit a third time).
   *
   * Restored to the original pulse semantics (write 1, delay, write 0
   * -- i.e. soft_reset ends at 0) that were in place before this
   * investigation and that were never observed to hang the system in
   * any prior run. The original "int_status stays 0x00000000" problem
   * this was trying to fix remains OPEN; see this file's other
   * comments (BK7258_SYS_REG_0X10/0X0D power/clock fix, ctrl register
   * full-field port) for what has already been ruled out, and the
   * PCLK/VSYNC software-poll diagnostic result (toggle_count=3978 on
   * PCLK, confirming a real DVP signal exists) for the evidence that
   * narrowed the problem to "YUV_BUF itself not recognizing a signal
   * that is genuinely present" -- global_ctrl's exact bit semantics on
   * this SoC revision remain an open question that will need either a
   * datasheet/register-map cross-check beyond what bk_avdk_smp's
   * source comments provide, or hardware-level register tracing (e.g.
   * a JTAG-attached debugger reading the live register value a
   * reference-firmware run leaves it at), rather than further
   * trial-and-error bit flips on real hardware. */
  putreg32(YUV_BUF_GLOBAL_CTRL_SOFT_RESET, YUV_BUF_GLOBAL_CTRL);
  up_udelay(10);
  putreg32(0, YUV_BUF_GLOBAL_CTRL);

  printf("bk7258_yuv_buf: global_ctrl=0x%08x (soft-reset pulse complete, "
         "reverted to original polarity after two hang-inducing "
         "polarity-flip attempts -- see comment above)\n",
         (unsigned int)getreg32(YUV_BUF_GLOBAL_CTRL));

  irq_attach(BK7258_IRQ_YUVB, bk7258_yuv_buf_isr, NULL);
  up_enable_irq(BK7258_IRQ_YUVB);
}

void bk7258_yuv_buf_configure(uint16_t width, uint16_t height)
{
  uint32_t x_pixel = width / 8u;
  uint32_t y_pixel = height / 8u;
  uint32_t frame_blk = (x_pixel * y_pixel) / 2u;
  uint32_t pixel_reg;
  uint32_t ctrl;
  uint32_t resize_reg;

  /* One ping-pong bank holds 8 lines of YUV422 (2 bytes/pixel) data,
   * matching bk_dvp.c's "yuv_config.yuv_pingpong_length = config->width
   * * 8 * 2".  Saved here so bk7258_yuv_buf_get_line_batch_bytes() can
   * report it without callers needing to duplicate this formula. */
  g_line_batch_bytes = (uint32_t)width * 8u * 2u;

  pixel_reg = (x_pixel << YUV_BUF_PIXEL_X_SHIFT) |
              (y_pixel << YUV_BUF_PIXEL_Y_SHIFT) |
              (frame_blk << YUV_BUF_PIXEL_FRAME_BLK_SHIFT);
  putreg32(pixel_reg, YUV_BUF_PIXEL);

  /* Full ctrl register field set, ported field-for-field from
   * yuv_buf_hal_set_config_common() (ap/middleware/soc/common/hal/
   * yuv_buf_hal.c) -- this driver previously only ever wrote
   * yuv_mode/h264_mode (bits 0/12) via bk7258_yuv_buf_start(), leaving
   * every other ctrl field at its post-soft-reset default of 0.  Two
   * of those defaults are wrong for this hardware:
   *   - mclk_div (bit[10:11]) defaults to 0 = YUV_MCLK_DIV_4, but
   *     bk_dvp.c's dvp_camera_yuv_buf_config_init() always sets
   *     YUV_MCLK_DIV_3 (value 3) regardless of sensor -- a wrong
   *     internal re-division ratio for YUV_BUF's sampling of the
   *     DVP bus could plausibly explain line-done interrupts never
   *     firing even with power/clock/soft-reset all correct.
   *   - sync_edge_dect_en (bit[9]) defaults to 0 = disabled, but the
   *     reference always enables it; with edge detection disabled the
   *     module may never notice HSYNC/VSYNC transitions at all, which
   *     would also fully explain zero interrupts.
   * yuv_fmt_sel/hsync_rev/vsync_rev are written explicitly too (even
   * though GC2145's PIXEL_FMT_YUYV/SYNC_HIGH_LEVEL/SYNC_HIGH_LEVEL
   * settings happen to match the all-zero reset default per
   * dvp_gc2145.c's dvp_sensor_gc2145) so this driver does not silently
   * depend on that coincidence if the sensor ever changes.
   * bps_cis/memrev are left disabled (0) and soi_hsync left at
   * posedge (1), matching yuv_buf_hal_set_config_common()'s
   * disable_bps_cis()/disable_memrev()/set_encode_begin_hsync_posedge()
   * calls. */
  ctrl = getreg32(YUV_BUF_CTRL);
  ctrl &= ~(0x3u << YUV_BUF_CTRL_YUV_FMT_SEL_SHIFT);
  ctrl &= ~(YUV_BUF_CTRL_HSYNC_REV | YUV_BUF_CTRL_VSYNC_REV |
            YUV_BUF_CTRL_MCLK_DIV_MASK | YUV_BUF_CTRL_BPS_CIS |
            YUV_BUF_CTRL_MEMREV);
  ctrl |= (0u << YUV_BUF_CTRL_YUV_FMT_SEL_SHIFT); /* YUV_FORMAT_YUYV = 0 */
  ctrl |= YUV_BUF_CTRL_SYNC_EDGE_DECT_EN;
  ctrl |= (YUV_BUF_MCLK_DIV_3 << YUV_BUF_CTRL_MCLK_DIV_SHIFT);
  ctrl |= YUV_BUF_CTRL_SOI_HSYNC;
  putreg32(ctrl, YUV_BUF_CTRL);

  resize_reg = (x_pixel << YUV_BUF_RESIZE_PIXEL_X_SHIFT) |
               (y_pixel << YUV_BUF_RESIZE_PIXEL_Y_SHIFT);
  putreg32(resize_reg, YUV_BUF_RESIZE_PIXEL);

  /* YUV direct-capture mode uses a fixed PSRAM line buffer address, not
   * an application-supplied one; see file header comment. */
  putreg32(YUV_BUF_PSRAM_LINE_BUF_ADDR, YUV_BUF_EM_BASE_ADDR);
  putreg32(YUV_BUF_PSRAM_LINE_BUF_ADDR, YUV_BUF_EMR_BASE_ADDR);

  /* Enable the full interrupt mask (0x1FF), matching
   * yuv_buf_ll_enable_int() exactly rather than only the two bits
   * (sm0_wr/sm1_wr) this driver actually consumes -- the reference
   * implementation never enables a narrower subset for YUV mode, and
   * since the actual root cause of "zero interrupts" turned out to be
   * elsewhere (ctrl register fields above), this is kept at parity
   * with the reference to eliminate any remaining doubt about int_en
   * itself being a contributing factor. */
  putreg32(0x1FFu, YUV_BUF_INT_EN);

  printf("bk7258_yuv_buf: configure ctrl=0x%08x pixel=0x%08x "
         "resize=0x%08x\n",
         (unsigned int)getreg32(YUV_BUF_CTRL),
         (unsigned int)getreg32(YUV_BUF_PIXEL),
         (unsigned int)getreg32(YUV_BUF_RESIZE_PIXEL));
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

void bk7258_yuv_buf_dump_status(void)
{
  printf("bk7258_yuv_buf: ctrl=0x%08x int_en=0x%08x int_status=0x%08x "
         "pixel=0x%08x em_base_addr=0x%08x isr_fire_count=%u\n",
         (unsigned int)getreg32(YUV_BUF_CTRL),
         (unsigned int)getreg32(YUV_BUF_INT_EN),
         (unsigned int)getreg32(YUV_BUF_INT_STATUS),
         (unsigned int)getreg32(YUV_BUF_PIXEL),
         (unsigned int)getreg32(YUV_BUF_EM_BASE_ADDR),
         (unsigned int)g_isr_fire_count);
}
