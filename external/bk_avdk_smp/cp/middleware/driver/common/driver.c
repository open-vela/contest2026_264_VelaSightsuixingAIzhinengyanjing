// Copyright 2020-2021 Beken
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <common/bk_include.h>
#include "dd_pub.h"
#include "bk_drv_model.h"
#include "bk_sys_ctrl.h"
#include "sys_driver.h"
#include <driver/int.h>
#include <driver/timer.h>
#include <driver/gpio.h>
#include <driver/dma.h>
#include <driver/uart.h>
#include <driver/wdt.h>
#include <driver/aon_wdt.h>
#include <driver/trng.h>
#include <driver/efuse.h>
#include <driver/ckmn.h>
#include <os/mem.h>
#include <driver/adc.h>
#include <driver/aon_rtc.h>
#include <modules/pm.h>
#include <driver/psram.h>
#include <driver/lin.h>
#include "bk_driver.h"
#include "interrupt_base.h"
#include <driver/otp.h>
#include <driver/pwr_clk.h>
#include "bk_api_ipc.h"

#if CONFIG_MAILBOX
#include <driver/mb_uart_driver.h>
#include <driver/mb_chnl_buff.h>
#include <components/ap_console_bridge.h>
#include <components/shell_task.h>
#include "mbox0_adapter.h"

#define AP_BRIDGE_TAG              "ap_bridge"
#define AP_BRIDGE_TX_RING_SIZE     1024
#define AP_BRIDGE_LOG_LINE_SIZE    256
#define AP_BRIDGE_LOG_FLUSH_MS     50
#define AP_BRIDGE_LOG_RETRY_MAX    3
#define AP_BRIDGE_LOG_RETRY_MS     10
#define AP_BRIDGE_EVENT_RX         0x01
#define AP_BRIDGE_EVENT_TX         0x02
#define AP_BRIDGE_EVENT_MODE       0x04
#define AP_BRIDGE_EVENT_DOWN       0x08
#define AP_BRIDGE_EVENT_RESET      0x10
#define AP_BRIDGE_EVENT_TIMEOUT    0x20
#define AP_BRIDGE_EVENT_READY      0x40
#define AP_BRIDGE_EVENT_SYSTEM     0x80
#define AP_BRIDGE_READY_ALL        (AP_BRIDGE_READY_MBUART | \
				    AP_BRIDGE_READY_IPC | AP_BRIDGE_READY_PWC)
#define AP_BRIDGE_MODE_TIMEOUT_MS  200

typedef struct {
	beken_semaphore_t sem;
	beken_thread_t thread;
	beken_mutex_t tx_mutex;
	beken_semaphore_t mode_sem;
	u8 tx_ring[AP_BRIDGE_TX_RING_SIZE];
	u16 tx_rd;
	u16 tx_wr;
	volatile u32 events;
	volatile u8 link_ready;
	volatile u8 output_mode;
	volatile u8 requested_mode;
	volatile u8 ready_flags;
	volatile u8 ready_set;
	volatile u8 ready_clear;
	volatile u32 mode_generation;
	volatile u32 mode_applied_generation;
	u32 last_down_log_ms;
	ap_console_bridge_stats_t stats;
} ap_bridge_cb_t;

static ap_bridge_cb_t s_ap_bridge;

static void ap_bridge_wake(u32 event)
{
	u32 flags = rtos_disable_int();
	s_ap_bridge.events |= event;
	rtos_enable_int(flags);
	(void)rtos_set_semaphore(&s_ap_bridge.sem);
}

static void ap_bridge_mb_rx(void *param)
{
	(void)param;
	ap_bridge_wake(AP_BRIDGE_EVENT_RX);
}

static void ap_bridge_mb_tx(void *param)
{
	(void)param;
	ap_bridge_wake(AP_BRIDGE_EVENT_TX);
}

static void ap_bridge_mb_event(u8 id, mb_uart_event_t event, void *param)
{
	(void)id;
	(void)param;

	if (event == MB_UART_EVENT_READY)
		ap_console_bridge_ready_update(AP_BRIDGE_READY_MBUART, true);
	else if (event == MB_UART_EVENT_PEER_RESET)
	{
		ap_bridge_wake(AP_BRIDGE_EVENT_RESET);
		ap_console_bridge_ready_update(AP_BRIDGE_READY_MBUART, false);
	}
	else if (event == MB_UART_EVENT_TIMEOUT)
	{
		ap_bridge_wake(AP_BRIDGE_EVENT_TIMEOUT);
		ap_console_bridge_ready_update(AP_BRIDGE_READY_MBUART, false);
	}
	else
	{
		ap_bridge_wake(AP_BRIDGE_EVENT_DOWN);
		ap_console_bridge_ready_update(AP_BRIDGE_READY_MBUART, false);
	}
}

static void ap_bridge_log_output(const u8 *line, u16 length, bool prefix_line)
{
	static const u8 prefix[] = "ap0: ";
	u8 output[sizeof(prefix) - 1 + AP_BRIDGE_LOG_LINE_SIZE];
	u16 output_length;
	u8 retry;

	output_length = 0;
	if (prefix_line)
	{
		memcpy(output, prefix, sizeof(prefix) - 1);
		output_length = sizeof(prefix) - 1;
	}
	memcpy(output + output_length, line, length);
	output_length += length;
	for (retry = 0; retry < AP_BRIDGE_LOG_RETRY_MAX; retry++)
	{
		if (shell_log_raw_data_nonblock(output, output_length))
			return;
		rtos_delay_milliseconds(AP_BRIDGE_LOG_RETRY_MS);
	}
	s_ap_bridge.stats.rx_queue_fail++;
}

static u16 ap_bridge_tx_count(void)
{
	if (s_ap_bridge.tx_rd <= s_ap_bridge.tx_wr)
		return s_ap_bridge.tx_wr - s_ap_bridge.tx_rd;
	return sizeof(s_ap_bridge.tx_ring) - (s_ap_bridge.tx_rd - s_ap_bridge.tx_wr);
}

static void ap_bridge_drain_tx(void)
{
	u8 data[MB_CHNL_BUFF_LEN];
	u16 count;
	u16 length;
	u16 written;
	u16 i;

	while (s_ap_bridge.link_ready)
	{
		rtos_lock_mutex(&s_ap_bridge.tx_mutex);
		count = ap_bridge_tx_count();
		if (!count)
		{
			rtos_unlock_mutex(&s_ap_bridge.tx_mutex);
			break;
		}
		length = MIN(count, (u16)sizeof(data));
		length = MIN(length, bk_mb_uart_write_ready(MB_UART0));
		if (!length)
		{
			rtos_unlock_mutex(&s_ap_bridge.tx_mutex);
			break;
		}

		for (i = 0; i < length; i++)
			data[i] = s_ap_bridge.tx_ring[(s_ap_bridge.tx_rd + i) %
							 sizeof(s_ap_bridge.tx_ring)];

		written = bk_mb_uart_write(MB_UART0, data, length);
		if (!written)
		{
			rtos_unlock_mutex(&s_ap_bridge.tx_mutex);
			break;
		}
		if (written < length)
			s_ap_bridge.stats.tx_partial++;

		s_ap_bridge.tx_rd = (s_ap_bridge.tx_rd + written) %
					 sizeof(s_ap_bridge.tx_ring);
		s_ap_bridge.stats.tx_bytes += written;
		rtos_unlock_mutex(&s_ap_bridge.tx_mutex);
	}
}

static void ap_bridge_task(beken_thread_arg_t arg)
{
	u8 input[MB_CHNL_BUFF_LEN];
	u8 line[AP_BRIDGE_LOG_LINE_SIZE];
	u16 line_length = 0;
	u8 active_mode = AP_CONSOLE_OUTPUT_LOG;
	bool line_continues = false;

	(void)arg;
	for (;;)
	{
		u32 events;
		u32 flags;
		u16 input_length;
		u16 index;
		u8 ready_set;
		u8 ready_clear;
		u8 ready_before;
		bk_err_t ret = rtos_get_semaphore(&s_ap_bridge.sem,
			line_length ? AP_BRIDGE_LOG_FLUSH_MS : 20);

		bk_mb_uart_poll(MB_UART0);

		flags = rtos_disable_int();
		events = s_ap_bridge.events;
		s_ap_bridge.events = 0;
		ready_set = s_ap_bridge.ready_set;
		ready_clear = s_ap_bridge.ready_clear;
		ready_before = s_ap_bridge.ready_flags;
		s_ap_bridge.ready_set = 0;
		s_ap_bridge.ready_clear = 0;
		rtos_enable_int(flags);

		if (events & (AP_BRIDGE_EVENT_DOWN | AP_BRIDGE_EVENT_RESET |
			      AP_BRIDGE_EVENT_TIMEOUT))
		{
			ready_clear |= AP_BRIDGE_READY_MBUART;
		}
		s_ap_bridge.ready_flags |= ready_set;
		s_ap_bridge.ready_flags &= ~ready_clear;
		/* poll() may complete recovery in the same iteration that consumes an
		 * older timeout/reset event.  The driver's current state is the final
		 * authority for the MBUART gate, so a stale event cannot clear a newer
		 * READY notification permanently.
		 */
		if (bk_mb_uart_is_link_ready(MB_UART0))
			s_ap_bridge.ready_flags |= AP_BRIDGE_READY_MBUART;
		else
			s_ap_bridge.ready_flags &= ~AP_BRIDGE_READY_MBUART;
		s_ap_bridge.link_ready =
			(s_ap_bridge.ready_flags & AP_BRIDGE_READY_ALL) ==
			AP_BRIDGE_READY_ALL && bk_mb_uart_is_link_ready(MB_UART0);

		if (s_ap_bridge.mode_applied_generation !=
			s_ap_bridge.mode_generation)
		{
			if (line_length)
				ap_bridge_log_output(line, line_length, !line_continues);
			line_length = 0;
			line_continues = false;
			active_mode = s_ap_bridge.requested_mode;
			s_ap_bridge.output_mode = active_mode;
			s_ap_bridge.mode_applied_generation =
				s_ap_bridge.mode_generation;
			rtos_set_semaphore(&s_ap_bridge.mode_sem);
		}

		do
		{
			input_length = bk_mb_uart_read(MB_UART0, input, sizeof(input));
			s_ap_bridge.stats.rx_bytes += input_length;
			if (active_mode == AP_CONSOLE_OUTPUT_RAW)
			{
				if (input_length && !shell_log_raw_data_nonblock(input, input_length))
					s_ap_bridge.stats.rx_queue_fail++;
				continue;
			}

			for (index = 0; index < input_length; index++)
			{
				line[line_length++] = input[index];
				if (input[index] == '\n' || line_length == sizeof(line))
				{
					bool complete = input[index] == '\n';

					ap_bridge_log_output(line, line_length,
							     !line_continues);
					line_length = 0;
					line_continues = !complete;
				}
			}
		} while (input_length != 0);

		if (ret == kTimeoutErr && line_length &&
			active_mode == AP_CONSOLE_OUTPUT_LOG)
		{
			ap_bridge_log_output(line, line_length, !line_continues);
			line_length = 0;
			line_continues = true;
		}

		ap_bridge_drain_tx();

		if (events & (AP_BRIDGE_EVENT_DOWN | AP_BRIDGE_EVENT_RESET |
			      AP_BRIDGE_EVENT_TIMEOUT))
		{
			u32 now = rtos_get_time();

			s_ap_bridge.stats.link_down++;
			if (events & AP_BRIDGE_EVENT_RESET)
				s_ap_bridge.stats.reset++;
			if (events & AP_BRIDGE_EVENT_TIMEOUT)
				s_ap_bridge.stats.timeout++;
			if ((ready_before & (AP_BRIDGE_READY_IPC |
					     AP_BRIDGE_READY_PWC)) ==
				(AP_BRIDGE_READY_IPC | AP_BRIDGE_READY_PWC) &&
				(u32)(now - s_ap_bridge.last_down_log_ms) >= 1000)
			{
				s_ap_bridge.last_down_log_ms = now;
				BK_LOGW(AP_BRIDGE_TAG,
					"link down event=%x queued=%u ready=%x\r\n",
					events, ap_bridge_tx_count(),
					s_ap_bridge.ready_flags);
			}
			if (!s_ap_bridge.link_ready)
				shell_ap_console_link_down();
		}
	}
}

bk_err_t ap_console_bridge_init(void)
{
	bk_err_t ret;

	memset(&s_ap_bridge, 0, sizeof(s_ap_bridge));
	ret = bk_pm_module_vote_sleep_ctrl(PM_SLEEP_MODULE_NAME_MAILBOX, 0, 0);
	if (ret != BK_OK)
		return ret;
	ret = rtos_init_mutex(&s_ap_bridge.tx_mutex);
	if (ret != BK_OK)
		goto init_fail_vote;
	ret = rtos_init_semaphore(&s_ap_bridge.sem, 1);
	if (ret != BK_OK)
		goto init_fail_mutex;
	ret = rtos_init_semaphore(&s_ap_bridge.mode_sem, 1);
	if (ret != BK_OK)
		goto init_fail_sem;

	ret = rtos_create_thread(&s_ap_bridge.thread,
					 BEKEN_DEFAULT_WORKER_PRIORITY,
					 "ap_console", ap_bridge_task, 3072, NULL);
	if (ret != BK_OK)
	{
		rtos_deinit_semaphore(&s_ap_bridge.mode_sem);
		rtos_deinit_semaphore(&s_ap_bridge.sem);
		goto init_fail_mutex;
	}

	ret = bk_mb_uart_dev_init(MB_UART0);
	if (ret != BK_OK)
	{
		rtos_delete_thread(&s_ap_bridge.thread);
		rtos_deinit_semaphore(&s_ap_bridge.mode_sem);
		rtos_deinit_semaphore(&s_ap_bridge.sem);
		goto init_fail_mutex;
	}

	ret = bk_mb_uart_register_rx_isr(MB_UART0, ap_bridge_mb_rx, NULL);
	if (ret == BK_OK)
		ret = bk_mb_uart_register_tx_isr(MB_UART0, ap_bridge_mb_tx, NULL);
	if (ret == BK_OK)
		ret = bk_mb_uart_register_event_callback(MB_UART0, ap_bridge_mb_event, NULL);
	if (ret != BK_OK)
	{
		bk_mb_uart_dev_deinit(MB_UART0);
		rtos_delete_thread(&s_ap_bridge.thread);
		rtos_deinit_semaphore(&s_ap_bridge.mode_sem);
		rtos_deinit_semaphore(&s_ap_bridge.sem);
		goto init_fail_mutex;
	}

	ap_bridge_wake(AP_BRIDGE_EVENT_RX | AP_BRIDGE_EVENT_TX);
	BK_LOGI(AP_BRIDGE_TAG, "init owner=MB_UART0 tx_ring=%u mode=LOG\r\n",
		(unsigned)sizeof(s_ap_bridge.tx_ring));
	return BK_OK;

init_fail_sem:
	rtos_deinit_semaphore(&s_ap_bridge.sem);
init_fail_mutex:
	rtos_deinit_mutex(&s_ap_bridge.tx_mutex);
init_fail_vote:
	bk_pm_module_vote_sleep_ctrl(PM_SLEEP_MODULE_NAME_MAILBOX, 1, 0);
	return ret;
}

u16 ap_console_bridge_write(const u8 *data, u16 length)
{
	u16 written = 0;

	if (!data || !length || !s_ap_bridge.link_ready)
		return 0;

	rtos_lock_mutex(&s_ap_bridge.tx_mutex);
	while (written < length)
	{
		u16 next = (s_ap_bridge.tx_wr + 1) % sizeof(s_ap_bridge.tx_ring);
		if (next == s_ap_bridge.tx_rd)
			break;
		s_ap_bridge.tx_ring[s_ap_bridge.tx_wr] = data[written++];
		s_ap_bridge.tx_wr = next;
	}
	rtos_unlock_mutex(&s_ap_bridge.tx_mutex);
	if (written < length)
	{
		s_ap_bridge.stats.tx_drop += length - written;
	}
	if (written)
		ap_bridge_wake(AP_BRIDGE_EVENT_TX);
	return written;
}

void ap_console_bridge_purge_tx(void)
{
	rtos_lock_mutex(&s_ap_bridge.tx_mutex);
	s_ap_bridge.stats.tx_drop += ap_bridge_tx_count();
	s_ap_bridge.tx_rd = s_ap_bridge.tx_wr;
	rtos_unlock_mutex(&s_ap_bridge.tx_mutex);
}

bk_err_t ap_console_bridge_set_output_mode(ap_console_output_mode_t mode)
{
	u32 generation;

	if (mode > AP_CONSOLE_OUTPUT_RAW)
		return BK_ERR_PARAM;
	s_ap_bridge.requested_mode = mode;
	generation = ++s_ap_bridge.mode_generation;
	ap_bridge_wake(AP_BRIDGE_EVENT_MODE);
	while (s_ap_bridge.mode_applied_generation != generation)
	{
		if (rtos_get_semaphore(&s_ap_bridge.mode_sem,
				       AP_BRIDGE_MODE_TIMEOUT_MS) != BK_OK)
			return BK_ERR_TIMEOUT;
	}
	return BK_OK;
}

bool ap_console_bridge_is_ready(void)
{
	return s_ap_bridge.link_ready;
}

void ap_console_bridge_get_stats(ap_console_bridge_stats_t *stats)
{
	mb_chnl_diag_t diag;
	mbox0_adapter_diag_t adapter;

	if (!stats)
		return;
	*stats = s_ap_bridge.stats;
	rtos_lock_mutex(&s_ap_bridge.tx_mutex);
	stats->tx_queued = ap_bridge_tx_count();
	rtos_unlock_mutex(&s_ap_bridge.tx_mutex);
	stats->mb_tx_free = bk_mb_uart_write_ready(MB_UART0);
	stats->mb_rx_ready = bk_mb_uart_read_ready(MB_UART0);
	stats->mb_status = bk_mb_uart_get_status(MB_UART0);
	stats->link_ready = ap_console_bridge_is_ready();
	stats->output_mode = s_ap_bridge.output_mode;
	stats->ready_flags = s_ap_bridge.ready_flags;
	stats->transport_state = bk_mb_uart_get_link_state(MB_UART0);
	bk_mailbox_get_diag(&adapter);
	stats->bad_envelope = adapter.bad_sid + adapter.bad_length +
		adapter.bad_alignment + adapter.bad_address +
		adapter.descriptor_overflow;
	if (mb_chnl_get_diag(MAILBOX_CPU1, &diag) == BK_OK)
	{
		stats->active_busy = diag.busy;
		stats->active_channel = diag.logical_chnl;
		stats->active_sequence = diag.sequence;
		stats->active_command = diag.command;
		stats->ack_retry = diag.ack_retry;
		stats->ack_queue_full = diag.ack_queue_full;
		stats->reset_bad = diag.reset_bad;
	}
}

void ap_console_bridge_ready_update(ap_console_ready_flag_t flag, bool ready)
{
	u32 flags = rtos_disable_int();

	if (ready)
	{
		s_ap_bridge.ready_set |= flag;
		s_ap_bridge.ready_clear &= ~flag;
	}
	else
	{
		s_ap_bridge.ready_clear |= flag;
		s_ap_bridge.ready_set &= ~flag;
		s_ap_bridge.link_ready = 0;
	}
	rtos_enable_int(flags);
	if (ready && (flag & (AP_BRIDGE_READY_IPC | AP_BRIDGE_READY_PWC)) &&
		!bk_mb_uart_is_link_ready(MB_UART0))
		bk_mb_uart_probe(MB_UART0);
	ap_bridge_wake(AP_BRIDGE_EVENT_SYSTEM);
}

void ap_console_bridge_link_down(void)
{
	u32 flags = rtos_disable_int();
	s_ap_bridge.ready_clear |= AP_BRIDGE_READY_ALL;
	s_ap_bridge.ready_set = 0;
	s_ap_bridge.link_ready = 0;
	rtos_enable_int(flags);
	ap_bridge_wake(AP_BRIDGE_EVENT_DOWN | AP_BRIDGE_EVENT_SYSTEM);
}
#endif

#if CONFIG_AON_PMU
#include "aon_pmu_driver.h"
#endif

#if CONFIG_FLASH
#include <driver/flash.h>
#endif

#if CONFIG_EASY_FLASH
#include "bk_ef.h"
#endif

#if ((CONFIG_SDIO_HOST) || (CONFIG_SDCARD))
#include "driver/sdio_host.h"
#endif

#if CONFIG_SDCARD
#include "sdcard.h"
#endif

#if (CONFIG_SDIO_V2P0 && CONFIG_SDIO_SLAVE)
#include "sdio_slave_driver.h"
#if CONFIG_SDIO_TEST_EN
#include "sdio_test.h"
#endif
#endif

#if CONFIG_QSPI
#include <driver/qspi.h>
#endif

#if CONFIG_JPEGENC_HW
#include <driver/jpeg_enc.h>
#endif

#if CONFIG_CALENDAR
#include <driver/calendar.h>
#endif

#if CONFIG_ATE
#include <components/ate.h>
#endif

#if CONFIG_TOUCH_PM_SUPPORT
#include <driver/touch.h>
#endif

#if CONFIG_CHIP_SUPPORT
#include "modules/chip_support.h"
#endif

#if CONFIG_YUV_BUF
#include <driver/yuv_buf.h>
#endif

#if CONFIG_H264
#include <driver/h264.h>
#endif

#if CONFIG_SDMADC
#include <driver/sdmadc.h>
#endif

#if CONFIG_HW_ROTATE_PFC
#include <driver/rott_driver.h>
#endif

#if CONFIG_GET_UID_ENABLE
#include <components/bk_uid.h>
#endif

//TODO only init driver model and necessary drivers
#if CONFIG_POWER_CLOCK_RF
#define   MODULES_POWER_OFF_ENABLE (1)
#define   ROSC_DEBUG_EN            (0)
#define   MODULES_CLK_ENABLE       (0)
extern void clock_dco_cali(UINT32 speed);
void power_clk_rf_init()
{
    uint32_t param =0;
	/*power on all the modules for bringup test*/

	module_name_t use_module = MODULE_NAME_WIFI;
    /*1. power on all the modules*/
	#if MODULES_POWER_OFF_ENABLE
	    sys_drv_module_power_ctrl(POWER_MODULE_NAME_ENCP,POWER_MODULE_STATE_OFF);
		//sys_drv_module_power_ctrl(POWER_MODULE_NAME_BAKP,POWER_MODULE_STATE_OFF);
		sys_drv_module_power_ctrl(POWER_MODULE_NAME_AUDP,POWER_MODULE_STATE_OFF);
		sys_drv_module_power_ctrl(POWER_MODULE_NAME_VIDP,POWER_MODULE_STATE_OFF);
		sys_drv_module_power_ctrl(POWER_MODULE_NAME_BTSP,POWER_MODULE_STATE_OFF);
		sys_drv_module_power_ctrl(POWER_MODULE_NAME_WIFIP_MAC,POWER_MODULE_STATE_OFF);
		sys_drv_module_power_ctrl(POWER_MODULE_NAME_WIFI_PHY,POWER_MODULE_STATE_OFF);
		sys_drv_module_power_ctrl(POWER_MODULE_NAME_CPU1,POWER_MODULE_STATE_OFF);
	#else
	    power_module_name_t module = POWER_MODULE_NAME_MEM1;
        for(module = POWER_MODULE_NAME_MEM1 ; module < POWER_MODULE_NAME_NONE ; module++)
        {
            sys_drv_module_power_ctrl(module,POWER_MODULE_STATE_ON);
        }
    #endif
    /*2. enable the analog clock*/
    sys_drv_module_RF_power_ctrl(use_module ,POWER_MODULE_STATE_ON);

	/*3.enable all the modules clock*/
	#if MODULES_CLK_ENABLE
	dev_clk_pwr_id_t devid = 0;
	for(devid = 0; devid < 32; devid++)
	{
	    sys_drv_dev_clk_pwr_up(devid, CLK_PWR_CTRL_PWR_UP);
    }
	#endif
    /*4.set the cpu0 and matrix clock*/
   /*cpu0:26m ,matrix:26m*/
   //sys_drv_core_bus_clock_ctrl(HIGH_FREQUECY_CLOCK_MODULE_CPU0, 1,0, HIGH_FREQUECY_CLOCK_MODULE_CPU0_MATRIX,0,0);
   /*cpu0:120m ,matrix:120m*/
   //sys_drv_core_bus_clock_ctrl(HIGH_FREQUECY_CLOCK_MODULE_CPU0, 3,0, HIGH_FREQUECY_CLOCK_MODULE_CPU0_MATRIX,0,0);
   /*cpu0:240m ,matrix:120m*/
	//sys_drv_core_bus_clock_ctrl(HIGH_FREQUECY_CLOCK_MODULE_CPU0, 0,0, HIGH_FREQUECY_CLOCK_MODULE_CPU0_MATRIX,0,0);

	//bk_pm_module_vote_cpu_freq(PM_DEV_ID_DEFAULT,PM_CPU_FRQ_120M);
	#if CONFIG_ATE_TEST
		//bk_pm_module_vote_cpu_freq(PM_DEV_ID_DEFAULT,PM_CPU_FRQ_320M);//improve the cpu frequency for save boot time at ate test
	#endif

   /*5.config the analog*/
   //sys_drv_analog_set(ANALOG_REG0, param);
   //sys_drv_analog_set(ANALOG_REG0, param);
   //sys_drv_analog_set(ANALOG_REG0, param);

	//config apll
	//param = 0;
	//param = sys_drv_analog_get(ANALOG_REG4);
	//param &= ~(0x1f << 5);
	//param |= (0x14 << 5);
	//sys_drv_analog_set(ANALOG_REG4, param);

	/*set low power low voltage value */
#if 0
	param = 0;
	param = sys_drv_analog_get(ANALOG_REG3);
	param &= ~(0x7 << 29);
	param |= (0x4 << 29);
	sys_drv_analog_set(ANALOG_REG3, param);

	param = sys_drv_analog_get(ANALOG_REG2);
	param |= (0x1 << 25);
	sys_drv_analog_set(ANALOG_REG2, param);
#endif
	/*tempreture det enable for VIO*/
	param = 0;
	param = sys_drv_analog_get(ANALOG_REG6);
	param |= (0x1 << SYS_ANA_REG6_EN_TEMPDET_POS)|(0x7 << SYS_ANA_REG6_RXTAL_LP_POS)|(0x7 << SYS_ANA_REG6_RXTAL_HP_POS);
	param &= ~(0x1 << SYS_ANA_REG6_EN_SLEEP_POS);
	sys_drv_analog_set(ANALOG_REG6, param);

#if 0
	param = 0;
	param = aon_pmu_drv_reg_get(PMU_REG3);
	param = 0x1 << 0; //security boot  bypass
	aon_pmu_drv_reg_set(PMU_REG3,param);
#endif
	/*let rosc to bt/wifi ip*/
	param = 0;
	param = aon_pmu_drv_reg_get(PMU_REG0x41);
	param |= 0x1 << 24;
	aon_pmu_drv_reg_set(PMU_REG0x41,param);
	/*wake delay of Xtal*/
#if 0
	param = 0;
	param = aon_pmu_drv_reg_get(PMU_REG0x40);
	param |= 0xF << 0;
	aon_pmu_drv_reg_set(PMU_REG0x40,param);
#endif
	/* rosc calibration start*/
	/*a.open rosc debug*/
#if ROSC_DEBUG_EN
	param = 0;
	param = sys_drv_analog_get(ANALOG_REG5);
	param |= 0x1 << SYS_ANA_REG5_CK_TST_ENBALE_POS;
	sys_drv_analog_set(ANALOG_REG5,param);

	param = 0;
	param = sys_drv_analog_get(ANALOG_REG11);
	param |= 0x1 << SYS_ANA_REG11_TEST_EN_POS;
	sys_drv_analog_set(ANALOG_REG11,param);

	param = 0;
	param = sys_drv_analog_get(ANALOG_REG4);
	param |= (0x1 << SYS_ANA_REG4_ROSC_TSTEN_POS);//Rosc test enable
	sys_drv_analog_set(ANALOG_REG4,param);

#endif
	/*b.config calibration*/
	param = 0;
	param = sys_drv_analog_get(ANALOG_REG4);
	param &= ~(SYS_ANA_REG4_ROSC_CAL_INTVAL_MASK << SYS_ANA_REG4_ROSC_CAL_INTVAL_POS);//clear the data
	param |= 0x4 << SYS_ANA_REG4_ROSC_CAL_INTVAL_POS;//Rosc Calibration Interlval 0.25s~2s (4:1s)
	param |= 0x1 << SYS_ANA_REG4_ROSC_CAL_MODE_POS;//0x1: 32K ;0x0: 31.25K
	param |= 0x1 << SYS_ANA_REG4_ROSC_CAL_EN_POS;//Rosc Calibration Enable
	param &= ~(0x1 << SYS_ANA_REG4_ROSC_MANU_EN_POS);//0:close Rosc Calibration Manual Mode
	sys_drv_analog_set(ANALOG_REG4,param);

	/*c.trigger calibration*/
	param = 0;
	param = sys_drv_analog_get(ANALOG_REG4);
	param &= ~(0x1 << SYS_ANA_REG4_ROSC_CAL_TRIG_POS);//trigger clear
	sys_drv_analog_set(ANALOG_REG4,param);

	param = 0;
	param = sys_drv_analog_get(ANALOG_REG4);
	param |= (0x1 << SYS_ANA_REG4_ROSC_CAL_TRIG_POS);//trigger enable
	sys_drv_analog_set(ANALOG_REG4,param);
	/* rosc calibration end*/
	/*7.dpll calibration */
	//sys_drv_cali_dpll(0);

	/*8.dco calibration*/
	//clock_dco_cali(0x4);
}
#endif

int driver_early_init(void)
{
	interrupt_init();

#if CONFIG_AON_PMU
	aon_pmu_drv_init();
#endif

#if CONFIG_POWER_CLOCK_RF
	power_clk_rf_init();
#endif

#if CONFIG_TRNG_SUPPORT
	bk_trng_driver_init();
#endif

#if CONFIG_EFUSE
	bk_efuse_driver_init();
#endif

	return 0;
}
int driver_init(void) {
	sys_drv_init();

	bk_gpio_driver_init();

	//Important notice!!!!!
	//ATE uses UART TX PIN as the detect ATE mode pin,
	//so it should be called after GPIO init and before UART init.
	//or caused ATE can't work or UART can't work
#if CONFIG_ATE
	bk_ate_init();
#endif

	//Important notice!
	//Before UART is initialized, any call of BK_LOG_RAW/os_print/BK_LOGx may
	//cause problems, such as crash etc!
	bk_uart_driver_init();

#if CONFIG_CHIP_SUPPORT
	if(!bk_is_chip_supported()) {
		return BK_FAIL;
	}
#endif

	drv_model_init();

	g_dd_init();

#if CONFIG_TIMER
	bk_timer_driver_init();
#endif

#if CONFIG_GENERAL_DMA
	bk_dma_driver_init();
#endif

	bk_wdt_driver_init();

#if CONFIG_AON_WDT && !CONFIG_INT_AON_WDT
	bk_aon_wdt_stop();
#endif

#if CONFIG_MAILBOX
	extern bk_err_t ipc_init(void);
	extern bk_err_t mb_ipc_init(void);
	ipc_init();
	if (ap_console_bridge_init() != BK_OK)
	{
		return BK_FAIL;
	}
#if CONFIG_MAILBOX_IPC
	mb_ipc_init();
#endif
	bk_ipc_init();
#endif

	os_show_memory_config_info();

#if CONFIG_FLASH
	bk_flash_driver_init();
#if CONFIG_FLASH_ORIGIN_API
	extern int hal_flash_init();
	hal_flash_init();
#endif
#endif

#if CONFIG_EASY_FLASH
	easyflash_init();
#endif

#if CONFIG_SARADC
	bk_adc_driver_init();
#endif

#if CONFIG_SPI
	bk_spi_driver_init();
#endif

#if CONFIG_I2C
	bk_i2c_driver_init();
#endif

#if CONFIG_QSPI
	bk_qspi_driver_init();
#endif

#if CONFIG_YUV_BUF
	bk_yuv_buf_driver_init();
#endif

#if CONFIG_JPEGENC_HW
	bk_jpeg_enc_driver_init();
#endif

#if CONFIG_H264
	bk_h264_driver_init();
#endif

#if CONFIG_AON_RTC
	bk_aon_rtc_driver_init();
#endif

#if ((CONFIG_SDIO_HOST) || (CONFIG_SDCARD))
	bk_sdio_host_driver_init();
#endif

#if CONFIG_CALENDAR
	bk_calendar_driver_init();
#endif

//call it after LOG is valid.
#if CONFIG_ATE
	BK_LOGD(NULL,"ate enabled is %d\r\n", ate_is_enabled());
#endif

#if CONFIG_TOUCH_PM_SUPPORT
	bk_touch_pm_init();
#endif

#if CONFIG_SDMADC
	//bk_sdmadc_driver_init();
#endif

#if CONFIG_HW_ROTATE_PFC
//	bk_rott_driver_init();
#endif

#if CONFIG_CKMN
	bk_ckmn_driver_init();
#endif
#if CONFIG_LIN
	bk_lin_driver_init();
#endif

#if (CONFIG_TRUSTENGINE)
	extern int dubhe_driver_init( unsigned long dbh_base_addr );
	dubhe_driver_init(SOC_SHANHAI_BASE);
#endif

#if CONFIG_GET_UID_ENABLE
	bk_uid_driver_init();
#endif

#if CONFIG_PSRAM
	/* The BOOT owner keeps CP PSRAM users independent from OpenVela's vote. */
	{
		bk_err_t psram_ret = bk_pm_module_vote_psram_ctrl(
			PM_POWER_PSRAM_MODULE_NAME_BOOT, PM_POWER_MODULE_STATE_ON);
		if (psram_ret != BK_OK) {
			BK_LOGE(NULL, "driver_init: bk_psram_init fail:%d\r\n", psram_ret);
		} else {
			BK_LOGI(NULL, "driver_init: psram init ok\r\n");
		}
	}
#endif

	BK_LOGD(NULL,"driver_init end\r\n");

	return 0;
}
