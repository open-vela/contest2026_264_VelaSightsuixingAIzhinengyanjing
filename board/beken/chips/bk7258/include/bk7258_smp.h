/****************************************************************************
 * BK7258 OpenVela AP SMP interfaces.
 ****************************************************************************/

#ifndef __VENDOR_BEKEN_CHIPS_BK7258_INCLUDE_BK7258_SMP_H
#define __VENDOR_BEKEN_CHIPS_BK7258_INCLUDE_BK7258_SMP_H

#include <nuttx/config.h>

#include <stdbool.h>
#include <stdint.h>

void bk7258_cpu_private_initialize(bool primary);
void bk7258_hwspinlock_initialize(void);
int bk7258_timer_start(void);

#ifdef CONFIG_SMP
void bk7258_irqinitialize_secondary(void);
void bk7258_smp_prepare_boot(void);
void bk7258_smp_boot_notify(void);
void bk7258_smp_boot_ack(void);
void bk7258_smp_secondary_ready(void);
bool bk7258_smp_secondary_is_ready(void);
bool bk7258_smp_boot_notified(void);
bool bk7258_smp_boot_ping_received(void);
bool bk7258_smp_boot_test_complete(void);
int bk7258_smp_boot_ping(int cpu);
#endif

#endif
