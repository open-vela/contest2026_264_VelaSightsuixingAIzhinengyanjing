/****************************************************************************
 * BK7258 CPU2 startup.
 ****************************************************************************/

#include <nuttx/config.h>

#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <syslog.h>

#include <nuttx/arch.h>
#include <nuttx/atomic.h>
#include <nuttx/clock.h>
#include <nuttx/sched.h>
#include <nuttx/sched_note.h>

#include "arm_internal.h"
#include "dwt.h"
#include "init/init.h"
#include "nvic.h"
#include "sched/sched.h"
#include "hardware/bk7258_memorymap.h"
#include "hardware/bk7258_mbox.h"
#include "hardware/bk7258_sysctrl.h"
#include "bk7258_boottrace.h"
#include "bk7258_smp.h"

#if CONFIG_SMP_NCPUS != 2
#  error "BK7258 OpenVela SMP supports exactly two AP CPUs"
#endif

#if CONFIG_BK7258_CPU_FREQ_HZ != 480000000
#  error "BK7258 OpenVela SMP requires the fixed 480 MHz CP contract"
#endif

#define BK7258_CPU2_ONLINE_CYCLES  (CONFIG_BK7258_CPU_FREQ_HZ / 2u)
#define BK7258_CPU2_POLL_LIMIT     4000000u

extern uint8_t __secondary_vectors_start[];
extern uint8_t __secondary_boot_stack_base[];

static void bk7258_cpu2_hold_reset(void)
{
  putreg32(getreg32(BK7258_CPU2_CONTROL) & ~BK7258_CPU2_RESET_RELEASE,
           BK7258_CPU2_CONTROL);
  UP_DSB();
}

static void bk7258_cpu2_release_reset(void)
{
  putreg32(getreg32(BK7258_CPU2_CONTROL) | BK7258_CPU2_RESET_RELEASE,
           BK7258_CPU2_CONTROL);
  UP_DSB();
  __asm__ volatile("sev" ::: "memory");
}

static void __attribute__((used, noreturn)) bk7258_secondary_idle(void)
{
  bk7258_boottrace_secondary(BK7258_BOOT_SECONDARY_IDLE);
  arm_initialize_stack();
  bk7258_boottrace_secondary(BK7258_BOOT_SECONDARY_STACK);
  bk7258_boottrace_secondary(BK7258_BOOT_SECONDARY_ONLINE);
  bk7258_smp_boot_notify();
  bk7258_boottrace_secondary(BK7258_BOOT_SECONDARY_NOTIFY);
  bk7258_boottrace_secondary(BK7258_BOOT_SECONDARY_IRQ_ON);
  up_irq_enable();
  sched_note_cpu_started(this_task());
  bk7258_boottrace_secondary(BK7258_BOOT_SECONDARY_SCHED);

  while (!bk7258_smp_boot_ping_received())
    {
      __asm__ volatile("nop");
    }

  bk7258_boottrace_secondary(BK7258_BOOT_SECONDARY_WAIT);
  bk7258_smp_boot_ack();
  bk7258_smp_secondary_ready();
  nx_idle_trampoline();
  PANIC();
}

static void __attribute__((naked, noreturn))
bk7258_secondary_enter(uintptr_t idle_top __attribute__((unused)),
                       uintptr_t idle_base __attribute__((unused)))
{
  __asm__ volatile
    (
      "msr psp, r0\n"
#ifdef CONFIG_ARMV8M_STACKCHECK_HARDWARE
      "msr psplim, r1\n"
#endif
      "mrs r1, control\n"
      "orr r1, r1, #2\n"
      "msr control, r1\n"
      "isb sy\n"
      "b bk7258_secondary_idle\n"
    );
}

static void __attribute__((used, noinline, noreturn,
                           target("general-regs-only")))
bk7258_secondary_boot(void)
{
  struct tcb_s *tcb;
  uintptr_t idle_top;

  bk7258_boottrace_secondary(BK7258_BOOT_SECONDARY_ENTRY);
  bk7258_cpu_private_initialize(false);
  bk7258_boottrace_secondary(BK7258_BOOT_SECONDARY_PRIVATE);
  bk7258_irqinitialize_secondary();
  bk7258_boottrace_secondary(BK7258_BOOT_SECONDARY_IRQ);
  if (bk7258_mbox_secondary_init() < 0)
    {
      PANIC();
    }

  bk7258_boottrace_secondary(BK7258_BOOT_SECONDARY_MBOX);

  tcb = current_task(1);
  idle_top = ((uintptr_t)tcb->stack_base_ptr + tcb->adj_stack_size) & ~7u;
  bk7258_secondary_enter(idle_top, (uintptr_t)tcb->stack_base_ptr);
}

void __attribute__((naked, noreturn, section(".start_text")))
bk7258_secondary_start(void)
{
  __asm__ volatile
    (
      "cpsid i\n"
      "movs r0, #1\n"
      "ldr r1, =0x20000000\n"
      "str r0, [r1]\n"
      "ldr r0, =__secondary_boot_stack_base\n"
      "msr msplim, r0\n"
      "b bk7258_secondary_boot\n"
    );
}

int up_cpu_start(int cpu)
{
  irqstate_t flags;
  uint32_t started;
  uint32_t polls = 0;
  uint32_t control;
  uint32_t status;
  bool ping_sent = false;

  bk7258_boottrace_primary(BK7258_BOOT_CPU2_ENTER);

  if (cpu != 1 || this_cpu() != 0)
    {
      return -EINVAL;
    }

  status = getreg32(BK7258_CPU_RUN_STATUS);
  bk7258_boottrace_detail(0, status);
  bk7258_boottrace_detail(1, bk7258_mbox_smp_ready());
  if ((status & BK7258_CPU_STATUS_CPU2_POWER_DOWN) != 0 ||
      !bk7258_mbox_smp_ready())
    {
      syslog(LOG_ERR, "CPU2 prerequisite failed: status=%08lx mbox=%u\n",
             (unsigned long)status, bk7258_mbox_smp_ready());
      PANIC();
    }

  bk7258_cpu2_hold_reset();
  control = getreg32(BK7258_CPU2_CONTROL);
  status = getreg32(BK7258_CPU_RUN_STATUS);
  if ((control & BK7258_CPU2_RESET_RELEASE) != 0 ||
      (status & BK7258_CPU_STATUS_CPU2_RESET) != 0)
    {
      syslog(LOG_ERR, "CPU2 reset hold failed: ctrl=%08lx status=%08lx\n",
             (unsigned long)control, (unsigned long)status);
      PANIC();
    }

  bk7258_boottrace_primary(BK7258_BOOT_CPU2_HELD);
  bk7258_boottrace_detail(0, control);
  bk7258_boottrace_detail(1, status);

  bk7258_smp_prepare_boot();
  putreg32(0, BK7258_CPU2_IRQ_EN0);
  putreg32(0, BK7258_CPU2_IRQ_EN1);
  modifyreg32(BK7258_CPU2_CONTROL, BK7258_CPU2_OFFSET_MASK,
              BK7258_CPU2_RXEVT_SEL |
              (((uintptr_t)__secondary_vectors_start >> 8) <<
               BK7258_CPU2_OFFSET_SHIFT));
  UP_DMB();
  UP_DSB();

  putreg32(getreg32(NVIC_DEMCR) | NVIC_DEMCR_TRCENA, NVIC_DEMCR);
  putreg32(0, DWT_CYCCNT);
  putreg32(getreg32(DWT_CTRL) | DWT_CTRL_CYCCNTENA_MASK, DWT_CTRL);
  started = getreg32(DWT_CYCCNT);
  flags = up_irq_save();
  bk7258_cpu2_release_reset();
  bk7258_boottrace_primary(BK7258_BOOT_CPU2_RELEASED);
  bk7258_boottrace_detail(0, getreg32(BK7258_CPU2_CONTROL));
  bk7258_boottrace_detail(1, getreg32(BK7258_CPU_RUN_STATUS));

  while (!bk7258_smp_boot_test_complete())
    {
      bk7258_mbox_kick_rx();
      if (bk7258_smp_boot_notified() && !ping_sent)
        {
          if (bk7258_smp_boot_ping(1) < 0)
            {
              break;
            }

          ping_sent = true;
          bk7258_boottrace_primary(BK7258_BOOT_CPU2_ONLINE);
        }

      if (getreg32(DWT_CYCCNT) - started >= BK7258_CPU2_ONLINE_CYCLES ||
          ++polls >= BK7258_CPU2_POLL_LIMIT)
        {
          break;
        }
    }

  if (!bk7258_smp_boot_test_complete())
    {
      bk7258_boottrace_detail(0, getreg32(BK7258_CPU2_CONTROL));
      bk7258_boottrace_detail(1, getreg32(BK7258_CPU_RUN_STATUS));
      bk7258_boottrace_detail(2, getreg32(BK7258_MBOX_CH1_STATUS));
      bk7258_boottrace_detail(3, getreg32(BK7258_MBOX_CH2_STATUS));
      bk7258_boottrace_detail(4, ping_sent);
      bk7258_cpu2_hold_reset();
      up_irq_restore(flags);
      syslog(LOG_ERR,
             "CPU2 online timeout: ctrl=%08lx status=%08lx fifo=%08lx/%08lx ping=%u\n",
             (unsigned long)getreg32(BK7258_CPU2_CONTROL),
             (unsigned long)getreg32(BK7258_CPU_RUN_STATUS),
             (unsigned long)getreg32(BK7258_MBOX_CH1_STATUS),
             (unsigned long)getreg32(BK7258_MBOX_CH2_STATUS), ping_sent);
      PANIC();
    }

  bk7258_boottrace_primary(BK7258_BOOT_CPU2_IPI_DONE);
  bk7258_boottrace_detail(2, UINT32_MAX);
  putreg32(1u << (BK7258_EXTIRQ_MAILBOX & 31),
           NVIC_IRQ_CLRPEND(BK7258_EXTIRQ_MAILBOX));
  UP_DSB();
  bk7258_boottrace_primary(BK7258_BOOT_CPU2_IRQ_RESTORE);
  up_irq_restore(flags);
  bk7258_boottrace_primary(BK7258_BOOT_CPU2_IRQ_RETURN);
  bk7258_boottrace_primary(BK7258_BOOT_CPU2_START_DONE);
  return OK;
}
