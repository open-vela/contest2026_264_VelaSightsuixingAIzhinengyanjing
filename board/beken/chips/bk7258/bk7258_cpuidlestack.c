/****************************************************************************
 * BK7258 secondary idle and per-CPU interrupt stacks.
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdint.h>
#include <sys/types.h>

#include <nuttx/arch.h>
#include <nuttx/sched.h>

#include "arm_internal.h"

#define BK7258_BOOTSTRAP_STACK_SIZE 1024u
#define BK7258_INTSTACK_ALLOC       (CONFIG_SMP_NCPUS * INTSTACK_SIZE)

static uint64_t g_bk7258_intstacks[BK7258_INTSTACK_ALLOC / sizeof(uint64_t)]
  __attribute__((section(".bk_intstacks"), aligned(32)));

static uint64_t
g_bk7258_secondary_bootstack[BK7258_BOOTSTRAP_STACK_SIZE / sizeof(uint64_t)]
  __attribute__((used, section(".bk_bootstack"), aligned(32)));

int up_cpu_idlestack(int cpu, struct tcb_s *tcb, size_t stack_size)
{
  DEBUGASSERT(cpu == 1);
  return up_create_stack(tcb, stack_size, TCB_FLAG_TTYPE_KERNEL);
}

uintptr_t up_get_intstackbase(int cpu)
{
  DEBUGASSERT((unsigned int)cpu < CONFIG_SMP_NCPUS);
  return (uintptr_t)g_bk7258_intstacks + (cpu * INTSTACK_SIZE);
}
