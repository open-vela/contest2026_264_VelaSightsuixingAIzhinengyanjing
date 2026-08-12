/****************************************************************************
 * BK7258 AP_SPINLOCK-backed software hwspinlock.
 ****************************************************************************/

#include <nuttx/config.h>

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>

#include <nuttx/hwspinlock/hwspinlock.h>

#include "arm_internal.h"
#include "bk7258_boottrace.h"

#define BK7258_GATE_TRACE_ENTRY       0xa1000000u
#define BK7258_GATE_TRACE_REENTER     0xa2000000u
#define BK7258_GATE_TRACE_OWNER_BUSY  0xa3000000u
#define BK7258_GATE_TRACE_INTENT      0xa4000000u
#define BK7258_GATE_TRACE_CONFLICT    0xa5000000u
#define BK7258_GATE_TRACE_ACQUIRED    0xa6000000u
#define BK7258_GATE_TRACE_UNLOCK      0xa7000000u
#define BK7258_GATE_TRACE_RELEASED    0xa8000000u

struct bk7258_atomic_gate_s
{
  volatile uint32_t interested[2];
  volatile uint32_t turn;
  volatile uint32_t owner;
  volatile uint32_t depth;
};

static struct bk7258_atomic_gate_s g_bk7258_atomic_gate
  __attribute__((section(".bk_spinlock"), aligned(32)));

static inline void bk7258_hwspinlock_trace(unsigned int cpu, uint32_t value)
{
  if (cpu == 0 && (bk7258_boottrace_words()[6] & 0xffu) == 2u)
    {
      bk7258_boottrace_detail(4, value);
    }
}

static bool bk7258_hwspinlock_trylock(struct hwspinlock_dev_s *dev)
{
  unsigned int cpu = (unsigned int)up_cpu_index();
  unsigned int other = cpu ^ 1u;

  (void)dev;
  DEBUGASSERT(cpu < 2);
  bk7258_hwspinlock_trace(cpu, BK7258_GATE_TRACE_ENTRY);

  /* BK7258 has only two SRAM exclusive monitors while CP, AP CPU1 and AP
   * CPU2 are all active.  Keep the AP-only atomic gate independent of
   * LDAEX/STREX so CP traffic cannot starve an AP exclusive sequence.
   */

  /* libc atomics are normally short, but the scheduler's recursive critical
   * section can issue another atomic operation on the same CPU.  Re-entry
   * must be decided before entering Peterson arbitration; otherwise a waiter
   * on the other CPU can make the current owner wait for itself.
   */

  if (g_bk7258_atomic_gate.owner == cpu + 1u)
    {
      if (g_bk7258_atomic_gate.depth == UINT32_MAX)
        {
          PANIC();
        }

      g_bk7258_atomic_gate.depth++;
      UP_DMB();
      bk7258_hwspinlock_trace(cpu, BK7258_GATE_TRACE_REENTER |
                             g_bk7258_atomic_gate.depth);
      return true;
    }

  /* This is only a fast rejection.  The Peterson protocol below remains
   * authoritative while a new owner is between intent and owner publish.
   */

  if (g_bk7258_atomic_gate.owner != 0)
    {
      bk7258_hwspinlock_trace(cpu, BK7258_GATE_TRACE_OWNER_BUSY |
                             g_bk7258_atomic_gate.owner);
      return false;
    }

  /* Every attempt must publish a fresh intent and turn.  Leaving intent set
   * after a failed trylock makes the state stale and breaks trylock's
   * one-attempt contract.
   */

  g_bk7258_atomic_gate.interested[cpu] = 1;
  UP_DMB();
  g_bk7258_atomic_gate.turn = other;
  UP_DMB();
  bk7258_hwspinlock_trace(cpu, BK7258_GATE_TRACE_INTENT);

  if (g_bk7258_atomic_gate.interested[other] != 0 &&
      g_bk7258_atomic_gate.turn == other)
    {
      g_bk7258_atomic_gate.interested[cpu] = 0;
      UP_DMB();
      __asm__ volatile("sev" ::: "memory");
      bk7258_hwspinlock_trace(cpu, BK7258_GATE_TRACE_CONFLICT);
      return false;
    }

  g_bk7258_atomic_gate.owner = cpu + 1u;
  g_bk7258_atomic_gate.depth = 1;
  UP_DMB();
  bk7258_hwspinlock_trace(cpu, BK7258_GATE_TRACE_ACQUIRED);
  return true;
}

static void bk7258_hwspinlock_relax(struct hwspinlock_dev_s *dev)
{
  (void)dev;
  __asm__ volatile("nop" ::: "memory");
}

static void bk7258_hwspinlock_unlock(struct hwspinlock_dev_s *dev)
{
  unsigned int cpu = (unsigned int)up_cpu_index();

  (void)dev;
  if (cpu >= 2 || g_bk7258_atomic_gate.owner != cpu + 1u ||
      g_bk7258_atomic_gate.depth == 0)
    {
      PANIC();
    }

  if (--g_bk7258_atomic_gate.depth != 0)
    {
      UP_DMB();
      bk7258_hwspinlock_trace(cpu, BK7258_GATE_TRACE_UNLOCK |
                             g_bk7258_atomic_gate.depth);
      return;
    }

  g_bk7258_atomic_gate.owner = 0;
  UP_DMB();
  g_bk7258_atomic_gate.interested[cpu] = 0;
  UP_DSB();
  __asm__ volatile("sev" ::: "memory");
  bk7258_hwspinlock_trace(cpu, BK7258_GATE_TRACE_RELEASED);
}

const struct hwspinlock_ops_s g_bk7258_hwspinlock_ops =
{
  .trylock = bk7258_hwspinlock_trylock,
  .relax   = bk7258_hwspinlock_relax,
  .unlock  = bk7258_hwspinlock_unlock
};

void bk7258_hwspinlock_initialize(void)
{
  g_bk7258_atomic_gate.interested[0] = 0;
  g_bk7258_atomic_gate.interested[1] = 0;
  g_bk7258_atomic_gate.turn = 0;
  g_bk7258_atomic_gate.owner = 0;
  g_bk7258_atomic_gate.depth = 0;
  UP_DSB();
  __asm__ volatile("sev" ::: "memory");
}
