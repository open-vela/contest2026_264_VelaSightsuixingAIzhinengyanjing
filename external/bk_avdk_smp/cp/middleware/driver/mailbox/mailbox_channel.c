// Copyright 2020-2022 Beken
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

#include <stdio.h>
#include <string.h>

#include <os/os.h>
#include "cmsis_gcc.h"
#include <driver/mailbox_channel.h>
#include "mailbox_driver_base.h"
#include "mbox0_adapter.h"

#define MB_PHY_CMD_CHNL		(MAILBOX_BOX0)
#define MB_PHY_ACK_CHNL		(MAILBOX_BOX1)

#define CHNL_STATE_BUSY		1
#define CHNL_STATE_IDLE		0

/* If a physical channel stays BUSY longer than this (in milliseconds)
 * without seeing the matching ACK from the peer CPU, mb_chnl_write() will
 * forcibly recover it. Normal mailbox round-trips are << 1 ms, so 200 ms
 * is well clear of any legitimate burst while still keeping the system
 * responsive in the unlikely event the peer never replies. */
#define MB_PHY_CHNL_BUSY_TIMEOUT_MS		200
#define MB_ACK_QUEUE_LEN			8
#define MB_UART_STATE_COMMAND		1

typedef struct
{
	volatile u8		tx_state;	/* physical channel tx state. */
	u8		tx_seq;				/* physical channel tx sequence. */
	u8		tx_log_chnl;		/* logical channel. */ /* bit7~bit4: dst/src CPU_ID, bit3~bit0: log_chnl_idx. */
	u8		tx_hdr_cmd;
	u32		rx_fault_cnt;
	u32		tx_fault_cnt;

	u32		param1;
	u32		param2;
	u32		param3;
	/* Wall-clock (ms via rtos_get_time()) at which tx_state was set to
	 * CHNL_STATE_BUSY. Used by mb_chnl_write() to detect a physical
	 * channel that has been BUSY abnormally long (peer lost / dropped
	 * the ACK) and forcibly clear it. 0 means "channel is IDLE". */
	volatile u32	busy_since_ms;
	u8		recovering;
	u32		ack_retry_cnt;
	u32		ack_queue_full_cnt;
	u32		reset_bad_cnt;
	u32		last_cmd_header;
	u32		last_ack_header;
	u32		last_ack_data1;
} mb_phy_chnl_cb_t;

typedef struct {
	mailbox_data_t data;
	u8 destination;
} mb_ack_entry_t;


#define CHNL_CTRL_MASK			0xF
/*
 *  there are 2 boxes in one MAILBOXn HW,
 *  but no way to know which box this msg is from in current mailbox_driver design.
 *  use the CHNL_CTRL_ACK_BOX bit in the msg hdr.ctrl to distinguish where it is from.
 *  when CHNL_CTRL_ACK_BOX is set, it means from ack box ( MAILBOXn_BOX1 ).
 */
#define CHNL_CTRL_ACK_BOX		0x01

#define CHNL_CTRL_SYNC_TX		0x02
#define CHNL_CTRL_RESET			0x04

typedef union
{
	struct
	{
		u32		cmd            : 8;
		u32		state          : 4;
		u32		ctrl           : 4;
		u32		tx_seq         : 8;
		u32		logical_chnl   : 8;
	} ;
	u32		data;
} phy_chnnl_hdr_t;

typedef struct
{
	phy_chnnl_hdr_t	hdr;

	u32		param1;
	u32		param2;
	u32		param3;
} mb_phy_chnl_cmd_t;

typedef struct
{
	phy_chnnl_hdr_t	hdr;

	u32		data1;
	u32		data2;
	u32		data3;
} mb_phy_chnl_ack_t;

typedef union
{
	phy_chnnl_hdr_t		phy_ch_hdr;
	mb_phy_chnl_cmd_t	phy_ch_cmd;
	mb_phy_chnl_ack_t	phy_ch_ack;
	mailbox_data_t		mb_data;
} mb_phy_chnl_union_t;

typedef struct
{
	volatile u8		tx_state;				/* logical channel tx state. */
	u8				in_used;
	chnl_rx_isr_t		rx_isr;
	chnl_rx_status_isr_t	rx_status_isr;
	chnl_tx_isr_t		tx_isr;
	chnl_tx_cmpl_isr_t	tx_cmpl_isr;
	chnl_event_isr_t		event_isr;
	void *				isr_param;
	mailbox_data_t		chnnl_tx_buff;		/* logical channel tx buffer. */
} mb_log_chnl_cb_t;

#define PHY_CHNL_NUM		SYSTEM_CPU_NUM

static mb_phy_chnl_cb_t		phy_chnl_x_cb[PHY_CHNL_NUM];
static mb_log_chnl_cb_t		log_chnl_cb0[CP0_MB_LOG_CHNL_END - CP0_MB_LOG_CHNL_START];
static mb_log_chnl_cb_t		log_chnl_cb1[CP1_MB_LOG_CHNL_END - CP1_MB_LOG_CHNL_START];
#if PHY_CHNL_NUM > 2
static mb_log_chnl_cb_t		log_chnl_cb2[CP2_MB_LOG_CHNL_END - CP2_MB_LOG_CHNL_START];
#endif
#if PHY_CHNL_NUM > 3
static mb_log_chnl_cb_t		log_chnl_cb3[CP3_MB_LOG_CHNL_END - CP3_MB_LOG_CHNL_START];
#endif

static const u8 	phy_chnl_log_chnl_num[PHY_CHNL_NUM] = {
	ARRAY_SIZE(log_chnl_cb0),
	ARRAY_SIZE(log_chnl_cb1),
#if PHY_CHNL_NUM > 2
	ARRAY_SIZE(log_chnl_cb2),
#endif
#if PHY_CHNL_NUM > 3
	ARRAY_SIZE(log_chnl_cb3),
#endif
//	CP0_MB_LOG_CHNL_END - CP0_MB_LOG_CHNL_START,
	};

static const mb_log_chnl_cb_t *	phy_chnl_log_chnl_list[PHY_CHNL_NUM] = {
	&log_chnl_cb0[0],
	&log_chnl_cb1[0],
#if PHY_CHNL_NUM > 2
	&log_chnl_cb2[0],
#endif
#if PHY_CHNL_NUM > 3
	&log_chnl_cb3[0],
#endif
	};

static u8				mb_chnnl_init_ok = 0;
static mb_ack_entry_t ack_queue[MB_ACK_QUEUE_LEN];
static volatile u8 ack_read;
static volatile u8 ack_write;
static volatile u8 ack_count;
static volatile u8 ack_sending;
static volatile u8 s_poll_active;
void mb_chnl_poll(void);

static void mb_chnl_notify(mb_log_chnl_cb_t *log_chnl_cb_x, u8 count,
			   mb_chnl_event_t event);

#if CONFIG_SOC_SMP
#include "spinlock.h"
static SPINLOCK_SECTION volatile spinlock_t mb_chnl_spin_lock = SPIN_LOCK_INIT;
#endif // CONFIG_SOC_SMP
static inline uint32_t mb_chnl_enter_critical()
{
	uint32_t flags = rtos_disable_int();

#if CONFIG_SOC_SMP
	spin_lock(&mb_chnl_spin_lock);
#endif // CONFIG_SOC_SMP

	return flags;
}

static inline void mb_chnl_exit_critical(uint32_t flags)
{
#if CONFIG_SOC_SMP
	spin_unlock(&mb_chnl_spin_lock);
#endif // CONFIG_SOC_SMP

	rtos_enable_int(flags);
}

/* =====================      physical channel functions      ==================*/

static volatile uint32_t s_mailbox_tx_cnt = 0;
static volatile uint32_t s_mailbox_rx_cnt = 0;
static volatile uint32_t s_mailbox_rx_ack_cnt = 0;
static volatile uint32_t s_mailbox_tx_ack_cnt = 0;
static volatile uint32_t s_mailbox_tx_compl_cnt = 0;
static volatile uint32_t s_mailbox_tx_ack_fault_mask = 0;

static inline bk_err_t bk_mailbox_send_safe(mailbox_data_t *data, mailbox_endpoint_t src, mailbox_endpoint_t dst, void *arg)
{
	bk_err_t		ret_code;

	u32 int_mask = mb_chnl_enter_critical();
	ret_code = bk_mailbox_send(data, src, dst, arg);
	mb_chnl_exit_critical(int_mask);

	return ret_code;
}

static bool mb_chnl_rx_ready(void)
{
	/* Keep one ACK slot available for the command currently being handled.
	 * A command must not be consumed and its business callback run if its
	 * transport ACK cannot be queued. */
	return ack_count < MB_ACK_QUEUE_LEN - 1;
}

static void mb_chnl_ack_retry(void)
{
	u16 chnl_type = MB_PHY_ACK_CHNL;

	while (1)
	{
		mb_ack_entry_t entry;
		bk_err_t ret;
		u32 flags = mb_chnl_enter_critical();

		if (!ack_count || ack_sending)
		{
			mb_chnl_exit_critical(flags);
			return;
		}
		entry = ack_queue[ack_read];
		ack_sending = 1;
		mb_chnl_exit_critical(flags);

		ret = bk_mailbox_send_safe(&entry.data, SELF_CPU,
			(mailbox_endpoint_t)entry.destination, &chnl_type);
		flags = mb_chnl_enter_critical();
		if (ret != BK_OK)
		{
			phy_chnl_x_cb[entry.destination].ack_retry_cnt++;
			ack_sending = 0;
			mb_chnl_exit_critical(flags);
			return;
		}
		ack_read = (ack_read + 1) % MB_ACK_QUEUE_LEN;
		ack_count--;
		ack_sending = 0;
		mb_chnl_exit_critical(flags);
		s_mailbox_rx_ack_cnt++;
		mb_chnl_notify((mb_log_chnl_cb_t *)phy_chnl_log_chnl_list[entry.destination],
			phy_chnl_log_chnl_num[entry.destination],
			MB_CHNL_EVENT_RX_ACK_SENT);
	}
}

static bool mb_chnl_ack_enqueue(u8 destination, const mailbox_data_t *data)
{
	u32 flags = mb_chnl_enter_critical();
	mb_ack_entry_t *entry;

	if (ack_count >= MB_ACK_QUEUE_LEN - 1)
	{
		phy_chnl_x_cb[destination].ack_queue_full_cnt++;
		mb_chnl_exit_critical(flags);
		return false;
	}
	entry = &ack_queue[ack_write];
	entry->data = *data;
	entry->destination = destination;
	ack_write = (ack_write + 1) % MB_ACK_QUEUE_LEN;
	ack_count++;
	mb_chnl_exit_critical(flags);
	mb_chnl_ack_retry();
	return true;
}

static void mb_chnl_notify(mb_log_chnl_cb_t *log_chnl_cb_x, u8 count,
			   mb_chnl_event_t event)
{
	u8 i;

	for (i = 0; i < count; i++)
	{
		if (log_chnl_cb_x[i].in_used && log_chnl_cb_x[i].event_isr)
			log_chnl_cb_x[i].event_isr(log_chnl_cb_x[i].isr_param, event);
	}
}

static void mb_chnl_fail_pending(mb_log_chnl_cb_t *logs, u8 count)
{
	mb_chnl_ack_t failure;
	u8 i;

	for (i = 0; i < count; i++)
	{
		if (logs[i].tx_state == CHNL_STATE_IDLE)
			continue;
		memset(&failure, 0, sizeof(failure));
		failure.hdr.cmd = ((mb_chnl_cmd_t *)&logs[i].chnnl_tx_buff)->hdr.cmd;
		failure.hdr.state = CHNL_STATE_COM_FAIL;
		logs[i].tx_state = CHNL_STATE_IDLE;
		if (logs[i].tx_cmpl_isr)
			logs[i].tx_cmpl_isr(logs[i].isr_param, &failure);
	}
}

static bk_err_t mb_phy_chnl_send_reset(u8 phy_idx, u8 logical_chnl)
{
	mb_phy_chnl_ack_t reset;
	u16 chnl_type = MB_PHY_ACK_CHNL;

	memset(&reset, 0, sizeof(reset));
	reset.hdr.ctrl = CHNL_CTRL_ACK_BOX | CHNL_CTRL_RESET;
	reset.hdr.logical_chnl = logical_chnl;
	return bk_mailbox_send_safe((mailbox_data_t *)&reset, SELF_CPU,
				    (mailbox_endpoint_t)phy_idx, &chnl_type);
}


static bk_err_t mb_phy_chnl_tx_cmd(u8 log_chnl)
{
	mb_phy_chnl_cmd_t	* cmd_ptr;
	bk_err_t		ret_code;
	u16 			chnl_type;
	u8				phy_chnl_idx = GET_DST_CPU_ID(log_chnl);   // = DST_CPU_ID;
	u8				log_chnl_idx = GET_LOG_CHNL_ID(log_chnl);

	mb_phy_chnl_cb_t * phy_chnl_ptr;
	mb_log_chnl_cb_t * log_chnl_cb_x;

#if !CONFIG_SOC_SMP
	/* for SMP project, log_chnl is always built by CPU0. it is always failed when call this API in CPU1. */
	if(SELF_CPU != GET_SRC_CPU_ID(log_chnl))
		return BK_ERR_PARAM;
#endif

	if(SELF_CPU == phy_chnl_idx)  // transferred to self.
		return BK_ERR_PARAM;

	if(phy_chnl_idx >= PHY_CHNL_NUM)
		return BK_ERR_PARAM;

	phy_chnl_ptr = &phy_chnl_x_cb[phy_chnl_idx];
	log_chnl_cb_x = (mb_log_chnl_cb_t *)(phy_chnl_log_chnl_list[phy_chnl_idx]);

	if(log_chnl_idx >= phy_chnl_log_chnl_num[phy_chnl_idx])
		return BK_ERR_PARAM;

	phy_chnl_ptr->tx_seq++;
	phy_chnl_ptr->tx_log_chnl = log_chnl;

	cmd_ptr = (mb_phy_chnl_cmd_t *)&log_chnl_cb_x[log_chnl_idx].chnnl_tx_buff;

	cmd_ptr->hdr.logical_chnl = log_chnl;
	cmd_ptr->hdr.tx_seq = phy_chnl_ptr->tx_seq;
	cmd_ptr->hdr.ctrl  = 0;
	cmd_ptr->hdr.state = 0;

	phy_chnl_ptr->tx_hdr_cmd = cmd_ptr->hdr.cmd;
	phy_chnl_ptr->last_cmd_header = cmd_ptr->hdr.data;

	phy_chnl_ptr->param1 = cmd_ptr->param1;
	phy_chnl_ptr->param2 = cmd_ptr->param2;
	phy_chnl_ptr->param3 = cmd_ptr->param3;

	__DMB();
	chnl_type = MB_PHY_CMD_CHNL;

	mailbox_endpoint_t    dst_cpu = (mailbox_endpoint_t)(phy_chnl_idx);

	ret_code = bk_mailbox_send(&log_chnl_cb_x[log_chnl_idx].chnnl_tx_buff, SELF_CPU, dst_cpu, (void *)&chnl_type);

	if(ret_code != BK_OK)
	{
		phy_chnl_ptr->tx_fault_cnt++;
		phy_chnl_ptr->tx_fault_cnt |= 0x40000000;

		return ret_code;
	}

	s_mailbox_tx_cnt++;

	log_chnl_cb_x[log_chnl_idx].tx_state = CHNL_STATE_IDLE;

	if(log_chnl_cb_x[log_chnl_idx].tx_isr != NULL)
	{
		log_chnl_cb_x[log_chnl_idx].tx_isr(log_chnl_cb_x[log_chnl_idx].isr_param);  	/* phy_chnl is BUSY now, tx_isr will not trigger phy_chnl_start_tx. */
	}

	return BK_OK;
}

void mb_chnl_start_service(void)
{
	/* Mailbox RX and ACK dispatch run in the hardware ISR path.  Retain this
	 * legacy entry point for callers, but do not start a second poll model. */
}

static void mb_phy_chnl_rx_ack_isr(mb_phy_chnl_ack_t *ack_ptr)
{
	u8		log_chnl;
	u8		ret_code;
	u8		phy_chnl_idx;
	u8		log_chnl_idx;

	mb_phy_chnl_cb_t * phy_chnl_ptr;
	mb_log_chnl_cb_t * log_chnl_cb_x;

	log_chnl = ack_ptr->hdr.logical_chnl;
	if (ack_ptr->hdr.ctrl & CHNL_CTRL_RESET)
	{
		mb_chnl_ack_t failure;

		phy_chnl_idx = GET_SRC_CPU_ID(log_chnl);
		if (ack_ptr->hdr.ctrl != (CHNL_CTRL_ACK_BOX | CHNL_CTRL_RESET) ||
			ack_ptr->hdr.state != 0 || ack_ptr->hdr.cmd != 0 ||
			ack_ptr->hdr.tx_seq != 0 || ack_ptr->data1 != 0 ||
			ack_ptr->data2 != 0 || ack_ptr->data3 != 0 ||
			GET_DST_CPU_ID(log_chnl) != SELF_CPU ||
			phy_chnl_idx == SELF_CPU || phy_chnl_idx >= PHY_CHNL_NUM)
		{
			s_mailbox_tx_ack_fault_mask |= 0x10;
			if (phy_chnl_idx < PHY_CHNL_NUM)
				phy_chnl_x_cb[phy_chnl_idx].reset_bad_cnt++;
			return;
		}

		phy_chnl_ptr = &phy_chnl_x_cb[phy_chnl_idx];
		phy_chnl_ptr->recovering = 1;
		log_chnl_cb_x = (mb_log_chnl_cb_t *)phy_chnl_log_chnl_list[phy_chnl_idx];
		mb_chnl_notify(log_chnl_cb_x,
				   phy_chnl_log_chnl_num[phy_chnl_idx],
				   MB_CHNL_EVENT_PEER_RESET);
		u32 reset_flags = mb_chnl_enter_critical();
		if (phy_chnl_ptr->tx_state == CHNL_STATE_BUSY)
		{
			phy_chnl_ptr->tx_state = CHNL_STATE_IDLE;
			phy_chnl_ptr->busy_since_ms = 0;
			mb_chnl_exit_critical(reset_flags);
			log_chnl_idx = GET_LOG_CHNL_ID(phy_chnl_ptr->tx_log_chnl);
			if (log_chnl_idx < phy_chnl_log_chnl_num[phy_chnl_idx] &&
				log_chnl_cb_x[log_chnl_idx].tx_cmpl_isr)
			{
				memset(&failure, 0, sizeof(failure));
				failure.hdr.cmd = phy_chnl_ptr->tx_hdr_cmd;
				failure.hdr.state = CHNL_STATE_COM_FAIL;
				log_chnl_cb_x[log_chnl_idx].tx_cmpl_isr(
					log_chnl_cb_x[log_chnl_idx].isr_param, &failure);
			}
			bk_mailbox_tx_timeout((mailbox_endpoint_t)phy_chnl_idx);
		}
		else
			mb_chnl_exit_critical(reset_flags);
		mb_chnl_fail_pending(log_chnl_cb_x,
				     phy_chnl_log_chnl_num[phy_chnl_idx]);
		return;
	}

	phy_chnl_idx = GET_DST_CPU_ID(log_chnl);   // = DST_CPU_ID;
	log_chnl_idx = GET_LOG_CHNL_ID(log_chnl);

	s_mailbox_tx_ack_cnt++;

	if(SELF_CPU != GET_SRC_CPU_ID(log_chnl)) {
		s_mailbox_tx_ack_fault_mask |= 0x1;
		return;
	}


	if(SELF_CPU == phy_chnl_idx)  // received from self.
	{
		s_mailbox_tx_ack_fault_mask |= 0x2;
		return;
	}


	if(phy_chnl_idx >= PHY_CHNL_NUM){
		s_mailbox_tx_ack_fault_mask |= 0x4;
		return;
	}

	phy_chnl_ptr = &phy_chnl_x_cb[phy_chnl_idx];
	phy_chnl_ptr->last_ack_header = ack_ptr->hdr.data;
	phy_chnl_ptr->last_ack_data1 = ack_ptr->data1;
	log_chnl_cb_x = (mb_log_chnl_cb_t *)(phy_chnl_log_chnl_list[phy_chnl_idx]);

	if(log_chnl_idx >= phy_chnl_log_chnl_num[phy_chnl_idx])
	{
		s_mailbox_tx_ack_fault_mask |= 0x8;
		return;
	}

	u32 claim_flags = mb_chnl_enter_critical();
	if (phy_chnl_ptr->tx_state != CHNL_STATE_BUSY ||
		(ack_ptr->hdr.ctrl != CHNL_CTRL_ACK_BOX) ||
		 (ack_ptr->hdr.state & ~CHNL_STATE_COM_FAIL) ||
		 (log_chnl != phy_chnl_ptr->tx_log_chnl) ||
		 (ack_ptr->hdr.tx_seq != phy_chnl_ptr->tx_seq) ||
		 (ack_ptr->hdr.cmd != phy_chnl_ptr->tx_hdr_cmd))
	{
		phy_chnl_ptr->rx_fault_cnt++;
		phy_chnl_ptr->rx_fault_cnt |= 0x40000000;
		mb_chnl_exit_critical(claim_flags);
		return;
	}
	phy_chnl_ptr->tx_state = CHNL_STATE_IDLE;
	phy_chnl_ptr->busy_since_ms = 0;
	mb_chnl_exit_critical(claim_flags);

	bk_mailbox_tx_complete((mailbox_endpoint_t)phy_chnl_idx);

	if(log_chnl_cb_x[log_chnl_idx].tx_cmpl_isr != NULL)
	{
		/* clear following header members. */
		ack_ptr->hdr.logical_chnl = 0;
		ack_ptr->hdr.tx_seq       = 0;
		ack_ptr->hdr.ctrl         = 0;

		/* hdr.state, hdr.cmd these 2 members keep untouched. */
		s_mailbox_tx_compl_cnt++;
		log_chnl_cb_x[log_chnl_idx].tx_cmpl_isr(log_chnl_cb_x[log_chnl_idx].isr_param, (mb_chnl_ack_t *)ack_ptr);
	} else {
		s_mailbox_tx_ack_fault_mask |= 0x100;
	}

	u32 int_mask = mb_chnl_enter_critical();
	if (phy_chnl_ptr->tx_state != CHNL_STATE_IDLE)
	{
		mb_chnl_exit_critical(int_mask);
		return;
	}
	for(log_chnl_idx = 0; log_chnl_idx < phy_chnl_log_chnl_num[phy_chnl_idx]; log_chnl_idx++)  /* priority descended search. */
	{
		if(log_chnl_cb_x[log_chnl_idx].tx_state != CHNL_STATE_IDLE)
			break;
	}

	if(log_chnl_idx >= phy_chnl_log_chnl_num[phy_chnl_idx])
	{
		mb_chnl_exit_critical(int_mask);
		return;
	}

	log_chnl = CPX_LOG_CHNL_START(SELF_CPU, phy_chnl_idx) + log_chnl_idx;

	/* About to dispatch the next pending tx on this physical channel:
	 * refresh the busy-watchdog timestamp so its lifetime starts from
	 * here rather than carrying over from the previous transaction. */
	phy_chnl_ptr->tx_state = CHNL_STATE_BUSY;
	phy_chnl_ptr->busy_since_ms = rtos_get_time();

	ret_code = mb_phy_chnl_tx_cmd(log_chnl);

	if(ret_code != BK_OK)
	{
		log_chnl_cb_x[log_chnl_idx].tx_state = CHNL_STATE_IDLE;
		phy_chnl_ptr->tx_state               = CHNL_STATE_IDLE;
		phy_chnl_ptr->busy_since_ms          = 0;
	}
	mb_chnl_exit_critical(int_mask);

	return;

}

static void mb_phy_chnl_rx_cmd_isr(mb_phy_chnl_cmd_t *cmd_ptr)
{
	phy_chnnl_hdr_t  chnl_hdr;
	u8			log_chnl = cmd_ptr->hdr.logical_chnl;

	u8		phy_chnl_idx;
	u8		log_chnl_idx;

	mb_phy_chnl_cb_t * phy_chnl_ptr;
	mb_log_chnl_cb_t * log_chnl_cb_x;

	phy_chnl_idx = GET_SRC_CPU_ID(log_chnl);   // = SRC_CPU_ID; from SRC_CPU.
	log_chnl_idx = GET_LOG_CHNL_ID(log_chnl);

	s_mailbox_rx_cnt++;

	if(SELF_CPU != GET_DST_CPU_ID(log_chnl))
		return;

	if(SELF_CPU == phy_chnl_idx)  // received from self.
		return;

	if(phy_chnl_idx >= PHY_CHNL_NUM)
		return;

	phy_chnl_ptr = &phy_chnl_x_cb[phy_chnl_idx];
	log_chnl_cb_x = (mb_log_chnl_cb_t *)(phy_chnl_log_chnl_list[phy_chnl_idx]);

	chnl_hdr.data = cmd_ptr->hdr.data;

	if(log_chnl_idx >= phy_chnl_log_chnl_num[phy_chnl_idx])
	{
		phy_chnl_ptr->rx_fault_cnt++;
		phy_chnl_ptr->rx_fault_cnt |= 0x80000000;

		return;
	}

	if(log_chnl_cb_x[log_chnl_idx].rx_status_isr != NULL)
	{
		/* clear all other hdr members except hdr.cmd. */
		cmd_ptr->hdr.logical_chnl = 0;
		cmd_ptr->hdr.tx_seq       = 0;
		cmd_ptr->hdr.ctrl         = 0;
		cmd_ptr->hdr.state        = 0;

		if (log_chnl_cb_x[log_chnl_idx].rx_status_isr(
			log_chnl_cb_x[log_chnl_idx].isr_param,
			(mb_chnl_cmd_t *)cmd_ptr) != BK_OK)
			chnl_hdr.state |= CHNL_STATE_COM_FAIL;
	}
	else if(log_chnl_cb_x[log_chnl_idx].rx_isr != NULL)
	{
		/* clear all other hdr members except hdr.cmd. */
		cmd_ptr->hdr.logical_chnl = 0;
		cmd_ptr->hdr.tx_seq       = 0;
		cmd_ptr->hdr.ctrl         = 0;
		cmd_ptr->hdr.state        = 0;

		log_chnl_cb_x[log_chnl_idx].rx_isr(log_chnl_cb_x[log_chnl_idx].isr_param, (mb_chnl_cmd_t *)cmd_ptr);

		/* !!!! cmd_ptr buffer now contains ACK data !!!!. */
	}
	else
	{
		memset(cmd_ptr, 0, sizeof(*cmd_ptr));
		cmd_ptr->hdr.data = chnl_hdr.data;
		chnl_hdr.state |= CHNL_STATE_COM_FAIL;		/* cmd NO target app, it is an ACK bit to peer CPU. */
	}

	if(chnl_hdr.ctrl & CHNL_CTRL_SYNC_TX)
	{
		/* sync tx cmd, do NOT send ACK. */
		return;
	}

	/* RE-USE the copied command buffer to construct the stable ACK. */
	cmd_ptr->hdr.data  = chnl_hdr.data;
	cmd_ptr->hdr.ctrl |= CHNL_CTRL_ACK_BOX;			/* ACK msg, use the ACK channel.  */

	if (!mb_chnl_ack_enqueue(phy_chnl_idx, (mailbox_data_t *)cmd_ptr))
	{
		phy_chnl_ptr->tx_fault_cnt++;
		phy_chnl_ptr->tx_fault_cnt |= 0x80000000;
		return;
	}
	return;
}

static void mb_phy_chnl_rx_isr(mailbox_data_t * mb_data)
{
	mb_phy_chnl_union_t	rx_data;

	rx_data.mb_data.param0 = mb_data->param0;
	rx_data.mb_data.param1 = mb_data->param1;
	rx_data.mb_data.param2 = mb_data->param2;
	rx_data.mb_data.param3 = mb_data->param3;
	/* the following process will damage the input parameter,
	so pass in the pointer of copied parameter instad of the original. */

	/*
	 *  there are 2 boxes in one MAILBOXn HW,
	 *  but no way to know which box this msg is from in current mailbox_driver design.
	 *  so use the CHNL_CTRL_ACK_BOX bit in the msg hdr.ctrl to distinguish where it is from.
	 *  when CHNL_CTRL_ACK_BOX is set, it means from ack box ( MAILBOXn_BOX1 ).
	 */
	if(rx_data.phy_ch_hdr.ctrl & CHNL_CTRL_ACK_BOX)		/* rx ack. */
	{
		mb_phy_chnl_rx_ack_isr(&rx_data.phy_ch_ack);
	}
	else		/* rx cmd. */
	{
		mb_phy_chnl_rx_cmd_isr(&rx_data.phy_ch_cmd);
	}
}

static bk_err_t mb_phy_chnl_start_tx(u8 log_chnl)
{
	bk_err_t	ret_code;

	u8		phy_chnl_idx;

	mb_phy_chnl_cb_t * phy_chnl_ptr;

	phy_chnl_idx = GET_DST_CPU_ID(log_chnl);   // = DST_CPU_ID;

	if(SELF_CPU == phy_chnl_idx)  // transferred to self.
		return BK_ERR_PARAM;

	if(phy_chnl_idx >= PHY_CHNL_NUM)
		return BK_ERR_PARAM;

	phy_chnl_ptr = &phy_chnl_x_cb[phy_chnl_idx];

	if(phy_chnl_ptr->tx_state == CHNL_STATE_IDLE)
	{
		phy_chnl_ptr->tx_state = CHNL_STATE_BUSY;		/* MUST set channel state to BUSY firstly. */
		phy_chnl_ptr->busy_since_ms = rtos_get_time();
		/* start_tx->tx_cmd->tx_isr callback->mb_chnl_write->start_tx, it is a loop.
		   break the loop by setting the phy_chnl_cb.tx_state to busy. */

		ret_code = mb_phy_chnl_tx_cmd(log_chnl);

		if(ret_code != BK_OK)
		{
			/* tx_cmd failed before the data hit hardware: roll back BOTH
			 * physical and logical state, so the caller can retry and so
			 * the logical channel does not stay BUSY forever. */
			u8                   log_chnl_idx_fail = GET_LOG_CHNL_ID(log_chnl);
			mb_log_chnl_cb_t    *log_chnl_cb_x_fail =
				(mb_log_chnl_cb_t *)(phy_chnl_log_chnl_list[phy_chnl_idx]);

			if (log_chnl_idx_fail < phy_chnl_log_chnl_num[phy_chnl_idx]) {
				log_chnl_cb_x_fail[log_chnl_idx_fail].tx_state = CHNL_STATE_IDLE;
			}
			phy_chnl_ptr->tx_state = CHNL_STATE_IDLE;
			phy_chnl_ptr->busy_since_ms = 0;
			return ret_code;
		}
	}

	return BK_OK;
}

volatile uint32_t s_mailbox_sync_cnt = 0;

static bk_err_t mb_phy_chnl_tx_cmd_sync(u8 log_chnl, mb_phy_chnl_cmd_t *cmd_ptr)
{
	bk_err_t		ret_code;
	u16 			chnl_type;

	u8		phy_chnl_idx;

	phy_chnl_idx = GET_DST_CPU_ID(log_chnl);   // = DST_CPU_ID;

#if !CONFIG_SOC_SMP
	if(SELF_CPU != GET_SRC_CPU_ID(log_chnl))
		return BK_ERR_PARAM;
#endif

	if(SELF_CPU == phy_chnl_idx)  // transferred to self.
		return BK_ERR_PARAM;

	if(phy_chnl_idx >= PHY_CHNL_NUM)
		return BK_ERR_PARAM;

	mailbox_endpoint_t    dst_cpu = (mailbox_endpoint_t)(phy_chnl_idx);

	cmd_ptr->hdr.logical_chnl = log_chnl;
	cmd_ptr->hdr.tx_seq = 0;
	cmd_ptr->hdr.ctrl  = CHNL_CTRL_SYNC_TX;
	cmd_ptr->hdr.state = 0;

	chnl_type = MB_PHY_CMD_CHNL;

	s_mailbox_sync_cnt++;

	/*
	 * can't wait 'phy_chnl_cb.tx_state' to be CHNL_STATE_IDLE here,
	 * 'phy_chnl_cb.tx_state' is set to CHNL_STATE_IDLE in interrupt callback.
	 * but the interrupt may be disabled when this API is called.
	 *    wait physical channel HW to be IDLE by <POLLing> !!
	 */
	while(1)
	{
		ret_code = bk_mailbox_send_safe((mailbox_data_t *)cmd_ptr, SELF_CPU, dst_cpu, (void *)&chnl_type);

		if(ret_code != BK_ERR_MAILBOX_TIMEOUT)
		{
			break;
		}
	}

	return ret_code;
}


/* =====================      logical channel APIs      ==================*/
/*
  * init logical chnanel module.
  * return:
  *     succeed: BK_OK;
  *     failed  : fail code.
  *
  */
bk_err_t mb_chnl_init(void)
{
	bk_err_t		ret_code;
	int				i, j;

	mb_log_chnl_cb_t * log_chnl_cb_x;

	/*
	 * mb_chnl_init() is reached from mb_chnl_open(), which is called by
	 * many subsystems (shell log forwarding, ipc_init, mb_uart, ...).
	 * Each one of those callers runs on its own FreeRTOS task and they
	 * can race here:
	 *
	 *   Task A: enters mb_chnl_open(MB_CHNL_LOG), sees mb_chnnl_init_ok==0,
	 *           enters mb_chnl_init(), runs all the memset's, sets
	 *           mb_chnnl_init_ok=1, returns. Caller then sets
	 *           log_chnl_cb2[15].in_used=1 and installs rx_isr.
	 *   Task B (preempted in the middle of mb_chnl_open before the
	 *           init_ok check finishes): comes back, sees init_ok still
	 *           == 0 (because it sampled before A set it), enters
	 *           mb_chnl_init() AGAIN, and the memset wipes the
	 *           log_chnl_cb_x[] array that A just populated.
	 *
	 * Fix: serialize the body with the existing critical section so the
	 * "if (init_ok) return; ... init_ok = 1;" sequence is atomic w.r.t.
	 * other tasks. The double-check pattern still keeps the fast path
	 * lock-free once init has completed.
	 */
	if(mb_chnnl_init_ok)
	{
		return BK_OK;
	}

	uint32_t init_int_mask = mb_chnl_enter_critical();
	if(mb_chnnl_init_ok)
	{
		mb_chnl_exit_critical(init_int_mask);
		return BK_OK;
	}

	memset(&phy_chnl_x_cb, 0, sizeof(phy_chnl_x_cb));
	memset(ack_queue, 0, sizeof(ack_queue));
	ack_read = 0;
	ack_write = 0;
	ack_count = 0;
	ack_sending = 0;
	for(i = 0; i < PHY_CHNL_NUM; i++)
	{
		phy_chnl_x_cb[i].tx_state = CHNL_STATE_IDLE;

		log_chnl_cb_x = (mb_log_chnl_cb_t *)(phy_chnl_log_chnl_list[i]);

		memset(log_chnl_cb_x, 0, sizeof(mb_log_chnl_cb_t) * phy_chnl_log_chnl_num[i]);

		for(j = 0; j < phy_chnl_log_chnl_num[i]; j++)
		{
			log_chnl_cb_x[j].tx_state = CHNL_STATE_IDLE;
		}
	}

	ret_code = bk_mailbox_init();
	if(ret_code != BK_OK)
	{
		mb_chnl_exit_critical(init_int_mask);
		return ret_code;
	}

	mailbox_endpoint_t    dst_cpu;

	for(i = 0; i < PHY_CHNL_NUM; i++)
	{
		dst_cpu = (mailbox_endpoint_t)(i);  // dst_cpu_id;

		bk_mailbox_recv_callback_register(dst_cpu, SELF_CPU, mb_phy_chnl_rx_isr);
	}

	mb_chnnl_init_ok = 1;
	bk_mailbox_rx_ready_callback_register(mb_chnl_rx_ready);

	mb_chnl_exit_critical(init_int_mask);

	return BK_OK;
}

/*
  * open logical chnanel.
  * input:
  *     log_chnl  : logical channel id to open.
  *     callback_param : param passsed to all callbacks.
  * return:
  *     succeed: BK_OK;
  *     failed  : fail code.
  *
  */
bk_err_t mb_chnl_open(u8 log_chnl, void * callback_param)
{
	if(!mb_chnnl_init_ok)	/* if driver is not initialized. */
	{
		bk_err_t		ret_code;

		ret_code = mb_chnl_init();

		if(ret_code != BK_OK)
		{
			return ret_code;
		}
	}

	u8		phy_chnl_idx = GET_DST_CPU_ID(log_chnl);   // = DST_CPU_ID;
	u8		log_chnl_idx = GET_LOG_CHNL_ID(log_chnl);

	mb_log_chnl_cb_t * log_chnl_cb_x;

#if !CONFIG_SOC_SMP
	if(SELF_CPU != GET_SRC_CPU_ID(log_chnl))
		return BK_ERR_PARAM;
#endif

	if(SELF_CPU == phy_chnl_idx)  // transferred to self.
		return BK_ERR_PARAM;

	if(phy_chnl_idx >= PHY_CHNL_NUM)
		return BK_ERR_PARAM;

	log_chnl_cb_x = (mb_log_chnl_cb_t *)(phy_chnl_log_chnl_list[phy_chnl_idx]);

	if(log_chnl_idx >= phy_chnl_log_chnl_num[phy_chnl_idx])
		return BK_ERR_PARAM;

	if(log_chnl_cb_x[log_chnl_idx].in_used)
		return BK_ERR_OPEN;

	log_chnl_cb_x[log_chnl_idx].in_used = 1;		/* chnl in used. */
	log_chnl_cb_x[log_chnl_idx].isr_param = callback_param;

	return BK_OK;
}

/*
  * close logical chnanel.
  * input:
  *     log_chnl  : logical channel id to close.
  * return:
  *     succeed: BK_OK;
  *     failed  : fail code.
  *
  */
bk_err_t mb_chnl_close(u8 log_chnl)
{
	u8		phy_chnl_idx = GET_DST_CPU_ID(log_chnl);   // = DST_CPU_ID;
	u8		log_chnl_idx = GET_LOG_CHNL_ID(log_chnl);

	mb_log_chnl_cb_t * log_chnl_cb_x;

#if !CONFIG_SOC_SMP
	if(SELF_CPU != GET_SRC_CPU_ID(log_chnl))
		return BK_ERR_PARAM;
#endif

	if(phy_chnl_idx >= PHY_CHNL_NUM)
		return BK_ERR_PARAM;

	log_chnl_cb_x = (mb_log_chnl_cb_t *)(phy_chnl_log_chnl_list[phy_chnl_idx]);

	if(log_chnl_idx >= phy_chnl_log_chnl_num[phy_chnl_idx])
		return BK_ERR_PARAM;

	if(log_chnl_cb_x[log_chnl_idx].in_used == 0)
		return BK_ERR_STATE;

	log_chnl_cb_x[log_chnl_idx].in_used = 0;
	log_chnl_cb_x[log_chnl_idx].tx_state = CHNL_STATE_IDLE;
	log_chnl_cb_x[log_chnl_idx].rx_isr = NULL;
	log_chnl_cb_x[log_chnl_idx].rx_status_isr = NULL;
	log_chnl_cb_x[log_chnl_idx].tx_isr = NULL;
	log_chnl_cb_x[log_chnl_idx].tx_cmpl_isr = NULL;
	log_chnl_cb_x[log_chnl_idx].event_isr = NULL;

	return BK_OK;
}

/*
  * read from logical chnanel.
  * input:
  *     log_chnl     : logical channel id to read.
  *     read_buf       : buffer to receive channel cmd data.
  * return:
  *     succeed: BK_OK;
  *     failed  : fail code.
  *
  */
bk_err_t mb_chnl_read(u8 log_chnl, mb_chnl_cmd_t * read_buf)
{
	return BK_ERR_NOT_SUPPORT;
}

/*
  * write to logical chnanel.
  * input:
  *     log_chnl     : logical channel id to write.
  *     cmd_buf     : buffer of channel cmd data.
  * return:
  *     succeed: BK_OK;
  *     failed  : fail code.
  *
  */
bk_err_t mb_chnl_write(u8 log_chnl, mb_chnl_cmd_t * cmd_buf)
{
	u16		write_len;

	u8		phy_chnl_idx = GET_DST_CPU_ID(log_chnl);   // = DST_CPU_ID;
	u8		log_chnl_idx = GET_LOG_CHNL_ID(log_chnl);

	mb_log_chnl_cb_t * log_chnl_cb_x;

#if !CONFIG_SOC_SMP
	if(SELF_CPU != GET_SRC_CPU_ID(log_chnl))
		return BK_ERR_PARAM;
#endif

	if(SELF_CPU == phy_chnl_idx)  // transferred to self.
		return BK_ERR_PARAM;

	if(phy_chnl_idx >= PHY_CHNL_NUM)
		return BK_ERR_PARAM;

	log_chnl_cb_x = (mb_log_chnl_cb_t *)(phy_chnl_log_chnl_list[phy_chnl_idx]);

	if(log_chnl_idx >= phy_chnl_log_chnl_num[phy_chnl_idx])
		return BK_ERR_PARAM;

	if(log_chnl_cb_x[log_chnl_idx].in_used == 0)
		return BK_ERR_STATE;

	u32 int_mask = mb_chnl_enter_critical();
	mb_phy_chnl_cb_t *phy = &phy_chnl_x_cb[phy_chnl_idx];
	mb_chnl_cmd_t *logical_cmd = cmd_buf;

	if (phy->recovering &&
		!(log_chnl == MB_CHNL_UART0 &&
		  logical_cmd->hdr.cmd == MB_UART_STATE_COMMAND))
	{
		mb_chnl_exit_critical(int_mask);
		return BK_ERR_BUSY;
	}

	if(log_chnl_cb_x[log_chnl_idx].tx_state != CHNL_STATE_IDLE)
	{
		mb_chnl_exit_critical(int_mask);

		return BK_ERR_BUSY;
	}

	write_len = sizeof(mailbox_data_t) > sizeof(mb_chnl_cmd_t) ? sizeof(mb_chnl_cmd_t) : sizeof(mailbox_data_t);

	memset(&log_chnl_cb_x[log_chnl_idx].chnnl_tx_buff, 0,
	       sizeof(log_chnl_cb_x[log_chnl_idx].chnnl_tx_buff));
	memcpy(&log_chnl_cb_x[log_chnl_idx].chnnl_tx_buff, cmd_buf, write_len);

	/* set to BUSY means there is data in tx-buff. mb_phy_chnl_rx_ack_isr will get it to send. */
	log_chnl_cb_x[log_chnl_idx].tx_state = CHNL_STATE_BUSY;   /* MUST set to BUSY after data was copied. */
	__DMB();

	bk_err_t start_ret = mb_phy_chnl_start_tx(log_chnl);

	mb_chnl_exit_critical(int_mask);

	return start_ret;
}

/*
  * logical chnanel misc io (set/get param).
  * input:
  *     log_chnl     : logical channel id to set/get param.
  *     cmd          : control command for logical channel.
  *     param      :  parameter of the command.
  * return:
  *     succeed: BK_OK;
  *     failed  : fail code.
  *
  */
bk_err_t mb_chnl_ctrl(u8 log_chnl, u8 cmd, void * param)
{
	bk_err_t	ret_code;

	u8		phy_chnl_idx = GET_DST_CPU_ID(log_chnl);   // = DST_CPU_ID;
	u8		log_chnl_idx = GET_LOG_CHNL_ID(log_chnl);

	mb_log_chnl_cb_t * log_chnl_cb_x;

#if !CONFIG_SOC_SMP
	if(SELF_CPU != GET_SRC_CPU_ID(log_chnl))
		return BK_ERR_PARAM;
#endif

	if(SELF_CPU == phy_chnl_idx)  // transferred to self.
		return BK_ERR_PARAM;

	if(phy_chnl_idx >= PHY_CHNL_NUM)
		return BK_ERR_PARAM;

	log_chnl_cb_x = (mb_log_chnl_cb_t *)(phy_chnl_log_chnl_list[phy_chnl_idx]);

	if(log_chnl_idx >= phy_chnl_log_chnl_num[phy_chnl_idx])
		return BK_ERR_PARAM;

	if(log_chnl_cb_x[log_chnl_idx].in_used == 0)
		return BK_ERR_STATE;

	switch(cmd)
	{
		case MB_CHNL_GET_STATUS:

			if(param == NULL)
				return BK_ERR_NULL_PARAM;

			*((u8 *)param) = log_chnl_cb_x[log_chnl_idx].tx_state;

			break;

		case MB_CHNL_SET_RX_ISR:
			log_chnl_cb_x[log_chnl_idx].rx_isr = (chnl_rx_isr_t)param;
			break;
		case MB_CHNL_SET_RX_STATUS_ISR:
			log_chnl_cb_x[log_chnl_idx].rx_status_isr =
				(chnl_rx_status_isr_t)param;
			break;

		case MB_CHNL_SET_TX_ISR:
			log_chnl_cb_x[log_chnl_idx].tx_isr = (chnl_tx_isr_t)param;
			break;

		case MB_CHNL_SET_TX_CMPL_ISR:
			log_chnl_cb_x[log_chnl_idx].tx_cmpl_isr = (chnl_tx_cmpl_isr_t)param;
			break;
		case MB_CHNL_SET_EVENT_ISR:
			log_chnl_cb_x[log_chnl_idx].event_isr = (chnl_event_isr_t)param;
			break;

		case MB_CHNL_WRITE_SYNC:
			if(param == NULL)
				return BK_ERR_NULL_PARAM;

			ret_code = mb_phy_chnl_tx_cmd_sync(log_chnl, (mb_phy_chnl_cmd_t *)param);
			return ret_code;
			break;

		default:
			return BK_ERR_NOT_SUPPORT;
			break;
	}

	return BK_OK;
}

void mb_chnl_poll(void)
{
	u8 phy_idx;
	u32 poll_flags = mb_chnl_enter_critical();

	/* The CPU1 boot transaction may poll directly while ap_console is also
	 * scheduled.  Serialize the whole poll pass without holding the channel
	 * lock across callbacks and physical sends.
	 */
	if (s_poll_active)
	{
		mb_chnl_exit_critical(poll_flags);
		return;
	}
	s_poll_active = 1;
	mb_chnl_exit_critical(poll_flags);

	bk_mailbox_poll();
	mb_chnl_ack_retry();

	for (phy_idx = 0; phy_idx < PHY_CHNL_NUM; phy_idx++)
	{
		mb_phy_chnl_cb_t *phy = &phy_chnl_x_cb[phy_idx];
		mb_log_chnl_cb_t *logs;
		mb_chnl_ack_t failure;
		u8 log_idx;

		u32 claim_flags = mb_chnl_enter_critical();
		if ((phy->tx_state != CHNL_STATE_BUSY) || !phy->busy_since_ms ||
			((u32)(rtos_get_time() - phy->busy_since_ms) < MB_PHY_CHNL_BUSY_TIMEOUT_MS))
		{
			mb_chnl_exit_critical(claim_flags);
			continue;
		}
		phy->tx_state = CHNL_STATE_IDLE;
		phy->busy_since_ms = 0;
		mb_chnl_exit_critical(claim_flags);

		logs = (mb_log_chnl_cb_t *)phy_chnl_log_chnl_list[phy_idx];
		log_idx = GET_LOG_CHNL_ID(phy->tx_log_chnl);
		if (log_idx >= phy_chnl_log_chnl_num[phy_idx])
			continue;

		phy->tx_fault_cnt++;
		phy->tx_fault_cnt |= 0x20000000;
		bk_mailbox_tx_timeout((mailbox_endpoint_t)phy_idx);
		phy->recovering = 1;
		memset(&failure, 0, sizeof(failure));
		failure.hdr.cmd = phy->tx_hdr_cmd;
		failure.hdr.state = CHNL_STATE_COM_FAIL;
		if (logs[log_idx].tx_cmpl_isr)
			logs[log_idx].tx_cmpl_isr(logs[log_idx].isr_param, &failure);
		mb_chnl_fail_pending(logs, phy_chnl_log_chnl_num[phy_idx]);
		mb_chnl_notify(logs, phy_chnl_log_chnl_num[phy_idx],
				   MB_CHNL_EVENT_TX_TIMEOUT);
		if (mb_phy_chnl_send_reset(phy_idx, phy->tx_log_chnl) == BK_OK)
			mb_chnl_notify(logs, phy_chnl_log_chnl_num[phy_idx],
					   MB_CHNL_EVENT_ABORT_SENT);
		else
			mb_chnl_notify(logs, phy_chnl_log_chnl_num[phy_idx],
					   MB_CHNL_EVENT_ABORT_FAILED);
	}

	poll_flags = mb_chnl_enter_critical();
	s_poll_active = 0;
	mb_chnl_exit_critical(poll_flags);
}

bk_err_t mb_chnl_get_diag(u8 peer_cpu, mb_chnl_diag_t *diag)
{
	mb_phy_chnl_cb_t *phy;

	if (!diag)
		return BK_ERR_NULL_PARAM;
	if (peer_cpu >= PHY_CHNL_NUM || peer_cpu == SELF_CPU)
		return BK_ERR_PARAM;

	phy = &phy_chnl_x_cb[peer_cpu];
	diag->busy = phy->tx_state == CHNL_STATE_BUSY;
	diag->logical_chnl = phy->tx_log_chnl;
	diag->sequence = phy->tx_seq;
	diag->command = phy->tx_hdr_cmd;
	diag->stale_ack = phy->rx_fault_cnt;
	diag->tx_fault = phy->tx_fault_cnt;
	diag->ack_retry = phy->ack_retry_cnt;
	diag->ack_queue_full = phy->ack_queue_full_cnt;
	diag->reset_bad = phy->reset_bad_cnt;
	diag->last_cmd_header = phy->last_cmd_header;
	diag->last_ack_header = phy->last_ack_header;
	diag->last_ack_data1 = phy->last_ack_data1;
	return BK_OK;
}

void mb_chnl_recovered(u8 peer_cpu)
{
	if (peer_cpu < PHY_CHNL_NUM && peer_cpu != SELF_CPU)
		phy_chnl_x_cb[peer_cpu].recovering = 0;
}

void mb_chnl_quiesce(u8 peer_cpu)
{
	if (peer_cpu < PHY_CHNL_NUM && peer_cpu != SELF_CPU)
		phy_chnl_x_cb[peer_cpu].recovering = 1;
}

void mb_debug_status() {

}
