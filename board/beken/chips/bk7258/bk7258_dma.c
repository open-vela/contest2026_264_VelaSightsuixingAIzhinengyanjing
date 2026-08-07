/****************************************************************************
 * board/beken/chips/bk7258/bk7258_dma.c
 *
 * BK7258 minimal single-channel (channel 0), single-shot, memory-to-memory
 * DMA driver.  Register layout source: bk_avdk_smp release/v3.1.1
 * ap/middleware/soc/bk7258_ap/soc/dma_struct.h and hal/dma_ll.h.
 *
 * Register map (byte offset from BK7258_DMA_BASE):
 *   Channel N's 16-word config group starts at 0x40 + N*0x40.  This
 *   driver only uses channel 0:
 *     ctrl            word 0  - bit[0] enable, bit[1] finish_int_en,
 *                     bit[3] mode(0=SINGLE), bit[8] src_addr_inc_en,
 *                     bit[9] dest_addr_inc_en, bit[16:31] transfer_len
 *                     (encoded as byte_count - 1)
 *     dest_start_addr word 1
 *     src_start_addr  word 2
 *     status          word 12 - bit[0:16] remain_len, bit[18] finish_int
 *
 * req_mux (word 7) is not used: both endpoints are plain memory
 * addresses (DTCM-style memory-to-memory transfer), matching
 * bk_dvp.c's encode_yuv_dma_cpy().
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#include <nuttx/config.h>
#include <nuttx/arch.h>
#include <nuttx/irq.h>
#include <stdbool.h>
#include <stdint.h>

#include "arm_internal.h"
#include "irq.h"
#include "bk7258_dma.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define BK7258_DMA_BASE              0x45020000u  /* SOC_GENER_DMA_REG_BASE */
#define BK7258_DMA_CHANNEL           0u

#define BK7258_DMA_CHAN_GROUP_BASE   (BK7258_DMA_BASE + 0x40u)
#define BK7258_DMA_CHAN_STRIDE       0x40u  /* 16 words per channel */

#define BK7258_DMA_REG(word_off) \
  (BK7258_DMA_CHAN_GROUP_BASE + \
   BK7258_DMA_CHANNEL * BK7258_DMA_CHAN_STRIDE + (word_off) * 4u)

#define BK7258_DMA_CTRL              BK7258_DMA_REG(0)
#define BK7258_DMA_DEST_ADDR         BK7258_DMA_REG(1)
#define BK7258_DMA_SRC_ADDR          BK7258_DMA_REG(2)
#define BK7258_DMA_STATUS            BK7258_DMA_REG(12)

#define BK7258_DMA_CTRL_ENABLE           (1u << 0)
#define BK7258_DMA_CTRL_FINISH_INT_EN    (1u << 1)
#define BK7258_DMA_CTRL_MODE_SINGLE      (0u << 3)
#define BK7258_DMA_CTRL_SRC_INC_EN       (1u << 8)
#define BK7258_DMA_CTRL_DEST_INC_EN      (1u << 9)
#define BK7258_DMA_CTRL_LEN_SHIFT        16u
#define BK7258_DMA_CTRL_LEN_MASK         0xffffu

#define BK7258_DMA_STATUS_REMAIN_LEN_MASK 0x1ffffu
#define BK7258_DMA_STATUS_FINISH_INT      (1u << 18)

/****************************************************************************
 * Private Data
 ****************************************************************************/

static bk7258_dma_done_cb_t g_done_cb;
static void *g_done_cb_arg;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static int bk7258_dma_isr(int irq, void *context, void *arg)
{
  uint32_t status = getreg32(BK7258_DMA_STATUS);

  if ((status & BK7258_DMA_STATUS_FINISH_INT) != 0)
    {
      /* Disable the channel so a stale "finish" state does not
       * confuse the next bk7258_dma_configure() call.
       */

      modifyreg32(BK7258_DMA_CTRL, BK7258_DMA_CTRL_ENABLE, 0);

      if (g_done_cb != NULL)
        {
          g_done_cb(g_done_cb_arg);
        }
    }

  return 0;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

void bk7258_dma_init(void)
{
  modifyreg32(BK7258_DMA_CTRL, BK7258_DMA_CTRL_ENABLE, 0);

  irq_attach(BK7258_IRQ_DMA, bk7258_dma_isr, NULL);
  up_enable_irq(BK7258_IRQ_DMA);
}

void bk7258_dma_configure(uint32_t src_addr, uint32_t dest_addr,
                           uint32_t transfer_len)
{
  uint32_t ctrl;
  uint32_t len_field;

  /* Disable before reprogramming source/dest/length. */

  modifyreg32(BK7258_DMA_CTRL, BK7258_DMA_CTRL_ENABLE, 0);

  putreg32(src_addr, BK7258_DMA_SRC_ADDR);
  putreg32(dest_addr, BK7258_DMA_DEST_ADDR);

  /* ctrl.transfer_len encodes (byte_count - 1). */

  len_field = (transfer_len - 1u) & BK7258_DMA_CTRL_LEN_MASK;

  ctrl = BK7258_DMA_CTRL_MODE_SINGLE |
         BK7258_DMA_CTRL_SRC_INC_EN |
         BK7258_DMA_CTRL_DEST_INC_EN |
         BK7258_DMA_CTRL_FINISH_INT_EN |
         (len_field << BK7258_DMA_CTRL_LEN_SHIFT);

  putreg32(ctrl, BK7258_DMA_CTRL);
}

void bk7258_dma_set_done_callback(bk7258_dma_done_cb_t cb, void *arg)
{
  g_done_cb = cb;
  g_done_cb_arg = arg;
}

void bk7258_dma_start(void)
{
  modifyreg32(BK7258_DMA_CTRL, 0, BK7258_DMA_CTRL_ENABLE);
}

void bk7258_dma_stop(void)
{
  modifyreg32(BK7258_DMA_CTRL, BK7258_DMA_CTRL_ENABLE, 0);
}

bool bk7258_dma_is_busy(void)
{
  return (getreg32(BK7258_DMA_CTRL) & BK7258_DMA_CTRL_ENABLE) != 0;
}

uint32_t bk7258_dma_get_remain_len(void)
{
  return getreg32(BK7258_DMA_STATUS) & BK7258_DMA_STATUS_REMAIN_LEN_MASK;
}
