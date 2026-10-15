/****************************************************************************
 * board/beken/chips/bk7258/bk7258_saradc.c
 *
 * The general-purpose SARADC as a NuttX ADC lower half.  ANIOC_TRIGGER
 * converts each configured channel once and hands one averaged code per
 * channel to the upper half.  Only the two internal channels are accepted,
 * see include/bk7258_saradc.h.  Not the audio ADC, which is bk7258_aud.c.
 *
 * Sharing the block with the CP
 * -----------------------------
 * In the vendor design the CP owns this block.  Its adc_driver.c is the
 * only register driver, and two daemons in this project's CP build use it
 * (CONFIG_TEMP_DETECT, CONFIG_VOLT_DETECT): once a second each for the first
 * 30 s after boot, every 15 s after that.  They announce nothing -- the
 * mailbox broadcast that would, CONFIG_SARADC_MB, is off for the reasons in
 * docs/plans/BK7258_OPENVELA_ADC_PORTING_PLAN.md section 3.3 -- and the AP
 * cannot take the CP's mutex.  That plan's full answer is an mb_ipc client
 * of the CP's SARADC server (its route B), which remains open.
 *
 * This driver instead stays out of the CP's way, and then checks it did:
 *
 *  - It starts only on a visibly idle block.  Every CP measurement sets
 *    sadc_cken (sys_drv_sadc_pwr_up()) and clears it last
 *    (sys_drv_sadc_pwr_down()), with the SARADC interrupt enabled on CPU0
 *    while it waits for samples; the temperature one switches the die
 *    sensor on (ana_reg5 en_temp, temp_sensor_enable()) even before the
 *    clock.  Any of the three seen means the CP is in the middle of one,
 *    and this backs off SARADC_BACKOFF_US; a CP measurement takes about
 *    3 ms.
 *
 *  - Before it uses the samples it checks the block is as it left it: the
 *    clock on, the CP interrupt and the sensor off, mode, channel, divider
 *    and saturation as configured, and a FIFO level, SARADC_FIFO_MARK,
 *    that no CP configuration writes (it always programs 31).  A CP
 *    measurement that began in between changes at least one of those, so
 *    the samples are dropped and the hardware is left to the CP untouched,
 *    rather than averaged into a wrong reading.
 *
 * A collision therefore costs a retry, not a wrong value.  What is left are
 * the few microseconds between a check and the writes that follow it, where
 * the two cores can still interleave: at one reading a minute against a CP
 * that measures every 15 s, about one reading in a million.  The CP's own
 * daemons already ride out a failed measurement; they retry on their next
 * tick.
 *
 * Conversion
 * ----------
 * Polled continuous mode with the configuration the vendor's battery
 * monitor uses for VBAT (ap/components/bk_batt_monitor/bat_monitor.c):
 * XTAL 26 MHz / 128 = 203125 Hz, steady 7, saturation mode 3, calibration
 * bypassed -- one sample every 79 us after a 315 us power-on wait, and that
 * after SARADC_IDLE_US held in power-down (saradc_start() says why).  The
 * first SARADC_SKIP samples are dropped and the rest averaged, leaving out
 * 0 and 2048, which the vendor's volt_detect.c treats as invalid.  The
 * caller sleeps through the waits; interrupts are masked only around the
 * register work, a few microseconds at a time.
 *
 * ana_reg2 gets the vendor's front-end settings (sys_hal_set_saradc_config())
 * through the analog SPI with its busy poll, and the fields it changed are
 * put back afterwards.  ana_reg5's input divider is only checked: every CP
 * measurement leaves it where this driver needs it (1/5 on VBAT, 1/3
 * elsewhere), and writing it would mean taking the 1 V latch in ana_reg9
 * from under the CP.
 *
 * No power vote.  The block is in the BAKP domain with the TRNG and the
 * PWM, both of which this port drives without one.
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <nuttx/analog/adc.h>
#include <nuttx/analog/ioctl.h>
#include <nuttx/clock.h>
#include <nuttx/irq.h>
#include <nuttx/kmalloc.h>
#include <nuttx/mutex.h>
#include <nuttx/signal.h>

#include "arm_internal.h"

#include "hardware/bk7258_sysctrl.h"
#include "hardware/bk7258_saradc.h"
#include "bk7258_saradc.h"

#ifdef CONFIG_BK7258_SARADC

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define SARADC_MAX_CHANNELS   2

/* The configuration, see the file header. */

#define SARADC_DIV            63u    /* 26 MHz / (2 * 64) = 203125 Hz */
#define SARADC_STEADY         7u
#define SARADC_SAT_CTRL       3u
#define SARADC_FIFO_MARK      21u    /* any value but the CP's 31 */
#define SARADC_ANA5_DIV       1u     /* 1/5 on VBAT, 1/3 on the others */

#define SARADC_ANA2_GADC      (BK7258_ANA2_SP_NT_CTRL(3) | \
                               BK7258_ANA2_CAPCAL(0) | \
                               BK7258_ANA2_NOBUF_ENABLE | \
                               BK7258_ANA2_REFBUF_ICTRL(2) | \
                               BK7258_ANA2_INBUF_ICTRL(2) | \
                               BK7258_ANA2_CMP_ICTRL(2))

/* The CTRL fields that make up a configuration: everything except the
 * write-one interrupt clear and the read-only status bits.
 */

#define SARADC_CTRL_CONFIG    (BK7258_SARADC_MODE_MASK | BK7258_SARADC_EN | \
                               BK7258_SARADC_CHAN_MASK | \
                               BK7258_SARADC_SETTING_8 | \
                               BK7258_SARADC_DIV_MASK | BK7258_SARADC_32M | \
                               BK7258_SARADC_SAMP_MASK | \
                               BK7258_SARADC_FILTER_MASK)
#define SARADC_STEADY_CONFIG  (BK7258_SARADC_FIFO_LEVEL_MASK | \
                               BK7258_SARADC_STEADY_MASK | \
                               BK7258_SARADC_BYPASS_CALIB)
#define SARADC_SAT_CONFIG     (BK7258_SARADC_SAT_CTRL_MASK | \
                               BK7258_SARADC_SAT_ENABLE)

/* Samples per conversion, the dropped head included.  About 1.6 ms of
 * conversion, so SARADC_SETTLE_US usually finds them all waiting.
 */

#define SARADC_FIFO_DEPTH     32u
#define SARADC_SAMPLES        16u
#define SARADC_SKIP           5u
#define SARADC_MIN_VALID      4u

#define SARADC_IDLE_US        1000u  /* powered down before the enable */
#define SARADC_SETTLE_US      2000u
#define SARADC_POLL_US        1000u
#define SARADC_DEADLINE_MS    8u

/* Twenty tries 2 ms apart outlast any one CP measurement several times
 * over; running out means something is holding the block on purpose.
 */

#define SARADC_ATTEMPTS       20u
#define SARADC_BACKOFF_US     2000u

/* A conversion that ran to SARADC_DEADLINE_MS with the configuration intact
 * and too few samples is tried again, up to this many attempts in all.  A
 * safety net.  Before saradc_start() held the power-down (see there), the
 * first conversion after a CP temperature measurement could stall like
 * this; since, none has in 300 readings across the CP's 1 s boot window.
 */

#define SARADC_STALL_RETRIES  3u

#define SARADC_ANA_POLL_LIMIT 100000u

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct bk7258_saradc_s
{
  const struct adc_callback_s *cb;
  mutex_t lock;                     /* one conversion at a time */
  uint8_t nchannels;
  uint8_t chanlist[SARADC_MAX_CHANNELS];
  bool probed;                      /* DEV_ID has been reported */
};

struct bk7258_saradc_dev_s
{
  struct adc_dev_s dev;
  struct bk7258_saradc_s priv;
};

/* What a conversion changed outside the block, to be put back, and what the
 * registers held when it ended short of samples -- printed, because that is
 * a state no other log line can explain.
 */

struct saradc_claim_s
{
  uint32_t ana2;                    /* ana_reg2's GADC fields as found */
  bool cksel;                       /* the clock was not on the XTAL */
  uint32_t dev_id;
  uint32_t version;
  uint32_t found_ctrl;              /* CTRL before this driver wrote it */
  uint32_t found_ana2;

  bool short_run;                   /* the snapshot below is valid */
  uint32_t ctrl;
  uint32_t steady;
  uint32_t sat;
  uint32_t global;
  uint32_t status;
  uint32_t raw;
  uint32_t devclk;
  uint32_t clkdiv;
  uint32_t ana2_now;
  uint32_t ana5;
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static int saradc_bind(struct adc_dev_s *dev,
                       const struct adc_callback_s *callback);
static void saradc_reset(struct adc_dev_s *dev);
static int saradc_setup(struct adc_dev_s *dev);
static void saradc_shutdown(struct adc_dev_s *dev);
static void saradc_rxint(struct adc_dev_s *dev, bool enable);
static int saradc_ioctl(struct adc_dev_s *dev, int cmd, unsigned long arg);

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const struct adc_ops_s g_saradc_ops =
{
  .ao_bind     = saradc_bind,
  .ao_reset    = saradc_reset,
  .ao_setup    = saradc_setup,
  .ao_shutdown = saradc_shutdown,
  .ao_rxint    = saradc_rxint,
  .ao_ioctl    = saradc_ioctl,
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: saradc_ana_write
 *
 * Description:
 *   One analog register write, waiting for any transfer of the same
 *   register to retire first and for this one after.  Called inside a
 *   critical section, which also keeps the AP's other analog writer,
 *   bk7258_aud.c, from starting one in between.
 *
 ****************************************************************************/

static int saradc_ana_write(unsigned int reg, uint32_t value)
{
  uint32_t limit = SARADC_ANA_POLL_LIMIT;

  while ((getreg32(BK7258_SARADC_ANA_STATE) &
          BK7258_SARADC_ANA_BUSY(reg)) != 0)
    {
      if (--limit == 0)
        {
          return -ETIMEDOUT;
        }
    }

  putreg32(value, BK7258_SARADC_ANA(reg));

  limit = SARADC_ANA_POLL_LIMIT;
  while ((getreg32(BK7258_SARADC_ANA_STATE) &
          BK7258_SARADC_ANA_BUSY(reg)) != 0)
    {
      if (--limit == 0)
        {
          return -ETIMEDOUT;
        }
    }

  return OK;
}

/* The CP has the die sensor on for a temperature measurement -- set before
 * that measurement takes the clock, cleared before it gives it back -- or
 * is waiting on its SARADC interrupt.
 */

static bool saradc_cp_signs(void)
{
  return (getreg32(BK7258_SARADC_ANA(BK7258_ANA5)) &
          BK7258_ANA5_EN_TEMP) != 0 ||
         (getreg32(BK7258_SYS_CPU0_IRQ_EN0) &
          BK7258_SYS_CPU0_SADC_INT_EN) != 0;
}

/* The CP is anywhere between the start and the end of a measurement. */

static bool saradc_cp_busy(void)
{
  return (getreg32(BK7258_SYS_DEVCLK_EN) & BK7258_SYS_SADC_CKEN) != 0 ||
         saradc_cp_signs();
}

static uint32_t saradc_ctrl(unsigned int chan)
{
  return BK7258_SARADC_MODE_CONTINUOUS |
         ((uint32_t)chan << BK7258_SARADC_CHAN_SHIFT) |
         (SARADC_DIV << BK7258_SARADC_DIV_SHIFT);
}

static uint32_t saradc_steady(void)
{
  return SARADC_FIFO_MARK | (SARADC_STEADY << BK7258_SARADC_STEADY_SHIFT) |
         BK7258_SARADC_BYPASS_CALIB;
}

static uint32_t saradc_sat(void)
{
  return SARADC_SAT_CTRL | BK7258_SARADC_SAT_ENABLE;
}

/****************************************************************************
 * Name: saradc_still_ours
 *
 * Description:
 *   Whether the block still runs the conversion saradc_start() enabled.
 *   The clock is checked first so that the block itself is read only while
 *   it is clocked.
 *
 ****************************************************************************/

static bool saradc_still_ours(unsigned int chan)
{
  if ((getreg32(BK7258_SYS_DEVCLK_EN) & BK7258_SYS_SADC_CKEN) == 0 ||
      saradc_cp_signs())
    {
      return false;
    }

  return (getreg32(BK7258_SARADC_CTRL) & SARADC_CTRL_CONFIG) ==
         (saradc_ctrl(chan) | BK7258_SARADC_EN) &&
         (getreg32(BK7258_SARADC_STEADY) & SARADC_STEADY_CONFIG) ==
         saradc_steady() &&
         (getreg32(BK7258_SARADC_SAT) & SARADC_SAT_CONFIG) == saradc_sat();
}

/* Put the clock back.  Inside a critical section. */

static void saradc_unclock(const struct saradc_claim_s *claim)
{
  if (claim->cksel)
    {
      modifyreg32(BK7258_SYS_CLKDIV1, 0, BK7258_SYS_CKSEL_SADC);
    }

  modifyreg32(BK7258_SYS_DEVCLK_EN, BK7258_SYS_SADC_CKEN, 0);
}

/****************************************************************************
 * Name: saradc_release
 *
 * Description:
 *   End the conversion and put back what saradc_claim() changed: the block
 *   powered down, ana_reg2's GADC fields as they were -- written only if
 *   they differ, and without touching the crystal trim the CP owns in the
 *   same word -- and the clock off.  Inside a critical section, and only
 *   while saradc_still_ours() holds.
 *
 ****************************************************************************/

static void saradc_release(const struct saradc_claim_s *claim)
{
  uint32_t ana2;

  putreg32(BK7258_SARADC_INT_CLEAR, BK7258_SARADC_CTRL);

  ana2 = getreg32(BK7258_SARADC_ANA(BK7258_ANA2));
  if ((ana2 & BK7258_ANA2_GADC_MASK) != claim->ana2)
    {
      (void)saradc_ana_write(BK7258_ANA2,
                             (ana2 & ~BK7258_ANA2_GADC_MASK) | claim->ana2);
    }

  saradc_unclock(claim);
}

/****************************************************************************
 * Name: saradc_claim
 *
 * Description:
 *   Take an idle block and leave it clocked, configured and powered down,
 *   in one critical section.  saradc_start() enables it.
 *
 * Returned Value:
 *   OK; -EBUSY while the CP is using the block; -ERANGE when the input
 *   divider is not where every CP measurement leaves it; -ETIMEDOUT when
 *   the analog SPI does not retire the front-end write.
 *
 ****************************************************************************/

static int saradc_claim(struct saradc_claim_s *claim)
{
  irqstate_t flags;
  uint32_t ana2;
  uint32_t want;
  uint32_t div;
  int ret = OK;

  flags = enter_critical_section();

  if (saradc_cp_busy())
    {
      leave_critical_section(flags);
      return -EBUSY;
    }

  div = (getreg32(BK7258_SARADC_ANA(BK7258_ANA5)) &
         BK7258_ANA5_ADC_DIV_MASK) >> BK7258_ANA5_ADC_DIV_SHIFT;
  if (div != SARADC_ANA5_DIV)
    {
      leave_critical_section(flags);
      printf("saradc: input divider is %" PRIu32 ", want %u; the CP's "
             "next measurement restores it\n", div, SARADC_ANA5_DIV);
      return -ERANGE;
    }

  modifyreg32(BK7258_SYS_DEVCLK_EN, 0, BK7258_SYS_SADC_CKEN);

  claim->short_run  = false;
  claim->found_ctrl = getreg32(BK7258_SARADC_CTRL);
  claim->cksel = (getreg32(BK7258_SYS_CLKDIV1) &
                  BK7258_SYS_CKSEL_SADC) != 0;
  if (claim->cksel)
    {
      modifyreg32(BK7258_SYS_CLKDIV1, BK7258_SYS_CKSEL_SADC, 0);
    }

  ana2 = getreg32(BK7258_SARADC_ANA(BK7258_ANA2));
  claim->found_ana2 = ana2;
  claim->ana2 = ana2 & BK7258_ANA2_GADC_MASK;
  want = (ana2 & ~BK7258_ANA2_GADC_MASK) | SARADC_ANA2_GADC;
  if (want != ana2)
    {
      ret = saradc_ana_write(BK7258_ANA2, want);
    }

  if (ret < 0)
    {
      saradc_unclock(claim);
      leave_critical_section(flags);
      return ret;
    }

  claim->dev_id  = getreg32(BK7258_SARADC_DEV_ID);
  claim->version = getreg32(BK7258_SARADC_DEV_VERSION);

  /* The vendor's adc_hal_init() order, folded into whole register writes:
   * reset, then quiesce in power-down mode with the rest of the
   * configuration in place.  saradc_start() leaves it there for
   * SARADC_IDLE_US before it enables anything.
   */

  modifyreg32(BK7258_SARADC_GLOBAL_CTRL, 0, BK7258_SARADC_SOFT_RESET);
  putreg32(BK7258_SARADC_INT_CLEAR, BK7258_SARADC_CTRL);
  putreg32(saradc_steady(), BK7258_SARADC_STEADY);
  putreg32(saradc_sat(), BK7258_SARADC_SAT);

  leave_critical_section(flags);
  return OK;
}

/****************************************************************************
 * Name: saradc_start
 *
 * Description:
 *   Enable the conversion saradc_claim() prepared, if the block is still
 *   ours and still powered down.
 *
 *   The power-down has to last.  The CP ends a measurement with the mode
 *   left at continuous and only the enable cleared, and a power-down
 *   written and overwritten within a few bus cycles is evidently not seen
 *   by the converter.  Measured 2026-10-14 with that, in the first 30 s
 *   after boot when the CP measures every second: the first conversion
 *   after one of its temperature measurements either came back with no
 *   samples at all -- configuration intact, the raw register still holding
 *   the CP's last reading -- or, once in 300, 110 mV high; every
 *   conversion that found this driver's own release, which leaves the mode
 *   at power-down, was normal.  So the caller sleeps SARADC_IDLE_US
 *   between saradc_claim() and this, hundreds of converter clocks.
 *
 * Returned Value:
 *   OK, or -EAGAIN when the CP took the block meanwhile.
 *
 ****************************************************************************/

static int saradc_start(unsigned int chan)
{
  irqstate_t flags;
  unsigned int i;

  flags = enter_critical_section();

  if ((getreg32(BK7258_SYS_DEVCLK_EN) & BK7258_SYS_SADC_CKEN) == 0 ||
      saradc_cp_signs() ||
      (getreg32(BK7258_SARADC_CTRL) & SARADC_CTRL_CONFIG) != 0 ||
      (getreg32(BK7258_SARADC_STEADY) & SARADC_STEADY_CONFIG) !=
      saradc_steady() ||
      (getreg32(BK7258_SARADC_SAT) & SARADC_SAT_CONFIG) != saradc_sat())
    {
      leave_critical_section(flags);
      return -EAGAIN;
    }

  putreg32(saradc_ctrl(chan) | BK7258_SARADC_INT_CLEAR, BK7258_SARADC_CTRL);

  for (i = 0; i < SARADC_FIFO_DEPTH &&
              (getreg32(BK7258_SARADC_CTRL) &
               BK7258_SARADC_FIFO_EMPTY) == 0; i++)
    {
      (void)getreg32(BK7258_SARADC_DATA);
    }

  putreg32(saradc_ctrl(chan) | BK7258_SARADC_EN, BK7258_SARADC_CTRL);

  leave_critical_section(flags);
  return OK;
}

/****************************************************************************
 * Name: saradc_drain
 *
 * Description:
 *   Move what the FIFO holds into samples, provided the block is still
 *   ours.  Once there are enough, or when finish is set, also end the
 *   conversion -- in the same critical section as the check, so nothing the
 *   CP starts can land between the two.
 *
 * Returned Value:
 *   1 when the conversion is over, 0 to come back for more, -EAGAIN when
 *   the CP took the block, which is then left alone.
 *
 ****************************************************************************/

static int saradc_drain(unsigned int chan, struct saradc_claim_s *claim,
                        uint16_t *samples, unsigned int *count, bool finish)
{
  irqstate_t flags;
  unsigned int n = *count;
  int ret = -EAGAIN;

  flags = enter_critical_section();

  if (saradc_still_ours(chan))
    {
      while (n < SARADC_SAMPLES &&
             (getreg32(BK7258_SARADC_CTRL) & BK7258_SARADC_FIFO_EMPTY) == 0)
        {
          samples[n++] = (uint16_t)(getreg32(BK7258_SARADC_DATA) &
                                    BK7258_SARADC_DATA_MASK);
        }

      ret = 0;
      if (n >= SARADC_SAMPLES || finish)
        {
          if (n < SARADC_SAMPLES)
            {
              claim->short_run = true;
              claim->ctrl     = getreg32(BK7258_SARADC_CTRL);
              claim->steady   = getreg32(BK7258_SARADC_STEADY);
              claim->sat      = getreg32(BK7258_SARADC_SAT);
              claim->global   = getreg32(BK7258_SARADC_GLOBAL_CTRL);
              claim->status   = getreg32(BK7258_SARADC_BASE + 0x0cu);
              claim->raw      = getreg32(BK7258_SARADC_BASE + 0x14u);
              claim->devclk   = getreg32(BK7258_SYS_DEVCLK_EN);
              claim->clkdiv   = getreg32(BK7258_SYS_CLKDIV1);
              claim->ana2_now = getreg32(BK7258_SARADC_ANA(BK7258_ANA2));
              claim->ana5     = getreg32(BK7258_SARADC_ANA(BK7258_ANA5));
            }

          saradc_release(claim);
          ret = 1;
        }
    }

  leave_critical_section(flags);

  *count = n;
  return ret;
}

/****************************************************************************
 * Name: saradc_try
 *
 * Description:
 *   One attempt at converting chan.
 *
 ****************************************************************************/

static int saradc_try(struct bk7258_saradc_s *priv, unsigned int chan,
                      int32_t *code)
{
  struct saradc_claim_s claim;
  uint16_t samples[SARADC_SAMPLES];
  unsigned int count = 0;
  unsigned int valid = 0;
  unsigned int i;
  uint32_t sum = 0;
  clock_t deadline;
  int ret;

  memset(&claim, 0, sizeof(claim));
  ret = saradc_claim(&claim);
  if (ret < 0)
    {
      return ret;
    }

  nxsig_usleep(SARADC_IDLE_US);

  ret = saradc_start(chan);
  if (ret < 0)
    {
      return ret;
    }

  deadline = clock_systime_ticks() + MSEC2TICK(SARADC_DEADLINE_MS);
  nxsig_usleep(SARADC_SETTLE_US);

  for (; ; )
    {
      ret = saradc_drain(chan, &claim, samples, &count,
                         clock_systime_ticks() >= deadline);
      if (ret != 0)
        {
          break;
        }

      nxsig_usleep(SARADC_POLL_US);
    }

  if (ret < 0)
    {
      return ret;
    }

  if (!priv->probed)
    {
      priv->probed = true;
      printf("saradc: dev_id=0x%08" PRIx32 " version=0x%08" PRIx32 "\n",
             claim.dev_id, claim.version);
    }

  for (i = SARADC_SKIP; i < count; i++)
    {
      if (samples[i] != 0 && samples[i] != 2048)
        {
          sum += samples[i];
          valid++;
        }
    }

  if (claim.short_run)
    {
      printf("saradc: channel %u: %u of %u samples in %u ms; found ctrl "
             "0x%08" PRIx32 " ana2 0x%08" PRIx32 " cksel %d; left ctrl "
             "0x%08" PRIx32 " steady 0x%08" PRIx32 " sat 0x%08" PRIx32
             " glb 0x%08" PRIx32 " st 0x%08" PRIx32 " raw 0x%08" PRIx32
             " clk 0x%08" PRIx32 " div 0x%08" PRIx32 " ana2 0x%08" PRIx32
             " ana5 0x%08" PRIx32 "\n", chan, count, SARADC_SAMPLES,
             SARADC_DEADLINE_MS, claim.found_ctrl, claim.found_ana2,
             claim.cksel, claim.ctrl, claim.steady, claim.sat,
             claim.global, claim.status, claim.raw, claim.devclk,
             claim.clkdiv, claim.ana2_now, claim.ana5);
    }

  /* Too few samples with the configuration intact is a stall, tried again
   * by the caller; enough samples that are mostly invalid is a fault.
   */

  if (count < SARADC_SKIP + SARADC_MIN_VALID)
    {
      return -ENODATA;
    }

  if (valid < SARADC_MIN_VALID)
    {
      printf("saradc: channel %u gave %u samples, %u usable\n", chan,
             count, valid);
      return -EIO;
    }

  *code = (int32_t)((sum + valid / 2u) / valid);
  return OK;
}

/****************************************************************************
 * Name: saradc_convert
 *
 * Description:
 *   Convert chan, waiting out the CP.  A line is printed whenever the CP
 *   overlapped a conversion -- that is rare by construction, and the count
 *   is the evidence that the check above is doing its job.
 *
 ****************************************************************************/

static int saradc_convert(struct bk7258_saradc_s *priv, unsigned int chan,
                          int32_t *code)
{
  unsigned int backoffs = 0;
  unsigned int collisions = 0;
  unsigned int stalls = 0;
  unsigned int attempt;
  int ret = -EBUSY;

  for (attempt = 0; attempt < SARADC_ATTEMPTS; attempt++)
    {
      ret = saradc_try(priv, chan, code);
      if (ret == -EBUSY)
        {
          backoffs++;
        }
      else if (ret == -EAGAIN)
        {
          collisions++;
        }
      else if (ret != -ENODATA || ++stalls >= SARADC_STALL_RETRIES)
        {
          break;
        }

      nxsig_usleep(SARADC_BACKOFF_US);
    }

  if (ret == -EBUSY || ret == -EAGAIN)
    {
      printf("saradc: channel %u still held by the CP after %u tries "
             "(clk 0x%08" PRIx32 " cp_irq 0x%08" PRIx32 " ana5 0x%08"
             PRIx32 ")\n", chan, attempt, getreg32(BK7258_SYS_DEVCLK_EN),
             getreg32(BK7258_SYS_CPU0_IRQ_EN0),
             getreg32(BK7258_SARADC_ANA(BK7258_ANA5)));
      ret = -EBUSY;
    }
  else if (collisions != 0 || stalls != 0)
    {
      printf("saradc: channel %u: %u conversion(s) overlapped by the CP, "
             "%u stalled, %u back-off(s), result %d\n", chan, collisions,
             stalls, backoffs, ret);
    }

  return ret == -ENODATA ? -EIO : ret;
}

/****************************************************************************
 * Name: saradc_bind ... saradc_rxint
 *
 * Description:
 *   Nothing to set up or take down: between conversions the hardware is
 *   the CP's, so every conversion configures it from scratch and leaves it
 *   powered down.  No interrupt either; this is a polled driver.
 *
 ****************************************************************************/

static int saradc_bind(struct adc_dev_s *dev,
                       const struct adc_callback_s *callback)
{
  struct bk7258_saradc_s *priv = dev->ad_priv;

  priv->cb = callback;
  return OK;
}

static void saradc_reset(struct adc_dev_s *dev)
{
  (void)dev;
}

static int saradc_setup(struct adc_dev_s *dev)
{
  (void)dev;
  return OK;
}

static void saradc_shutdown(struct adc_dev_s *dev)
{
  (void)dev;
}

static void saradc_rxint(struct adc_dev_s *dev, bool enable)
{
  (void)dev;
  (void)enable;
}

/****************************************************************************
 * Name: saradc_ioctl
 *
 * Description:
 *   ANIOC_TRIGGER converts every channel and queues the codes; nothing is
 *   queued unless all of them converted.  ANIOC_GET_NCHANNELS reports the
 *   channel count.
 *
 ****************************************************************************/

static int saradc_ioctl(struct adc_dev_s *dev, int cmd, unsigned long arg)
{
  struct bk7258_saradc_s *priv = dev->ad_priv;
  int32_t codes[SARADC_MAX_CHANNELS];
  irqstate_t flags;
  unsigned int i;
  int ret;

  (void)arg;

  switch (cmd)
    {
      case ANIOC_TRIGGER:
        ret = nxmutex_lock(&priv->lock);
        if (ret < 0)
          {
            return ret;
          }

        for (i = 0; i < priv->nchannels && ret >= 0; i++)
          {
            ret = saradc_convert(priv, priv->chanlist[i], &codes[i]);
          }

        nxmutex_unlock(&priv->lock);

        if (ret < 0)
          {
            return ret;
          }

        if (priv->cb == NULL || priv->cb->au_receive == NULL)
          {
            return -ENODEV;
          }

        /* The upper half's FIFO is otherwise updated from interrupt
         * context, and its reader masks interrupts around it.
         */

        for (i = 0; i < priv->nchannels; i++)
          {
            flags = enter_critical_section();
            ret = priv->cb->au_receive(dev, priv->chanlist[i], codes[i]);
            leave_critical_section(flags);
            if (ret < 0)
              {
                return ret;
              }
          }

        return OK;

      case ANIOC_GET_NCHANNELS:
        return priv->nchannels;

      default:
        return -ENOTTY;
    }
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

struct adc_dev_s *bk7258_saradc_initialize(const uint8_t *chanlist,
                                           unsigned int nchannels)
{
  struct bk7258_saradc_dev_s *adc;
  unsigned int i;

  if (chanlist == NULL || nchannels == 0 ||
      nchannels > SARADC_MAX_CHANNELS)
    {
      return NULL;
    }

  for (i = 0; i < nchannels; i++)
    {
      if (chanlist[i] != BK7258_SARADC_CH_VBAT &&
          chanlist[i] != BK7258_SARADC_CH_VTEMP)
        {
          printf("saradc: channel %u is not an internal one\n",
                 chanlist[i]);
          return NULL;
        }
    }

  /* From the heap rather than static: this is registered late in bring-up,
   * after PSRAM is online, and static data here would come out of the
   * boot-time heap instead (SKILLS.md 3.7).
   */

  adc = kmm_zalloc(sizeof(*adc));
  if (adc == NULL)
    {
      return NULL;
    }

  nxmutex_init(&adc->priv.lock);
  adc->priv.nchannels = (uint8_t)nchannels;
  memcpy(adc->priv.chanlist, chanlist, nchannels);

  adc->dev.ad_ops  = &g_saradc_ops;
  adc->dev.ad_priv = &adc->priv;
  return &adc->dev;
}

#endif /* CONFIG_BK7258_SARADC */
