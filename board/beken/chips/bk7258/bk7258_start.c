/****************************************************************************
 * vendor/beken/chips/bk7258/bk7258_start.c
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdint.h>
#include <stdbool.h>

#include <nuttx/init.h>

#include "arm_internal.h"
#include "mpu.h"
#include "nvic.h"

#include "hardware/bk7258_memorymap.h"

extern const uint8_t _eronly[];
extern uint8_t _sdata[];
extern uint8_t _edata[];
extern uint8_t _sbss[];
extern uint8_t _ebss[];
extern uint8_t __ram_vectors_load[];
extern uint8_t __ram_vectors_start[];
extern uint8_t __ram_vectors_end[];
extern uint8_t __idle_stack_base[];
extern uint8_t __idle_stack_top[];

const uintptr_t g_idle_topstack = (uintptr_t)__idle_stack_top;

static const struct mpu_region_s g_bk7258_mpu_regions[] =
{
  {
    BK7258_AP_FLASH_BASE,
    BK7258_AP_FLASH_SIZE,
    MPU_RBAR_AP_RORO | MPU_RBAR_SH_NO,
    MPU_RLAR_WRITE_THROUGH
  },
  {
    BK7258_AP_RAM_BASE,
    BK7258_AP_RAM_SIZE,
    MPU_RBAR_XN | MPU_RBAR_AP_RWRW | MPU_RBAR_SH_INNER,
    MPU_RLAR_NONCACHEABLE
  },
  {
    0x40000000u,
    0x20000000u,
    MPU_RBAR_XN | MPU_RBAR_AP_RWRW | MPU_RBAR_SH_INNER,
    MPU_RLAR_DEVICE
  },
};

static void __attribute__((used, noinline, noreturn,
                           target("general-regs-only")))
bk7258_start(void)
{
  const uint8_t *src;
  uint8_t *dest;

  /* Enable CP10/CP11 before code compiled for the hard-float ABI can use the
   * floating-point extension.
   */

  modifyreg32(NVIC_CPACR, 0,
              NVIC_CPACR_CP_FULL(10) | NVIC_CPACR_CP_FULL(11));
  UP_DSB();
  UP_ISB();

  /* Match the current golden SPE SAU setup: the secure alias is retained,
   * while the 0x10000000 non-secure alias range is exposed as non-secure.
   */

  putreg32(0, BK7258_SAU_BASE + 0x08);
  putreg32(0x00000000u, BK7258_SAU_BASE + 0x0c);
  putreg32(0x0fffffe3u, BK7258_SAU_BASE + 0x10);
  putreg32(1, BK7258_SAU_BASE + 0x08);
  putreg32(0x10000000u, BK7258_SAU_BASE + 0x0c);
  putreg32(0xefffffe1u, BK7258_SAU_BASE + 0x10);
  putreg32(1, BK7258_SAU_BASE + 0x00);
  UP_DSB();
  UP_ISB();

  for (src = _eronly, dest = _sdata; dest < _edata; )
    {
      *dest++ = *src++;
    }

  for (dest = _sbss; dest < _ebss; )
    {
      *dest++ = 0;
    }

  for (src = __ram_vectors_load, dest = __ram_vectors_start;
       dest < __ram_vectors_end; )
    {
      *dest++ = *src++;
    }

  putreg32((uintptr_t)__ram_vectors_start, NVIC_VECTAB);
  UP_DSB();
  UP_ISB();

#ifdef CONFIG_ARM_MPU
  mpu_reset();
  mpu_initialize(g_bk7258_mpu_regions,
                 sizeof(g_bk7258_mpu_regions) /
                 sizeof(g_bk7258_mpu_regions[0]),
                 false, true);
#endif

#ifdef USE_EARLYSERIALINIT
  arm_earlyserialinit();
#endif

  nx_start();
  for (; ; )
    {
    }
}

void __attribute__((naked, noreturn, section(".start_text"))) __start(void)
{
  __asm__ volatile
    (
      "cpsid i\n"
      "ldr r0, =__idle_stack_base\n"
      "msr msplim, r0\n"
      "b bk7258_start\n"
    );
}
