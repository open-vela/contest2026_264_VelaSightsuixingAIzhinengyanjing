/****************************************************************************
 * vendor/beken/chips/bk7258/hardware/bk7258_memorymap.h
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef __VENDOR_BEKEN_CHIPS_BK7258_HARDWARE_BK7258_MEMORYMAP_H
#define __VENDOR_BEKEN_CHIPS_BK7258_HARDWARE_BK7258_MEMORYMAP_H

/* Secure aliases used by the current BK7258 AP SPE image. */

#define BK7258_FLASH_BASE          0x02000000u
#define BK7258_AP_FLASH_BASE       0x02150000u
#define BK7258_AP_FLASH_SIZE       0x00110000u

#define BK7258_AP_RAM_BASE         0x28010000u
#define BK7258_AP_RAM_SIZE         0x00054000u
#define BK7258_AP_RAM_END          (BK7258_AP_RAM_BASE + BK7258_AP_RAM_SIZE)

#define BK7258_SYSCTRL_BASE        0x44010000u
#define BK7258_AON_GPIO_BASE       0x44000400u
#define BK7258_UART1_BASE          0x45830000u
#define BK7258_MBOX0_BASE          0x41000000u

#define BK7258_NVIC_BASE           0xe000e100u
#define BK7258_SCB_BASE            0xe000ed00u
#define BK7258_SAU_BASE            0xe000edd0u

#endif
