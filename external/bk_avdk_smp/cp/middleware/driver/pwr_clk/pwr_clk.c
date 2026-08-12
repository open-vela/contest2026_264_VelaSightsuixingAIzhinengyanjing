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
#include <driver/pwr_clk.h>
#if CONFIG_MAILBOX
#include <driver/mailbox_channel.h>
#include "bk_api_ipc.h"
#include "mbox0_adapter.h"
#endif
#include <modules/pm.h>
#include "sys_driver.h"
#include "sys_hal.h"
#if CONFIG_PSRAM
#include <driver/psram.h>
#endif
#include <os/mem.h>
#include "sys_types.h"
#include <driver/aon_rtc.h>
#include <os/os.h>
#include <driver/psram.h>
#include <components/system.h>
#include "bk_pm_internal_api.h"
#include <common/bk_kernel_err.h>
#include "aon_pmu_hal.h"
#include "driver/low_pwr_core.h"
#include "low_pwr_misc.h"
#include <components/ap_console_bridge.h>
/*=====================DEFINE  SECTION  START=====================*/
#define PM_SEND_CMD_CP1_RESPONSE_TIEM        (100)  //100ms
#define PM_BOOT_CP1_WAITING_TIEM             (10000) // 10s
#define PM_CP1_RECOVERY_DEFAULT_VALUE        (0xFFFFFFFFFFFFFFFF)
#define PM_OPEN_CP1_TIMEOUT                  (20000) //20s
#define PM_CHNL_STATE_BUSY                  (1)
#define PM_CHNL_STATE_IDLE                  (0)
#define TAG "CP"
#define LOGD(...) BK_LOGD(TAG, ##__VA_ARGS__)
#define LOGW(...) BK_LOGW(TAG, ##__VA_ARGS__)
#define LOGE(...) BK_LOGE(TAG, ##__VA_ARGS__)
#define LOGV(...) BK_LOGV(TAG, ##__VA_ARGS__)

/*=====================DEFINE  SECTION  END=====================*/

/*=====================VARIABLE  SECTION  START=================*/

static volatile  pm_mailbox_communication_state_e s_pm_cp1_boot_ready            = 0;
static volatile  pm_mailbox_communication_state_e s_pm_cp1_psram_malloc_state    = 0;
static volatile  uint32_t                         s_pm_cp1_psram_malloc_count    = 0;
static volatile  uint64_t                         s_pm_cp1_module_recovery_state = PM_CP1_RECOVERY_DEFAULT_VALUE;
static volatile  uint32_t                         s_pm_vdddig_ctrl_state         = 0;
#if (CONFIG_CPU_CNT > 1)
static beken_semaphore_t                          s_sync_cp1_open_sema           = NULL;
#endif
static volatile  uint32_t                         s_pm_cp1_closing               = 0;
static volatile  pm_boot_cp1_module_name_e        s_pm_cp1_closing_module        = PM_BOOT_CP1_MODULE_NAME_MAX;
static volatile  uint32_t                         s_pm_cp1_sema_count            = 0;

static volatile  uint32_t                         s_pm_cp1_boot_try_count        = 0;
static volatile uint32_t                          s_pm_openvela_boot_stage       = 0;
#if CONFIG_OPENVELA_AP_480M
static volatile int32_t                           s_pm_openvela_boot_error       = BK_OK;
static bool                                       s_pm_openvela_cpu1_sleep_held;
static bool                                       s_pm_openvela_freq_held;
#endif
#if CONFIG_PSRAM
static uint32_t                                   s_pm_psram_ctrl_state         = 0;
#endif
#if (CONFIG_CPU_CNT > 2)
static uint32_t                                   s_pm_cp2_ctrl_state            = 0;
static bool                                       s_pm_openvela_cpu2_held;
#endif

/*=====================VARIABLE  SECTION  END=================*/

/*================FUNCTION DECLARATION  SECTION  START========*/

#if (CONFIG_CPU_CNT > 1)
#if CONFIG_MAILBOX
static void pm_cp0_mailbox_init();
static bk_err_t pm_cp0_mailbox_send_data(uint32_t cmd, uint32_t param1,uint32_t param2,uint32_t param3);
#endif
static bk_err_t pm_module_shutdown_cpu1(pm_power_module_name_e module);
bk_err_t bk_pm_cp1_recovery_module_state_ctrl(pm_cp1_prepare_close_module_name_e module,pm_cp1_module_recovery_state_e state);
static void pm_cp1_close_cancel(void);
#endif
extern void bk_delay_us(UINT32 us);
/*================FUNCTION DECLARATION  SECTION  END========*/

pm_lpo_src_e bk_clk_32k_customer_config_get(void)
{
#if CONFIG_LPO_MP_A_FORCE_USE_EXT32K
	uint32_t chip_id = aon_pmu_hal_get_chipid();
	if ((chip_id & PM_CHIP_ID_MASK) == (PM_CHIP_ID_MP_A & PM_CHIP_ID_MASK))
	{
		return PM_LPO_SRC_X32K;
	}
	else
	{
		#if CONFIG_EXTERN_32K
			return PM_LPO_SRC_X32K;
		#elif CONFIG_LPO_SRC_26M32K
			return PM_LPO_SRC_DIVD;
		#else
			return PM_LPO_SRC_ROSC;
		#endif
	}
#else
	#if CONFIG_EXTERN_32K
		return PM_LPO_SRC_X32K;
	#elif CONFIG_LPO_SRC_26M32K
		return PM_LPO_SRC_DIVD;
	#else
		return PM_LPO_SRC_ROSC;
	#endif
#endif
	return PM_LPO_SRC_ROSC;
}
bk_err_t bk_pm_mailbox_init()
{
	/*cp0 mailbox init*/
	#if CONFIG_MAILBOX && (CONFIG_CPU_CNT > 1)
	pm_cp0_mailbox_init();
	#endif //CONFIG_MAILBOX

	return BK_OK;
}



#if (CONFIG_CPU_CNT > 1)
pm_mailbox_communication_state_e bk_pm_cp1_work_state_get()
{
	return s_pm_cp1_boot_ready;
}
bk_err_t bk_pm_cp1_work_state_set(pm_mailbox_communication_state_e state)
{
	s_pm_cp1_boot_ready = state;
	return BK_OK;
}
pm_mailbox_communication_state_e bk_pm_cp0_psram_malloc_state_get()
{
	return s_pm_cp1_psram_malloc_state;
}
bk_err_t bk_pm_cp0_psram_malloc_state_set(pm_mailbox_communication_state_e state)
{
	s_pm_cp1_psram_malloc_state = state;
	return BK_OK;
}
#if CONFIG_MAILBOX
static bk_err_t pm_cp0_send_msg(uint32_t event, uint32_t param1,uint32_t param2,uint32_t param3)
{
	low_pwr_core_msg_t msg = {0};
	msg.event= event;
	msg.param1 = param1;
	msg.param2 = param2;
	msg.param3 = param3;
	return bk_low_pwr_core_send_msg(&msg);
}
bk_err_t bk_pm_cp0_response_cp1(uint32_t cmd, uint32_t param1,uint32_t param2,uint32_t param3)
{
	return pm_cp0_mailbox_send_data(cmd,param1,param2,param3);
}
static bk_err_t pm_cp0_mailbox_send_data(uint32_t cmd, uint32_t param1,uint32_t param2,uint32_t param3)
{
	mb_chnl_cmd_t mb_cmd = {0};
	int ret              = 0;
	uint8_t  retry_count = 0;
	bk_pm_module_vote_sleep_ctrl(PM_SLEEP_MODULE_NAME_MAILBOX,0,0);
	mb_cmd.hdr.cmd = cmd;
	mb_cmd.param1 = param1;
	mb_cmd.param2 = param2;
	mb_cmd.param3 = param3;
	ret = mb_chnl_write(MB_CHNL_PWC, &mb_cmd);
    while(ret != BK_OK)
	{
	    retry_count++;
		ret = mb_chnl_write(MB_CHNL_PWC, &mb_cmd);
		rtos_delay_milliseconds(2);
        if(retry_count > 5)
        {
            LOGE("Mailbox send data fail[ret:%d]\r\n",ret);
			bk_pm_module_vote_sleep_ctrl(PM_SLEEP_MODULE_NAME_MAILBOX,1,0);
            return ret;
        }
	}
	bk_pm_module_vote_sleep_ctrl(PM_SLEEP_MODULE_NAME_MAILBOX,1,0);
	return BK_OK;
}

static void pm_cp0_mailbox_response(uint32_t cmd, int ret)
{
	pm_cp0_mailbox_send_data(cmd,ret,0,0);
}

#if CONFIG_MAILBOX
static void pm_dump_ap_mailbox_diag(void)
{
	mbox0_adapter_diag_t adapter;
	mb_chnl_diag_t logical;
	volatile uint32_t *trace = (volatile uint32_t *)0x2800ffe0;

	memset(&adapter, 0, sizeof(adapter));
	memset(&logical, 0, sizeof(logical));
	bk_mailbox_get_diag(&adapter);
	(void)mb_chnl_get_diag(MAILBOX_CPU1, &logical);
	BK_LOGE(TAG,
		"AP mbox diag rx=%lu bad_sid=%lu len=%lu align=%lu addr=%lu "
		"overflow=%lu slot_busy=%lu reaped=%lu\r\n",
		(unsigned long)adapter.rx_accepted,
		(unsigned long)adapter.bad_sid,
		(unsigned long)adapter.bad_length,
		(unsigned long)adapter.bad_alignment,
		(unsigned long)adapter.bad_address,
		(unsigned long)adapter.descriptor_overflow,
		(unsigned long)adapter.tx_slot_busy,
		(unsigned long)adapter.tx_reaped);
	BK_LOGE(TAG,
		"AP logical diag busy=%u ch=%u seq=%u cmd=%u stale=%lu "
		"tx_fault=%lu ack_retry=%lu ack_full=%lu reset_bad=%lu ack_slots=%u\r\n",
		logical.busy, logical.logical_chnl, logical.sequence, logical.command,
		(unsigned long)logical.stale_ack,
		(unsigned long)logical.tx_fault,
		(unsigned long)logical.ack_retry,
		(unsigned long)logical.ack_queue_full,
		(unsigned long)logical.reset_bad,
		bk_mailbox_ack_slots_used(MAILBOX_CPU1));
	BK_LOGE(TAG, "AP logical raw cmd=%08lx ack=%08lx data1=%08lx\r\n",
		(unsigned long)logical.last_cmd_header,
		(unsigned long)logical.last_ack_header,
		(unsigned long)logical.last_ack_data1);
	BK_LOGE(TAG, "AP physical raw rx=%08lx cmd=%08lx ack=%08lx sent=%lu\r\n",
		(unsigned long)adapter.last_rx_header,
		(unsigned long)adapter.last_cmd_header,
		(unsigned long)adapter.last_ack_header,
		(unsigned long)adapter.ack_sent);
	BK_LOGE(TAG, "AP physical counts cmd=%lu called=%lu missing=%lu ack_fail=%lu\r\n",
		(unsigned long)adapter.cmd_received,
		(unsigned long)adapter.callback_called,
		(unsigned long)adapter.callback_missing,
		(unsigned long)adapter.ack_send_fail);
	BK_LOGE(TAG,
		"AP trace=%08lx primary=%lu secondary=%lu detail=%08lx/%08lx/%08lx/%08lx/%08lx\r\n",
		(unsigned long)trace[0], (unsigned long)trace[1],
		(unsigned long)trace[2], (unsigned long)trace[3],
		(unsigned long)trace[4], (unsigned long)trace[5],
		(unsigned long)trace[6], (unsigned long)trace[7]);
}
#endif

static void pm_cp0_mailbox_tx_cmpl_isr(int *pm_mb, mb_chnl_ack_t *cmd_buf)
{
}

static void pm_cp0_mailbox_rx_isr(int *pm_mb, mb_chnl_cmd_t *cmd_buf)
{
	bk_err_t ret = BK_OK;

	switch(cmd_buf->hdr.cmd) {
		case PM_POWER_CTRL_CMD:
			ret = pm_cp0_send_msg(LOW_PWR_CORE_POWER_CTRL, cmd_buf->param1,cmd_buf->param2,cmd_buf->param3);
			break;
		case PM_CLK_CTRL_CMD:
			ret = pm_cp0_send_msg(LOW_PWR_CORE_CLK_CTRL, cmd_buf->param1,cmd_buf->param2,cmd_buf->param3);
			break;
		case PM_SLEEP_CTRL_CMD:
			//bk_pm_cp0_response_cp1(PM_SLEEP_CTRL_CMD,BK_OK,0,0);//for more quick when enter lv
			ret = pm_cp0_send_msg(LOW_PWR_CORE_SLEEP_CTRL, cmd_buf->param1,cmd_buf->param2,cmd_buf->param3);
			break;
		case PM_CPU_FREQ_CTRL_CMD:
			ret = pm_cp0_send_msg(LOW_PWR_CORE_FREQ_CTRL, cmd_buf->param1,cmd_buf->param2,cmd_buf->param3);
			break;
		case PM_CTRL_EXTERNAL_LDO_CMD:
			ret = pm_cp0_send_msg(LOW_PWR_CORE_EXTERNAL_LDO, cmd_buf->param1,cmd_buf->param2,cmd_buf->param3);
			break;
		case PM_CTRL_PSRAM_POWER_CMD:
			ret = pm_cp0_send_msg(LOW_PWR_CORE_PSRAM_POWER, cmd_buf->param1,cmd_buf->param2,cmd_buf->param3);
			break;
		case PM_CPU1_BOOT_READY_CMD:
			if(cmd_buf->param1 == 0x1)
			{
				/* A local UART STATE ACK proves only CP->AP delivery.  Keep
				 * ordinary CP transactions quarantined until AP confirms its
				 * own probe completed and reaches boot-ready. */
				mb_chnl_recovered(MAILBOX_CPU1);
				s_pm_cp1_boot_ready = PM_MAILBOX_COMMUNICATION_FINISH;
				mb_chnl_start_service();
#if CONFIG_OPENVELA_AP_480M
				if (s_pm_openvela_boot_stage == 4)
					s_pm_openvela_boot_stage = 5;
#endif
			}
			else
			{
				s_pm_cp1_boot_ready = PM_MAILBOX_COMMUNICATION_INIT;
			}
			//if(pm_debug_mode()&0x2)//for temp debug
				BK_LOGD(NULL,"cpu0 receive the cpu1 boot success event [%d]\r\n",cmd_buf->param1);
			break;
		case PM_CP1_PSRAM_MALLOC_STATE_CMD:
			if(cmd_buf->param1 == PM_CP1_PSRAM_MALLOC_STATE_CMD)//Get the psram malloc count
			{
				s_pm_cp1_psram_malloc_count = cmd_buf->param2;
			}
			bk_pm_cp0_psram_malloc_state_set(PM_MAILBOX_COMMUNICATION_FINISH);
			break;
		case PM_CP1_RECOVERY_CMD:
			ret = pm_cp0_send_msg(LOW_PWR_CORE_CP2_RECOVERY, cmd_buf->param1,cmd_buf->param2,cmd_buf->param3);
			break;
		case PM_RTC_DEEPSLEEP_CMD:
			ret = pm_cp0_send_msg(LOW_PWR_CORE_RTC_DEEPSLEEP, cmd_buf->param1,cmd_buf->param2,cmd_buf->param3);
			break;
		case PM_GET_PM_DATA_CMD:
			ret = pm_cp0_send_msg(LOW_PWR_CORE_GET_CP_DATA, cmd_buf->param1,cmd_buf->param2,cmd_buf->param3);
			break;
		case PM_CTRL_AP_STATE_CMD:
			ret = pm_cp0_send_msg(LOW_PWR_CORE_CTRL_CP2_STATE, cmd_buf->param1,cmd_buf->param2,cmd_buf->param3);
			break;
		case PM_ENTER_DEEP_SLEEP_CMD:
			ret = pm_cp0_send_msg(LOW_PWR_CORE_STATE_ENTER_DEEPSLEEP, cmd_buf->param1,cmd_buf->param2,cmd_buf->param3);
			break;
		case PM_WAKEUP_CONFIG_CMD:
			ret = pm_cp0_send_msg(LOW_PWR_CORE_WAKEUP_SRC_CFG, cmd_buf->param1,cmd_buf->param2,cmd_buf->param3);
			break;
		case PM_OPENVELA_READY_CMD:
			ret = pm_cp0_send_msg(LOW_PWR_CORE_OPENVELA_READY,
				cmd_buf->param1, cmd_buf->param2, cmd_buf->param3);
			break;
		default:
			break;
	}
	if(ret != BK_OK)
	{
		BK_LOGD(NULL,"cp0 handle cp1 message error\r\n");
	}

	//if(pm_debug_mode()&0x2)
	{
		if(cmd_buf->hdr.cmd != PM_CP1_PSRAM_MALLOC_STATE_CMD)
		{
			BK_LOGV(NULL,"cp0_mb_rx_isr %d %d %d %d %d\r\n",cmd_buf->hdr.cmd,cmd_buf->param1,cmd_buf->param2,cmd_buf->param3,ret);
		}
	}

}
static void pm_cp0_mailbox_tx_isr(int *pm_mb)
{
}
static void pm_cp0_mailbox_init()
{
	mb_chnl_open(MB_CHNL_PWC, NULL);
	if (pm_cp0_mailbox_rx_isr != NULL)
		mb_chnl_ctrl(MB_CHNL_PWC, MB_CHNL_SET_RX_ISR, pm_cp0_mailbox_rx_isr);
	if (pm_cp0_mailbox_tx_isr != NULL)
		mb_chnl_ctrl(MB_CHNL_PWC, MB_CHNL_SET_TX_ISR, pm_cp0_mailbox_tx_isr);
	if (pm_cp0_mailbox_tx_cmpl_isr != NULL)
		mb_chnl_ctrl(MB_CHNL_PWC, MB_CHNL_SET_TX_CMPL_ISR, pm_cp0_mailbox_tx_cmpl_isr);
}
#endif //CONFIG_MAILBOX
#endif //(CONFIG_CPU_CNT > 1)


#if (CONFIG_CPU_CNT > 1)
static uint32_t s_pm_cp1_ctrl_state           = 0;
extern void start_cpu1_core(void);
extern void stop_cpu1_core(void);

bk_err_t bk_pm_cp1_recovery_module_state_ctrl(pm_cp1_prepare_close_module_name_e module,pm_cp1_module_recovery_state_e state)
{
	bk_err_t ret;

	if (!s_pm_cp1_closing || module >= PM_CP1_PREPARE_CLOSE_MODULE_NAME_MAX ||
		state > PM_CP1_MODULE_RECOVERY_STATE_FINISH)
	{
		LOGE("reject recovery state module=%d state=%d closing=%d\r\n",
			module, state, s_pm_cp1_closing);
		if (s_pm_cp1_closing)
			pm_cp1_close_cancel();
		return BK_ERR_PARAM;
	}
	if (state == PM_CP1_MODULE_RECOVERY_STATE_INIT)
	{
		LOGW("AP rejected shutdown at module %d\r\n", module);
		pm_cp1_close_cancel();
		return BK_ERR_STATE;
	}
	s_pm_cp1_module_recovery_state |= 0x1ULL << module;
	LOGD("pm_cp1_rcv:0x%llx %d %d %d\r\n",s_pm_cp1_module_recovery_state,bk_pm_cp1_work_state_get(),bk_pm_cp1_recovery_all_state_get(),s_pm_cp1_ctrl_state);
	if(bk_pm_cp1_recovery_all_state_get())
	{
		ret = bk_pm_module_check_cp1_shutdown();
		return ret;
	}
	pm_cp1_close_cancel();
	return BK_ERR_STATE;
}

bool bk_pm_cp1_recovery_all_state_get()
{
	bool cp1_all_module_recovery = false;
	if(bk_pm_cp1_work_state_get())
	{
		cp1_all_module_recovery = (s_pm_cp1_module_recovery_state == PM_CP1_RECOVERY_DEFAULT_VALUE);
	}
	return cp1_all_module_recovery;
}

static void pm_record_first_error(bk_err_t *first, bk_err_t ret)
{
	if (*first == BK_OK && ret != BK_OK)
		*first = ret;
}

static void pm_cp1_close_cancel(void)
{
	GLOBAL_INT_DECLARATION();

	GLOBAL_INT_DISABLE();
	if (s_pm_cp1_closing_module < PM_BOOT_CP1_MODULE_NAME_MAX)
		s_pm_cp1_ctrl_state |= 0x1U << s_pm_cp1_closing_module;
	s_pm_cp1_closing_module = PM_BOOT_CP1_MODULE_NAME_MAX;
	s_pm_cp1_closing = 0;
	s_pm_cp1_module_recovery_state = PM_CP1_RECOVERY_DEFAULT_VALUE;
	GLOBAL_INT_RESTORE();
	if (s_sync_cp1_open_sema != NULL)
		(void)rtos_set_semaphore(&s_sync_cp1_open_sema);
}

static bk_err_t pm_openvela_clock_validate(void)
{
#if CONFIG_OPENVELA_AP_480M
	uint32_t clk = sys_drv_all_modules_clk_div_get(CLK_DIV_REG0);
	uint32_t required = bk_pm_vdddig_required_get(PM_CPU_FRQ_480M);

	if (bk_pm_module_current_cpu_freq_get(PM_DEV_ID_CPU1) != PM_CPU_FRQ_480M ||
		bk_pm_current_max_cpu_freq_get() != PM_CPU_FRQ_480M ||
		((clk >> 4) & 0x3) != 0x3 || ((clk >> 0) & 0xf) != 0x0 ||
		((clk >> 6) & 0x1) != 0x1 ||
		sys_drv_cpu_clk_div_get(0) != 0 ||
		sys_drv_cpu_clk_div_get(1) != 1 ||
		sys_drv_cpu_clk_div_get(2) != 1 ||
		sys_hal_vdddig_h_vol_get() < required)
	{
		LOGE("OpenVela clock invalid slot/max=%d/%d clk=0x%x "
			"speed=%d/%d/%d vdddig=0x%x need=0x%x\r\n",
			bk_pm_module_current_cpu_freq_get(PM_DEV_ID_CPU1),
			bk_pm_current_max_cpu_freq_get(), clk,
			sys_drv_cpu_clk_div_get(0), sys_drv_cpu_clk_div_get(1),
			sys_drv_cpu_clk_div_get(2), sys_hal_vdddig_h_vol_get(), required);
		return BK_FAIL;
	}
#if CONFIG_RX_OPTIMIZE
	if (sys_hal_vddd_h_vol_get() < 0x7)
	{
		LOGE("OpenVela VDDD invalid: 0x%x\r\n",
			sys_hal_vddd_h_vol_get());
		return BK_FAIL;
	}
#endif
#endif
	return BK_OK;
}

static void pm_openvela_boot_rollback(void)
{
#if CONFIG_OPENVELA_AP_480M
	bk_err_t first = BK_OK;
	bk_err_t ret;

	ap_console_bridge_link_down();
#if CONFIG_MAILBOX
	mb_ipc_reset_notify(1, 0);
#endif
	ret = bk_cpu2_hold_reset();
	pm_record_first_error(&first, ret);
	if (ret != BK_OK)
		LOGE("rollback: CPU2 reset hold failed: %d\r\n", ret);
	ret = bk_cpu1_hold_reset();
	pm_record_first_error(&first, ret);
	if (ret != BK_OK)
		LOGE("rollback: CPU1 reset hold failed: %d\r\n", ret);
	ret = bk_pm_module_vote_psram_ctrl(PM_POWER_PSRAM_MODULE_NAME_OPENVELA,
		PM_POWER_MODULE_STATE_OFF);
	pm_record_first_error(&first, ret);
	if (ret != BK_OK)
		LOGE("rollback: OpenVela PSRAM release failed: %d\r\n", ret);
	ret = bk_pm_openvela_cpu2_power_hold(PM_POWER_MODULE_STATE_OFF);
	pm_record_first_error(&first, ret);
	if (ret != BK_OK)
		LOGE("rollback: CPU2 power release failed: %d\r\n", ret);
	ret = bk_pm_module_vote_power_ctrl(PM_POWER_MODULE_NAME_CPU1,
		PM_POWER_MODULE_STATE_OFF);
	pm_record_first_error(&first, ret);
	if (ret != BK_OK)
		LOGE("rollback: CPU1 power release failed: %d\r\n", ret);
	if (s_pm_openvela_freq_held)
	{
		ret = bk_pm_module_vote_cpu_freq(PM_DEV_ID_CPU1, PM_CPU_FRQ_DEFAULT);
		pm_record_first_error(&first, ret);
		if (ret == BK_OK)
			s_pm_openvela_freq_held = false;
		else
			LOGE("rollback: frequency release failed: %d\r\n", ret);
	}
	if (s_pm_openvela_cpu1_sleep_held)
	{
		ret = bk_pm_module_vote_sleep_ctrl(PM_SLEEP_MODULE_NAME_CPU1, 1, 0);
		pm_record_first_error(&first, ret);
		if (ret == BK_OK)
			s_pm_openvela_cpu1_sleep_held = false;
		else
			LOGE("rollback: CPU1 sleep vote restore failed: %d\r\n", ret);
	}
	s_pm_cp1_boot_ready = PM_MAILBOX_COMMUNICATION_INIT;
	s_pm_openvela_boot_stage = 0;
	s_pm_cp1_ctrl_state = 0;
	s_pm_cp1_closing = 0;
	s_pm_cp1_closing_module = PM_BOOT_CP1_MODULE_NAME_MAX;
	if (first != BK_OK)
		LOGE("OpenVela rollback completed with error: %d\r\n", first);
#else
	ap_console_bridge_link_down();
#if CONFIG_MAILBOX
	mb_ipc_reset_notify(1, 0);
#endif
	(void)bk_cpu1_hold_reset();
	(void)bk_pm_module_vote_power_ctrl(PM_POWER_MODULE_NAME_CPU1,
		PM_POWER_MODULE_STATE_OFF);
#if CONFIG_PM_AP_POWERDOWN_WHEN_LV
	(void)bk_pm_module_vote_sleep_ctrl(PM_SLEEP_MODULE_NAME_CPU1, 1, 0);
#endif
#endif
}

bk_err_t bk_pm_openvela_ready_handle(uint32_t ready, int32_t error)
{
#if CONFIG_OPENVELA_AP_480M
	bk_err_t ret = BK_OK;
	uint32_t owner = 0x1U << PM_POWER_PSRAM_MODULE_NAME_OPENVELA;

	if (ready == 0)
	{
		if (error >= 0)
			LOGE("OpenVela abort missing negative errno: %d\r\n", error);
		else
			LOGE("OpenVela boot aborted by AP: %d\r\n", error);
		s_pm_openvela_boot_error = error < 0 ? error : BK_FAIL;
		pm_openvela_boot_rollback();
		return error < 0 ? error : BK_FAIL;
	}
	if (ready != 1)
		return BK_ERR_PARAM;
	if (s_pm_openvela_boot_stage != 5 ||
		sys_drv_module_power_state_get(PM_POWER_MODULE_NAME_CPU1) !=
			PM_POWER_MODULE_STATE_ON ||
		sys_drv_module_power_state_get(PM_POWER_MODULE_NAME_CPU2) !=
			PM_POWER_MODULE_STATE_ON ||
		!s_pm_openvela_cpu2_held || !(s_pm_psram_ctrl_state & owner))
	{
		LOGE("OpenVela commit state invalid stage=%d cpu=%d/%d hold=%d psram=0x%x\r\n",
			s_pm_openvela_boot_stage,
			sys_drv_module_power_state_get(PM_POWER_MODULE_NAME_CPU1),
			sys_drv_module_power_state_get(PM_POWER_MODULE_NAME_CPU2),
			s_pm_openvela_cpu2_held, s_pm_psram_ctrl_state);
		ret = BK_ERR_STATE;
	}
	if (ret == BK_OK)
		ret = pm_openvela_clock_validate();
	if (ret != BK_OK)
	{
		s_pm_openvela_boot_error = ret;
		return ret;
	}
	s_pm_openvela_boot_stage = 6;
	ap_console_bridge_ready_update(AP_BRIDGE_READY_PWC, true);
	(void)bk_pm_openvela_diag_dump();
	return BK_OK;
#else
	(void)ready;
	(void)error;
	return BK_ERR_NOT_SUPPORT;
#endif
}

static bk_err_t pm_module_bootup_cpu1(pm_power_module_name_e module)
{
	uint64_t previous_tick = 0;
	uint64_t current_tick   = 0;
	bk_err_t ret;
#if CONFIG_OPENVELA_AP_480M
	if (module == PM_POWER_MODULE_NAME_CPU1 &&
		PM_POWER_MODULE_STATE_OFF != sys_drv_module_power_state_get(module))
	{
		LOGE("CPU1 powered before OpenVela lifecycle transaction\r\n");
		return BK_ERR_STATE;
	}
#endif
	if(PM_POWER_MODULE_STATE_OFF == sys_drv_module_power_state_get(module))
	{
		if(module == PM_POWER_MODULE_NAME_CPU1)
		{
#if CONFIG_OPENVELA_AP_480M
			s_pm_openvela_boot_error = BK_OK;
			ret = bk_pm_module_vote_cpu_freq(PM_DEV_ID_CPU1, PM_CPU_FRQ_480M);
			if (ret != BK_OK)
				return ret;
			s_pm_openvela_freq_held = true;
			s_pm_openvela_boot_stage = 1;
			ret = pm_openvela_clock_validate();
			if (ret != BK_OK)
				goto rollback;
			ret = bk_pm_module_vote_sleep_ctrl(PM_SLEEP_MODULE_NAME_CPU1, 0, 0);
			if (ret != BK_OK)
				goto rollback;
			s_pm_openvela_cpu1_sleep_held = true;
			if (!bk_cpu2_reset_is_held())
			{
				LOGE("CPU2 is not reset-held ctrl=0x%x run=0x%x\r\n",
					bk_cpu2_ctrl_get(), bk_cpu_run_status_get());
				ret = BK_ERR_STATE;
				goto rollback;
			}
			ret = bk_pm_openvela_cpu2_power_hold(PM_POWER_MODULE_STATE_ON);
			if (ret != BK_OK)
				goto rollback;
			s_pm_openvela_boot_stage = 2;
#else
#if CONFIG_PM_AP_POWERDOWN_WHEN_LV
			ret = bk_pm_module_vote_sleep_ctrl(PM_SLEEP_MODULE_NAME_CPU1, 0, 0);
			if (ret != BK_OK)
				return ret;
#endif
#endif
			ret = bk_pm_module_vote_power_ctrl(PM_POWER_MODULE_NAME_CPU1,
				PM_POWER_MODULE_STATE_ON);
			if (ret != BK_OK ||
				sys_drv_module_power_state_get(PM_POWER_MODULE_NAME_CPU1) !=
				PM_POWER_MODULE_STATE_ON)
			{
#if CONFIG_OPENVELA_AP_480M
				if (sys_drv_module_power_state_get(PM_POWER_MODULE_NAME_CPU1) ==
					PM_POWER_MODULE_STATE_ON)
					s_pm_openvela_boot_stage = 3;
#endif
				ret = ret == BK_OK ? BK_FAIL : ret;
				goto rollback;
			}
#if CONFIG_OPENVELA_AP_480M
			s_pm_openvela_boot_stage = 3;
#endif

			#if defined(RECV_LOG_FROM_MBOX)
			void reset_forward_log_status(void);
			// reset cpu1's log transfer status on cpu0.
			reset_forward_log_status();
			#endif

			#if CONFIG_OPENVELA_AP_480M
			s_pm_openvela_boot_stage = 4;
			#endif
			mb_chnl_quiesce(MAILBOX_CPU1);
			ret = bk_cpu1_start_partition();
			if (ret != BK_OK)
				goto rollback;
			mb_ipc_reset_notify(1, 1);

			previous_tick = bk_aon_rtc_get_current_tick(AON_RTC_ID_1);
			current_tick = previous_tick;
			while((current_tick - previous_tick) < (PM_BOOT_CP1_WAITING_TIEM*AON_RTC_MS_TICK_CNT))
			{
				if (bk_pm_cp1_work_state_get()
#if CONFIG_OPENVELA_AP_480M
					|| s_pm_openvela_boot_error != BK_OK
#endif
				)
				{
					break;
				}
				current_tick = bk_aon_rtc_get_current_tick(AON_RTC_ID_1);
				rtos_delay_milliseconds(1);
			}
			#if CONFIG_OPENVELA_AP_480M
			if (s_pm_openvela_boot_error != BK_OK)
				return s_pm_openvela_boot_error;
			#endif

			if(!bk_pm_cp1_work_state_get())
			{
				volatile uint32_t *trace = (volatile uint32_t *)0x2800ffe0;

				LOGE("CPU1 boot timeout stage=%d\r\n",
					s_pm_openvela_boot_stage);
				#if CONFIG_MAILBOX
				pm_dump_ap_mailbox_diag();
				#endif
				LOGE("AP trace=%08x primary=%u secondary=%u detail=%08x/%08x/%08x/%08x/%08x\r\n",
					trace[0], trace[1], trace[2], trace[3], trace[4],
					trace[5], trace[6], trace[7]);
				ret = BK_ERR_TIMEOUT;
				goto rollback;
			}
#if CONFIG_OPENVELA_AP_480M
			if (s_pm_openvela_boot_stage == 4)
				s_pm_openvela_boot_stage = 5;
			(void)bk_pm_openvela_diag_dump();
#endif
		}
	}
	return BK_OK;

rollback:
	pm_openvela_boot_rollback();
	(void)bk_pm_openvela_diag_dump();
	return ret;
}
bk_err_t bk_pm_module_check_cp1_shutdown()
{
	if(0x0 == s_pm_cp1_ctrl_state)
	{
		return pm_module_shutdown_cpu1(PM_POWER_MODULE_NAME_CPU1);
	}
    return BK_OK;
}
static bk_err_t pm_module_shutdown_cpu1(pm_power_module_name_e module)
{
	bk_err_t first = BK_OK;
	bk_err_t ret;
	GLOBAL_INT_DECLARATION();

	if (module != PM_POWER_MODULE_NAME_CPU1)
		return BK_ERR_PARAM;
	ap_console_bridge_link_down();
#if CONFIG_MAILBOX
	mb_ipc_reset_notify(1, 0);
#endif
#if CONFIG_OPENVELA_AP_480M
	ret = bk_cpu2_hold_reset();
	pm_record_first_error(&first, ret);
	if (ret != BK_OK)
		LOGE("shutdown: CPU2 reset hold failed: %d\r\n", ret);
	ret = bk_pm_openvela_cpu2_power_hold(PM_POWER_MODULE_STATE_OFF);
	pm_record_first_error(&first, ret);
	if (ret != BK_OK)
		LOGE("shutdown: CPU2 power release failed: %d\r\n", ret);
	ret = bk_pm_module_vote_psram_ctrl(PM_POWER_PSRAM_MODULE_NAME_OPENVELA,
		PM_POWER_MODULE_STATE_OFF);
	pm_record_first_error(&first, ret);
	if (ret != BK_OK)
		LOGE("shutdown: OpenVela PSRAM release failed: %d\r\n", ret);
#endif
	ret = bk_cpu1_hold_reset();
	pm_record_first_error(&first, ret);
	if (ret != BK_OK)
		LOGE("shutdown: CPU1 reset hold failed: %d\r\n", ret);
#if CONFIG_PM_AP_POWERDOWN_WHEN_LV
	ret = bk_pm_module_vote_psram_ctrl(PM_POWER_PSRAM_MODULE_NAME_MEDIA,
		PM_POWER_MODULE_STATE_OFF);
	pm_record_first_error(&first, ret);
#endif
	ret = bk_pm_module_vote_power_ctrl(PM_POWER_MODULE_NAME_CPU1,
		PM_POWER_MODULE_STATE_OFF);
	pm_record_first_error(&first, ret);
	if (ret != BK_OK)
		LOGE("shutdown: CPU1 power release failed: %d\r\n", ret);
#if CONFIG_OPENVELA_AP_480M
	if (s_pm_openvela_freq_held)
	{
		ret = bk_pm_module_vote_cpu_freq(PM_DEV_ID_CPU1, PM_CPU_FRQ_DEFAULT);
		pm_record_first_error(&first, ret);
		if (ret == BK_OK)
			s_pm_openvela_freq_held = false;
	}
	if (s_pm_openvela_cpu1_sleep_held)
	{
		ret = bk_pm_module_vote_sleep_ctrl(PM_SLEEP_MODULE_NAME_CPU1, 1, 0);
		pm_record_first_error(&first, ret);
		if (ret == BK_OK)
			s_pm_openvela_cpu1_sleep_held = false;
	}
	s_pm_openvela_boot_stage = 0;
#elif CONFIG_PM_AP_POWERDOWN_WHEN_LV
	ret = bk_pm_module_vote_sleep_ctrl(PM_SLEEP_MODULE_NAME_CPU1, 1, 0);
	pm_record_first_error(&first, ret);
#endif
	GLOBAL_INT_DISABLE();
	s_pm_cp1_boot_ready = PM_MAILBOX_COMMUNICATION_INIT;
	s_pm_cp1_closing = 0;
	s_pm_cp1_closing_module = PM_BOOT_CP1_MODULE_NAME_MAX;
	s_pm_cp1_boot_try_count = 0;
	s_pm_cp1_module_recovery_state = PM_CP1_RECOVERY_DEFAULT_VALUE;
	GLOBAL_INT_RESTORE();
	if (s_sync_cp1_open_sema != NULL)
		(void)rtos_set_semaphore(&s_sync_cp1_open_sema);
	bk_printf_nonblock(4, NULL, "Shutdown_cp1[%d][%d][%d]\r\n",
		s_pm_cp1_closing, first, s_pm_cp1_sema_count);
	return first;
}

bk_err_t bk_pm_module_vote_boot_cp1_ctrl(pm_boot_cp1_module_name_e module,pm_power_module_state_e power_state)
{
	bk_err_t ret = BK_OK;
	GLOBAL_INT_DECLARATION();

	BK_LOGD(NULL,"boot_cp1 %d %d 0x%x [%d][0x%x]E_1\r\n",module, power_state,s_pm_cp1_ctrl_state,s_pm_cp1_closing,&s_sync_cp1_open_sema);
	if (NULL == s_sync_cp1_open_sema)
	{
		rtos_init_semaphore(&s_sync_cp1_open_sema, 1);
	}
	if(s_pm_cp1_closing)
	{
		GLOBAL_INT_DISABLE();
		s_pm_cp1_sema_count++;
		GLOBAL_INT_RESTORE();
		BK_LOGD(NULL,"boot_cp1 get sema[%d][0x%x]\r\n",s_pm_cp1_sema_count,&s_sync_cp1_open_sema);

		/*add protect:init again when the s_sync_cp1_open_sema free*/
		if (NULL == s_sync_cp1_open_sema)
		{
			rtos_init_semaphore(&s_sync_cp1_open_sema, 1);
		}
		ret = rtos_get_semaphore(&s_sync_cp1_open_sema, PM_OPEN_CP1_TIMEOUT);

		GLOBAL_INT_DISABLE();
		s_pm_cp1_sema_count--;
		GLOBAL_INT_RESTORE();
		if(ret == kTimeoutErr)
		{
			BK_LOGD(NULL,"boot_cp1[%d]0x%llx %d %d %d\r\n",ret,s_pm_cp1_module_recovery_state,bk_pm_cp1_work_state_get(),bk_pm_cp1_recovery_all_state_get(),s_pm_cp1_ctrl_state);
			if (s_pm_cp1_closing)
				pm_cp1_close_cancel();
			return ret;
		}
	}
	BK_LOGD(NULL,"boot_cp1 %d %d 0x%x [%d]E_2\r\n",module, power_state,s_pm_cp1_ctrl_state,ret);
    if(power_state == PM_POWER_MODULE_STATE_ON)//power on
    {
		GLOBAL_INT_DISABLE();
		s_pm_cp1_ctrl_state |= 0x1 << (module);
		GLOBAL_INT_RESTORE();
		ret = pm_module_bootup_cpu1(PM_POWER_MODULE_NAME_CPU1);
		if (ret != BK_OK)
		{
			GLOBAL_INT_DISABLE();
			s_pm_cp1_ctrl_state &= ~(0x1 << module);
			GLOBAL_INT_RESTORE();
			return ret;
		}
    }
    else //power down
    {
		if(s_pm_cp1_ctrl_state&(0x1 << (module)))
		{
			GLOBAL_INT_DISABLE();
			s_pm_cp1_ctrl_state &= ~(0x1 << (module));
			GLOBAL_INT_RESTORE();
			if(0x0 == s_pm_cp1_ctrl_state)
			{
				s_pm_cp1_closing = 1;
				s_pm_cp1_closing_module = module;
				BK_LOGD(NULL,"boot_cp1 %d %d close 0x%llx %d\r\n",module, power_state,s_pm_cp1_module_recovery_state,s_pm_cp1_boot_ready);
				ret = pm_cp0_mailbox_send_data(PM_CP1_RECOVERY_CMD,0,0,0);
				if (ret != BK_OK)
				{
					pm_cp1_close_cancel();
					return ret;
				}
				//pm_module_shutdown_cpu1(PM_POWER_MODULE_NAME_CPU1);
			}
		}
    }

    return BK_OK;
}
bk_err_t bk_pm_cp_wakeup_ap_from_wfi(uint8_t core_id)
{
	int ret                       = BK_OK;
#if CONFIG_PM_LV_SUBCORES_ON && !CONFIG_PM_AP_POWERDOWN_WHEN_LV
	mb_chnl_cmd_t mb_cmd          = {0};

	mb_cmd.hdr.cmd = PM_SLEEP_WAKEUP_NOTIFY_CMD;
	mb_cmd.param1 = 0;
	mb_cmd.param2 = 0;
	mb_cmd.param3 = 0;
	ret = mb_chnl_write(MB_CHNL_PWC, &mb_cmd);
	if(ret == BK_ERR_BUSY)
	{
		BK_LOGI(NULL,"Mb busy[%d]wait next wakeup\r\n",ret);
		ret = BK_FAIL;
	}
	else if(ret == BK_OK)
	{
	}
	else
	{
		BK_LOGE(NULL,"Mb write error[%d]\r\n",ret);
	}
	FIXED_ADDR_WAKEUP_CP_COUNT += 1;

#endif
	return ret;
}
/*Get the cp1 heap malloc count*/
uint32_t bk_pm_get_cp1_psram_malloc_count(uint32_t using_psram_type)
{
	uint64_t previous_tick = 0;
	uint64_t current_tick   = 0;
	if(s_pm_cp1_boot_ready)
	{
		bk_pm_cp0_psram_malloc_state_set(PM_MAILBOX_COMMUNICATION_INIT);
		pm_cp0_mailbox_send_data(PM_CP1_PSRAM_MALLOC_STATE_CMD,using_psram_type,0,0);
		if(using_psram_type == 0x0)
		{
			s_pm_cp1_psram_malloc_count = 0;
			previous_tick = bk_aon_rtc_get_current_tick(AON_RTC_ID_1);
			current_tick = previous_tick;
			while((current_tick - previous_tick) < (PM_SEND_CMD_CP1_RESPONSE_TIEM*AON_RTC_MS_TICK_CNT))
			{
				if (bk_pm_cp0_psram_malloc_state_get()) // wait the cp1 response
				{
					break;
				}
				current_tick = bk_aon_rtc_get_current_tick(AON_RTC_ID_1);
			}
			if(!bk_pm_cp0_psram_malloc_state_get())
			{
				BK_LOGD(NULL,"cp0 get the psram malloc state[%d] time out > 100ms\r\n",using_psram_type);
			}

			return s_pm_cp1_psram_malloc_count;
		}
	}
	else
	{
		return 0;
	}
	return 0;
}

/*trigger the cp1 heap malloc dump*/
bk_err_t bk_pm_dump_cp1_psram_malloc_info()
{
	if(s_pm_cp1_boot_ready)
	{
		pm_cp0_mailbox_send_data(PM_CP1_DUMP_PSRAM_MALLOC_INFO_CMD,0,0,0);
	}
    return BK_OK;
}

#endif

#if (CONFIG_CPU_CNT > 2)
static volatile  pm_mailbox_communication_state_e s_pm_cp2_boot_ready        = 0;
extern void start_cpu2_core(void);
extern void stop_cpu2_core(void);
static void pm_module_bootup_cpu2(pm_power_module_name_e module)
{
	if(PM_POWER_MODULE_STATE_OFF == sys_drv_module_power_state_get(module))
	{
		if(module == PM_POWER_MODULE_NAME_CPU2)
		{
            bk_pm_module_vote_power_ctrl(PM_POWER_MODULE_NAME_CPU2, PM_POWER_MODULE_STATE_ON);
#if !CONFIG_OPENVELA_AP_480M
            start_cpu2_core();
#endif
            //while(!s_pm_cp2_boot_ready);
		}
	}
}
static void pm_module_shutdown_cpu2(pm_power_module_name_e module)
{
	GLOBAL_INT_DECLARATION();
	if(PM_POWER_MODULE_STATE_ON == sys_drv_module_power_state_get(module))
	{
		if(module == PM_POWER_MODULE_NAME_CPU2 && !s_pm_openvela_cpu2_held)
		{
            stop_cpu2_core();
		    bk_pm_module_vote_power_ctrl(PM_POWER_MODULE_NAME_CPU2, PM_POWER_MODULE_STATE_OFF);
			GLOBAL_INT_DISABLE();
			s_pm_cp2_boot_ready = 0;
			GLOBAL_INT_RESTORE();
		}
	}
}
bk_err_t bk_pm_module_vote_boot_cp2_ctrl(pm_boot_cp2_module_name_e module,pm_power_module_state_e power_state)
{
#if CONFIG_OPENVELA_AP_480M
	(void)module;
	(void)power_state;
	return BK_ERR_NOT_SUPPORT;
#else
	GLOBAL_INT_DECLARATION();

    if(power_state == PM_POWER_MODULE_STATE_ON)//power on
    {
        GLOBAL_INT_DISABLE();
        s_pm_cp2_ctrl_state |= 0x1 << (module);
        GLOBAL_INT_RESTORE();
        pm_module_bootup_cpu2(PM_POWER_MODULE_NAME_CPU2);
    }
    else //power down
    {
		GLOBAL_INT_DISABLE();
		s_pm_cp2_ctrl_state &= ~(0x1 << (module));
		GLOBAL_INT_RESTORE();
		if(0x0 == s_pm_cp2_ctrl_state)
		{
			pm_module_shutdown_cpu2(PM_POWER_MODULE_NAME_CPU2);
		}
    }
    return BK_OK;
#endif
}

bk_err_t bk_pm_openvela_cpu2_power_hold(pm_power_module_state_e power_state)
{
#if CONFIG_OPENVELA_AP_480M
	bk_err_t ret;

	if (power_state == PM_POWER_MODULE_STATE_ON)
	{
		bool powered_here = false;

		if (s_pm_openvela_cpu2_held)
			return BK_OK;
		if (s_pm_cp2_ctrl_state != 0)
			return BK_ERR_BUSY;
		if (!bk_cpu2_reset_is_held())
			return BK_ERR_STATE;
		ret = BK_OK;
		if (sys_drv_module_power_state_get(PM_POWER_MODULE_NAME_CPU2) !=
			PM_POWER_MODULE_STATE_ON)
		{
			ret = bk_pm_module_vote_power_ctrl(PM_POWER_MODULE_NAME_CPU2,
				PM_POWER_MODULE_STATE_ON);
			powered_here = ret == BK_OK;
		}
		if (ret != BK_OK ||
			sys_drv_module_power_state_get(PM_POWER_MODULE_NAME_CPU2) !=
			PM_POWER_MODULE_STATE_ON || !bk_cpu2_reset_is_held())
		{
			if (powered_here)
				(void)bk_pm_module_vote_power_ctrl(PM_POWER_MODULE_NAME_CPU2,
					PM_POWER_MODULE_STATE_OFF);
			return ret == BK_OK ? BK_FAIL : ret;
		}
		s_pm_openvela_cpu2_held = true;
		return BK_OK;
	}

	if (!s_pm_openvela_cpu2_held)
		return BK_OK;
	if (!bk_cpu2_reset_is_held())
		return BK_ERR_STATE;
	s_pm_openvela_cpu2_held = false;
	if (s_pm_cp2_ctrl_state == 0)
	{
		ret = bk_pm_module_vote_power_ctrl(PM_POWER_MODULE_NAME_CPU2,
			PM_POWER_MODULE_STATE_OFF);
		if (ret != BK_OK ||
			sys_drv_module_power_state_get(PM_POWER_MODULE_NAME_CPU2) !=
			PM_POWER_MODULE_STATE_OFF)
		{
			s_pm_openvela_cpu2_held = true;
			return ret == BK_OK ? BK_FAIL : ret;
		}
	}
#else
	(void)power_state;
	return BK_ERR_NOT_SUPPORT;
#endif
	return BK_OK;
}
#endif

static bk_err_t pm_psram_power_ctrl(pm_power_psram_module_name_e module,pm_power_module_state_e power_state)
{
	bk_err_t ret = BK_OK;
#if CONFIG_PSRAM
	GLOBAL_INT_DECLARATION();
	uint32_t owner_bit;

	if (module >= PM_POWER_PSRAM_MODULE_NAME_MAX)
		return BK_ERR_PARAM;
	owner_bit = 0x1U << module;

    if(power_state == PM_POWER_MODULE_STATE_ON)//power on
    {
		if (s_pm_psram_ctrl_state & owner_bit)
			return BK_OK;
		if(s_pm_psram_ctrl_state == 0)
		{
			ret = bk_pm_module_vote_vdddig_ctrl(PM_VDDDIG_MODULE_PSRAM,
				PM_VDDDIG_HIGH_STATE_ON);
			if (ret != BK_OK)
				return ret;
		}
		ret = bk_psram_init();
		if(ret != BK_OK)
		{
			LOGE("psram init owner=%d ret=%d\r\n", module, ret);
			if (s_pm_psram_ctrl_state == 0)
				bk_pm_module_vote_vdddig_ctrl(PM_VDDDIG_MODULE_PSRAM,
					PM_VDDDIG_HIGH_STATE_OFF);
			return ret;
		}
		GLOBAL_INT_DISABLE();
		s_pm_psram_ctrl_state |= owner_bit;
        GLOBAL_INT_RESTORE();
	}
    else //power down
    {
		if(s_pm_psram_ctrl_state & owner_bit)
		{
			GLOBAL_INT_DISABLE();
			s_pm_psram_ctrl_state &= ~owner_bit;
			GLOBAL_INT_RESTORE();
			#if !CONFIG_PM_PSRAM_FORCE_ON
			if(0x0 == s_pm_psram_ctrl_state)
			{
				ret = bk_psram_deinit();
				if (ret != BK_OK)
				{
					GLOBAL_INT_DISABLE();
					s_pm_psram_ctrl_state |= owner_bit;
					GLOBAL_INT_RESTORE();
					return ret;
				}
				bk_pm_module_vote_vdddig_ctrl(PM_VDDDIG_MODULE_PSRAM,PM_VDDDIG_HIGH_STATE_OFF);
				FIXED_ADDR_PSRAM_POWER_DOWN = PM_PSRAM_POWER_DOWN_MAGIC;
                bk_pm_get_cp1_psram_malloc_count(0x1);
			}
			#endif
		}
	}
#endif
	return ret;
}

bk_err_t pm_debug_pwr_clk_state()
{
#if CONFIG_PSRAM
	uint8_t psram_config_state = 0;
	#if CONFIG_PM_PSRAM_FORCE_ON
		psram_config_state = 1;
	#endif
	BK_LOGI(NULL,"pm_psram:0x%x 0x%x[force on:%d]\r\n",s_pm_psram_ctrl_state,bk_psram_heap_init_flag_get(),psram_config_state,psram_config_state);
#endif
#if (CONFIG_CPU_CNT > 1)
	BK_LOGI(NULL,"pm_cp1_ctr:0x%x \r\n",s_pm_cp1_ctrl_state);
#endif
	BK_LOGI(NULL,"pm_cp1_boot_ready:0x%x 0x%x\r\n",s_pm_cp1_boot_ready,s_pm_cp1_module_recovery_state);
	return BK_OK;
}
uint32_t bk_pm_get_psram_ctrl_state()
{
	uint32_t psram_ctrl_state = 0x1;//Default psram used and power on
	#if CONFIG_PSRAM
	if(s_pm_psram_ctrl_state == 0x0)
	{
		psram_ctrl_state = 0x0;//psram state:power off
	}
	#endif
	return psram_ctrl_state;
}
bk_err_t bk_pm_module_vote_psram_ctrl(pm_power_psram_module_name_e module,pm_power_module_state_e power_state)
{
	bk_err_t ret = BK_OK;
	ret = pm_psram_power_ctrl(module,power_state);
	return ret;
}

bk_err_t bk_pm_module_vote_ctrl_external_ldo(uint32_t module,gpio_id_t gpio_id,gpio_output_state_e value)
{
	bk_gpio_ctrl_external_ldo(module,gpio_id,value);
	return BK_OK;
}
bk_err_t bk_pm_module_vote_vdddig_ctrl(pm_vdddig_module_e module,pm_vdddig_high_state_e state)
{
#if CONFIG_SYS_CPU0
	uint32_t target;
	uint32_t previous_state;
	bk_err_t ret;

	if (module >= PM_VDDDIG_MODULE_MAX)
		return BK_ERR_PARAM;
	previous_state = s_pm_vdddig_ctrl_state;
	if(state == PM_VDDDIG_HIGH_STATE_ON)
	{
		s_pm_vdddig_ctrl_state |= 0x1 << module;
	}
	else
	{
		s_pm_vdddig_ctrl_state &= ~(0x1 << module);
	}
	target = bk_pm_vdddig_required_get(bk_pm_current_max_cpu_freq_get());
	ret = sys_hal_set_vdddig_h_vol(target);
	if (ret != BK_OK || sys_hal_vdddig_h_vol_get() != target)
	{
		s_pm_vdddig_ctrl_state = previous_state;
		return BK_FAIL;
	}
#endif
	return BK_OK;
}

uint32_t bk_pm_vdddig_required_get(pm_cpu_freq_e cpu_freq)
{
	const cpu_freq_vdddig_t map[] = CPU_FREQ_VDDDIG_MAP;
	uint32_t target = PM_VDDDIG_DEFAULT;

	for (uint32_t i = 0; i < sizeof(map) / sizeof(map[0]); i++)
	{
		if (map[i].cpu_freq == cpu_freq)
		{
			target = map[i].vdddig;
			break;
		}
	}
	if ((s_pm_vdddig_ctrl_state & (0x1U << PM_VDDDIG_MODULE_PSRAM)) &&
		target < PM_VDDDIG_095)
		target = PM_VDDDIG_095;
	if ((s_pm_vdddig_ctrl_state & (0x1U << PM_VDDDIG_MODULE_CPU_FREQ)) &&
		target < PM_VDDDIG_095)
		target = PM_VDDDIG_095;
	return target;
}

bk_err_t bk_pm_openvela_diag_get(pm_openvela_diag_t *diag)
{
	if (!diag)
		return BK_ERR_NULL_PARAM;
	diag->cpu1_vote = bk_pm_module_current_cpu_freq_get(PM_DEV_ID_CPU1);
	diag->max_vote = bk_pm_current_max_cpu_freq_get();
	diag->clk_div_reg0 = sys_drv_all_modules_clk_div_get(CLK_DIV_REG0);
	for (uint32_t i = 0; i < 3; i++)
		diag->cpu_speed[i] = sys_drv_cpu_clk_div_get(i);
#if CONFIG_SOC_BK7258
	diag->vddd = sys_hal_vddd_h_vol_get();
#else
	diag->vddd = 0;
#endif
	diag->vdddig = sys_hal_vdddig_h_vol_get();
#if CONFIG_CPU_CNT > 1
	diag->cpu1_power = sys_drv_module_power_state_get(PM_POWER_MODULE_NAME_CPU1);
	diag->cpu1_ctrl = bk_cpu1_ctrl_get();
#else
	diag->cpu1_power = PM_POWER_MODULE_STATE_OFF;
	diag->cpu1_ctrl = 0;
#endif
#if CONFIG_CPU_CNT > 2
	diag->cpu2_power = sys_drv_module_power_state_get(PM_POWER_MODULE_NAME_CPU2);
	diag->cpu2_ctrl = bk_cpu2_ctrl_get();
	diag->cpu_run_status = bk_cpu_run_status_get();
#else
	diag->cpu2_power = PM_POWER_MODULE_STATE_OFF;
	diag->cpu2_ctrl = 0;
	diag->cpu_run_status = 0;
#endif
#if CONFIG_PSRAM
	diag->psram_owners = s_pm_psram_ctrl_state;
#else
	diag->psram_owners = 0;
#endif
	diag->boot_stage = s_pm_openvela_boot_stage;
	return BK_OK;
}

bk_err_t bk_pm_openvela_diag_dump(void)
{
	pm_openvela_diag_t d;
	bk_err_t ret = bk_pm_openvela_diag_get(&d);
	uint32_t owner;

	if (ret != BK_OK)
		return ret;
	BK_LOGI(NULL, "OpenVela PM stage=%d vote=%d/%d clk=0x%x "
		"speed=%d/%d/%d vol=%d/%d power=%d/%d ctrl=0x%x/0x%x "
		"run=0x%x psram=0x%x\r\n", d.boot_stage, d.cpu1_vote,
		d.max_vote, d.clk_div_reg0, d.cpu_speed[0], d.cpu_speed[1],
		d.cpu_speed[2], d.vddd, d.vdddig, d.cpu1_power, d.cpu2_power,
		d.cpu1_ctrl, d.cpu2_ctrl, d.cpu_run_status, d.psram_owners);
	for (owner = 0; owner < PM_DEV_ID_MAX; owner++)
	{
		pm_cpu_freq_e vote = bk_pm_module_current_cpu_freq_get(owner);

		if (vote != PM_CPU_FRQ_26M || owner == PM_DEV_ID_DEFAULT ||
			owner == PM_DEV_ID_CPU1)
			BK_LOGI(NULL, "OpenVela PM frequency owner[%d]=%d\r\n",
				owner, vote);
	}
	bk_pm_cpu_freq_dump();
	return BK_OK;
}

void bk_pm_openvela_mailbox_diag_dump(void)
{
#if CONFIG_MAILBOX && (CONFIG_CPU_CNT > 1)
	pm_dump_ap_mailbox_diag();
#endif
}

uint8_t bk_pm_cp_mb_busy(void)
{
    uint8_t state = 0;
    mb_chnl_ctrl(MB_CHNL_PWC,MB_CHNL_GET_STATUS, &state);
    return (state == PM_CHNL_STATE_BUSY) ? 1 : 0;
}
