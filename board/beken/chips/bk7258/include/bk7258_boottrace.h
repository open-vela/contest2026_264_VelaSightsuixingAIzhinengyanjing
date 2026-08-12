/****************************************************************************
 * BK7258 AP boot trace visible to the CP without mailbox services.
 ****************************************************************************/

#ifndef __VENDOR_BEKEN_CHIPS_BK7258_INCLUDE_BK7258_BOOTTRACE_H
#define __VENDOR_BEKEN_CHIPS_BK7258_INCLUDE_BK7258_BOOTTRACE_H

#include <stdint.h>

#define BK7258_BOOTTRACE_BASE       0x2800ffe0u
#define BK7258_BOOTTRACE_MAGIC      0x41505452u

#define BK7258_BOOT_PRIMARY_ENTRY   1u
#define BK7258_BOOT_PRIMARY_C       2u
#define BK7258_BOOT_PRIMARY_PRIVATE 3u
#define BK7258_BOOT_PRIMARY_NXSTART 4u
#define BK7258_BOOT_PRIMARY_CPACR   5u
#define BK7258_BOOT_PRIMARY_DATA    6u
#define BK7258_BOOT_PRIMARY_BSS     7u
#define BK7258_BOOT_PRIMARY_VECTORS 8u
#define BK7258_BOOT_PRIMARY_SPINLOCK 9u
#define BK7258_BOOT_SERIAL_ENTER    10u
#define BK7258_BOOT_SERIAL_LOGICAL  11u
#define BK7258_BOOT_SERIAL_UART     12u
#define BK7258_BOOT_SERIAL_PHYSICAL 13u
#define BK7258_BOOT_SERIAL_DONE     14u
#define BK7258_BOOT_CPU2_ENTER      20u
#define BK7258_BOOT_CPU2_HELD       21u
#define BK7258_BOOT_CPU2_RELEASED   22u
#define BK7258_BOOT_CPU2_ONLINE     23u
#define BK7258_BOOT_CPU2_IPI_DONE   24u
#define BK7258_BOOT_CPU2_IRQ_RESTORE 25u
#define BK7258_BOOT_CPU2_FIRST_IRQ   26u
#define BK7258_BOOT_CPU2_IRQ_RETURN  27u
#define BK7258_BOOT_CPU2_START_DONE  28u
#define BK7258_BOOT_SYSTICK_STARTED   29u
#define BK7258_BOOT_BRINGUP_ENTER   30u
#define BK7258_BOOT_WORKERS_READY   31u
#define BK7258_BOOT_LINK_READY      32u
#define BK7258_BOOT_PWC_ENTER       33u
#define BK7258_BOOT_READY_SENT      34u
#define BK7258_BOOT_TIMER_RETURN    35u
#define BK7258_BOOT_TIMER_READY     36u
#define BK7258_BOOT_BRINGUP_WORKERS 37u
#define BK7258_BOOT_UART_WORKER_ENTER 38u
#define BK7258_BOOT_UART_CREATE_ENTER 39u
#define BK7258_BOOT_UART_CREATE_RETURN 40u
#define BK7258_BOOT_BRINGUP_UART_WORKER 41u
#define BK7258_BOOT_BRINGUP_TIMER_START 42u
#define BK7258_BOOT_BRINGUP_TIMER_RETURN 43u
#define BK7258_BOOT_BRINGUP_ACTIVATE 44u
#define BK7258_BOOT_BRINGUP_UART_START 45u
#define BK7258_BOOT_BRINGUP_LINK_WAIT 46u
#define BK7258_BOOT_BRINGUP_LINK_READY 47u
#define BK7258_BOOT_PWC_CREATE_ENTER 48u
#define BK7258_BOOT_PWC_WORKER_ENTER 49u
#define BK7258_BOOT_PWC_WORKER_READY 50u
#define BK7258_BOOT_PWC_CREATE_RETURN 51u
#define BK7258_BOOT_PWC_READY_WAIT_DONE 52u
#define BK7258_BOOT_PWC_BOOT_READY_SEND 53u
#define BK7258_BOOT_PWC_CREATE_FAILED  54u

#define BK7258_BOOT_SECONDARY_ENTRY   1u
#define BK7258_BOOT_SECONDARY_PRIVATE 2u
#define BK7258_BOOT_SECONDARY_IRQ     3u
#define BK7258_BOOT_SECONDARY_MBOX    4u
#define BK7258_BOOT_SECONDARY_IDLE    5u
#define BK7258_BOOT_SECONDARY_STACK   6u
#define BK7258_BOOT_SECONDARY_ONLINE  7u
#define BK7258_BOOT_SECONDARY_NOTIFY  8u
#define BK7258_BOOT_SECONDARY_IRQ_ON  9u
#define BK7258_BOOT_SECONDARY_ACK     10u
#define BK7258_BOOT_SECONDARY_SCHED   11u
#define BK7258_BOOT_SECONDARY_WAIT    12u
#define BK7258_BOOT_SECONDARY_RUNNING 13u
#define BK7258_BOOT_SECONDARY_MBOX_ENTER 14u
#define BK7258_BOOT_SECONDARY_MBOX_RAW   15u
#define BK7258_BOOT_SECONDARY_MBOX_EXIT  16u

static inline volatile uint32_t *bk7258_boottrace_words(void)
{
  return (volatile uint32_t *)BK7258_BOOTTRACE_BASE;
}

static inline void bk7258_boottrace_primary(uint32_t stage)
{
  volatile uint32_t *trace = bk7258_boottrace_words();

  trace[0] = BK7258_BOOTTRACE_MAGIC;
  trace[1] = stage;
  __asm__ volatile("dmb sy" ::: "memory");
}

static inline void bk7258_boottrace_secondary(uint32_t stage)
{
  volatile uint32_t *trace = bk7258_boottrace_words();

  trace[0] = BK7258_BOOTTRACE_MAGIC;
  trace[2] = stage;
  __asm__ volatile("dmb sy" ::: "memory");
}

static inline void bk7258_boottrace_detail(unsigned int index, uint32_t value)
{
  volatile uint32_t *trace = bk7258_boottrace_words();

  if (index < 5)
    {
      trace[index + 3] = value;
      __asm__ volatile("dmb sy" ::: "memory");
    }
}

static inline uint32_t bk7258_boottrace_primary_stage(void)
{
  return bk7258_boottrace_words()[1];
}

static inline uint32_t bk7258_boottrace_secondary_stage(void)
{
  return bk7258_boottrace_words()[2];
}

#endif
