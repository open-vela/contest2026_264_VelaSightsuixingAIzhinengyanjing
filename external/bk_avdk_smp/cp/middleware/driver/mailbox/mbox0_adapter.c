#include <string.h>
#include <stdint.h>
#include "cmsis_gcc.h"
#include "armstar.h"
#include "mbox0_adapter.h"

#define MBOX0_CTRL_ACK_BOX       0x01
#define MBOX0_CTRL_SYNC_TX       0x02
#define MBOX0_CTRL_RESET         0x04
#define MBOX0_CMD_SLOT_NUM       3
#define MBOX0_ACK_SLOT_NUM       8
#define MBOX0_RX_DESC_NUM        7
#define MBOX0_SLOT_QUARANTINE_MS 20
#define MBOX0_AP_RAM_START       0x28010000u
#define MBOX0_AP_RAM_END         0x28064000u

typedef struct __attribute__((aligned(32))) {
	mailbox_data_t data;
	u8 padding[32 - sizeof(mailbox_data_t)];
} mbox0_stable_slot_t;

typedef struct {
	mbox0_stable_slot_t cmd[MBOX0_CMD_SLOT_NUM];
	mbox0_stable_slot_t ack[MBOX0_ACK_SLOT_NUM];
	mbox0_stable_slot_t reset;
	mbox0_stable_slot_t sync;
	u8 cmd_used[MBOX0_CMD_SLOT_NUM];
	u8 cmd_active;
	u8 ack_used[MBOX0_ACK_SLOT_NUM];
	u8 reset_used;
	u8 sync_used;
	u32 publish_order;
	u32 cmd_order[MBOX0_CMD_SLOT_NUM];
	u32 ack_order[MBOX0_ACK_SLOT_NUM];
	u32 reset_order;
	u32 sync_order;
	u32 fifo_empty_since_ms;
} mbox0_tx_slots_t;

typedef struct {
	mailbox_data_t data;
	u8 source;
	u8 used;
	u32 order;
} mbox0_rx_desc_t;

typedef char mbox0_slot_size_must_be_32[(sizeof(mbox0_stable_slot_t) == 32) ? 1 : -1];

static mbox0_tx_slots_t mailbox_slots[MBOX_CHNL_NUM];
static mailbox_callback_t mailbox_callback[MBOX_CHNL_NUM];
static mbox0_rx_desc_t rx_desc[MBOX0_RX_DESC_NUM];
static volatile u8 rx_count;
static u32 rx_order;
static mbox0_adapter_diag_t adapter_diag;
static mailbox_rx_ready_t upper_rx_ready;

static void mbox0_reap_slots(u8 dst)
{
	mbox0_tx_slots_t *slots;
	uint32_t fifo_count;
	u32 reaped = 0;
	u32 now;
	u8 i;

	if (mbox0_drv_get_send_count(dst, &fifo_count) != MBOX0_HAL_OK)
		return;

	slots = &mailbox_slots[dst];
	if (fifo_count != 0)
	{
		slots->fifo_empty_since_ms = 0;
		return;
	}

	now = rtos_get_time();
	if (!slots->fifo_empty_since_ms)
	{
		slots->fifo_empty_since_ms = now ? now : 1;
		return;
	}
	if ((u32)(now - slots->fifo_empty_since_ms) <
		MBOX0_SLOT_QUARANTINE_MS)
		return;

	for (i = 0; i < MBOX0_CMD_SLOT_NUM; i++)
	{
		reaped += slots->cmd_used[i] != 0;
		slots->cmd_used[i] = 0;
	}
	/* The hardware FIFO contains pointers into these slots.  Once it has
	 * remained empty for the quarantine interval, the peer has had ample time
	 * to copy every published entry.  ACK-only AP traffic has no later command
	 * fence, so retaining these slots indefinitely exhausts the pool during
	 * RESET/STATE/PWC bring-up.
	 */
	for (i = 0; i < MBOX0_ACK_SLOT_NUM; i++)
	{
		reaped += slots->ack_used[i] != 0;
		slots->ack_used[i] = 0;
	}
	reaped += slots->reset_used != 0;
	reaped += slots->sync_used != 0;
	slots->reset_used = 0;
	slots->sync_used = 0;
	slots->cmd_active = 0;
	slots->fifo_empty_since_ms = 0;
	adapter_diag.tx_reaped += reaped;
}

static bool mbox0_rx_ready(void)
{
	return rx_count < MBOX0_RX_DESC_NUM;
}

static mbox0_rx_desc_t *mbox0_oldest_desc(bool priority)
{
	mbox0_rx_desc_t *oldest = NULL;
	u8 i;

	for (i = 0; i < MBOX0_RX_DESC_NUM; i++)
	{
		u8 ctrl;

		if (!rx_desc[i].used)
			continue;
		ctrl = (rx_desc[i].data.param0 >> 12) & 0x0f;
		if (((ctrl & MBOX0_CTRL_ACK_BOX) != 0) != priority)
			continue;
		if (!oldest || (int32_t)(rx_desc[i].order - oldest->order) < 0)
			oldest = &rx_desc[i];
	}
	return oldest;
}

static mbox0_stable_slot_t *mbox0_alloc_slot(u8 dst, u8 ctrl,
					     u8 *kind, u8 *index)
{
	mbox0_tx_slots_t *slots = &mailbox_slots[dst];
	u8 i;

	if (ctrl == (MBOX0_CTRL_ACK_BOX | MBOX0_CTRL_RESET))
	{
		if (slots->reset_used)
			return NULL;
		slots->reset_used = 1;
		slots->reset_order = ++slots->publish_order;
		*kind = 2;
		return &slots->reset;
	}

	if (ctrl & MBOX0_CTRL_ACK_BOX)
	{
		for (i = 0; i < MBOX0_ACK_SLOT_NUM; i++)
		{
			if (!slots->ack_used[i])
			{
				slots->ack_used[i] = 1;
				slots->ack_order[i] = ++slots->publish_order;
				*kind = 1;
				*index = i;
				return &slots->ack[i];
			}
		}
		return NULL;
	}

	if (ctrl == MBOX0_CTRL_SYNC_TX)
	{
		if (slots->sync_used)
			return NULL;
		slots->sync_used = 1;
		slots->sync_order = ++slots->publish_order;
		*kind = 3;
		return &slots->sync;
	}

	for (i = 0; i < MBOX0_CMD_SLOT_NUM; i++)
	{
		if (!slots->cmd_used[i])
		{
			slots->cmd_used[i] = 1;
			slots->cmd_order[i] = ++slots->publish_order;
			slots->cmd_active = i + 1;
			*kind = 0;
			*index = i;
			return &slots->cmd[i];
		}
	}

	return NULL;
}

static void mbox0_release_slot(u8 dst, u8 kind, u8 index)
{
	mbox0_tx_slots_t *slots = &mailbox_slots[dst];

	if (kind == 0)
	{
		slots->cmd_used[index] = 0;
		if (slots->cmd_active == index + 1)
			slots->cmd_active = 0;
	}
	else if (kind == 1)
		slots->ack_used[index] = 0;
	else if (kind == 2)
		slots->reset_used = 0;
	else
		slots->sync_used = 0;
}

void bk_mailbox_poll(void)
{
	u8 dst;

	u32 flags = rtos_disable_int();
	for (dst = 0; dst < MBOX_CHNL_NUM; dst++)
	{
		if (dst != MAILBOX_CPU0)
			mbox0_reap_slots(dst);
	}
	mbox0_drv_poll();
	rtos_enable_int(flags);

	while (1)
	{
		mbox0_rx_desc_t desc;
		mbox0_rx_desc_t *selected;

		flags = rtos_disable_int();
		if (!rx_count)
		{
			rtos_enable_int(flags);
			break;
		}
		selected = mbox0_oldest_desc(true);
		if (!selected)
			selected = mbox0_oldest_desc(false);
		if (!selected ||
			(((selected->data.param0 >> 12) & MBOX0_CTRL_ACK_BOX) == 0 &&
			 upper_rx_ready && !upper_rx_ready()))
		{
			rtos_enable_int(flags);
			break;
		}
		desc = *selected;
		selected->used = 0;
		rx_count--;
		rtos_enable_int(flags);
		if (mailbox_callback[desc.source])
			mailbox_callback[desc.source](&desc.data);

		flags = rtos_disable_int();
		mbox0_drv_poll();
		rtos_enable_int(flags);
	}
}

static void mbox0_rx_isr(mbox0_message_t *msg)
{
	u32 address;
	u32 source;
	u32 reaped = 0;
	mbox0_rx_desc_t *desc;
	u8 ctrl;
	u8 i;

	if (!msg)
		return;

	source = msg->src_cpu;
	if (source != MAILBOX_CPU1)
	{
		adapter_diag.bad_sid++;
		return;
	}
	if (msg->data[1] != sizeof(mailbox_data_t))
	{
		adapter_diag.bad_length++;
		return;
	}

	address = msg->data[0];
	if (address & 0x3u)
	{
		adapter_diag.bad_alignment++;
		return;
	}
	if (address > UINT32_MAX - sizeof(mailbox_data_t) ||
		address < MBOX0_AP_RAM_START ||
		address + sizeof(mailbox_data_t) > MBOX0_AP_RAM_END)
	{
		adapter_diag.bad_address++;
		return;
	}
	if (rx_count >= MBOX0_RX_DESC_NUM)
	{
		adapter_diag.descriptor_overflow++;
		return;
	}

#if CONFIG_CACHE_ENABLE
	SCB_InvalidateDCache_by_Addr((void *)address, sizeof(mailbox_data_t));
#endif
	__DMB();
	desc = NULL;
	for (i = 0; i < MBOX0_RX_DESC_NUM; i++)
	{
		if (!rx_desc[i].used)
		{
			desc = &rx_desc[i];
			break;
		}
	}
	if (!desc)
	{
		adapter_diag.descriptor_overflow++;
		return;
	}
	memcpy(&desc->data, (void *)address, sizeof(desc->data));
	adapter_diag.last_rx_header = desc->data.param0;
	ctrl = (desc->data.param0 >> 12) & 0x0f;
	if ((ctrl & MBOX0_CTRL_ACK_BOX) == 0)
	{
		adapter_diag.last_cmd_header = desc->data.param0;
		adapter_diag.cmd_received++;
	}
	if (ctrl == 0)
	{
		/* AP serializes ordinary logical commands.  Receiving the next one
		 * proves that it copied the preceding CP transport ACK, providing an
		 * exact lifetime fence even during a continuous UART/PWC burst. */
		for (i = 0; i < MBOX0_ACK_SLOT_NUM; i++)
		{
			reaped += mailbox_slots[source].ack_used[i] != 0;
			mailbox_slots[source].ack_used[i] = 0;
		}
		adapter_diag.tx_reaped += reaped;
	}
	desc->source = source;
	desc->order = ++rx_order;
	desc->used = 1;
	rx_count++;
	adapter_diag.rx_accepted++;

	/* The vendor adapter dispatches the logical callback from the mailbox
	 * receive path.  Keep ACK generation in the same interrupt transaction;
	 * delaying it until mb_chnl_poll() can stall AP bring-up. */
	if (mailbox_callback[source] != NULL)
	{
		adapter_diag.callback_called++;
		mailbox_callback[source](&desc->data);
	}
	else
		adapter_diag.callback_missing++;

	desc->used = 0;
	rx_count--;

}

bk_err_t bk_mailbox_init(void)
{
	int ret_code = mbox0_drv_init();

	if (ret_code != 0)
		return BK_FAIL;
	memset(mailbox_slots, 0, sizeof(mailbox_slots));
	memset(rx_desc, 0, sizeof(rx_desc));
	memset(&adapter_diag, 0, sizeof(adapter_diag));
	upper_rx_ready = NULL;
	rx_count = 0;
	rx_order = 0;
	mbox0_drv_ready_callback_register(mbox0_rx_ready);
	return mbox0_drv_callback_register(mbox0_rx_isr);
}

bk_err_t bk_mailbox_deinit(void)
{
	mbox0_drv_ready_callback_register(NULL);
	return mbox0_drv_deinit() == 0 ? BK_OK : BK_FAIL;
}

bk_err_t bk_mailbox_ready(mailbox_endpoint_t src, mailbox_endpoint_t dst,
				  uint32_t box_id)
{
	(void)src;
	(void)box_id;
	return dst < MBOX_CHNL_NUM ? BK_OK : BK_ERR_MAILBOX_SRC_DST;
}

bk_err_t bk_mailbox_send(mailbox_data_t *data, mailbox_endpoint_t src,
			 mailbox_endpoint_t dst, void *arg)
{
	mbox0_message_t message;
	mbox0_stable_slot_t *slot;
	uint32_t fifo_status;
	u8 ctrl;
	u8 kind = 0;
	u8 index = 0;
	int ret_code;

	(void)arg;
	if (src != MAILBOX_CPU0 || dst >= MBOX_CHNL_NUM)
		return BK_ERR_MAILBOX_SRC_DST;
	if (!data)
		return BK_ERR_NULL_PARAM;

	ret_code = mbox0_drv_get_send_stat(dst, &fifo_status);
	if (ret_code != MBOX0_HAL_OK)
		return BK_ERR_MAILBOX_SRC_DST;
	if (fifo_status & RX_FIFO_STAT_FULL)
		return BK_ERR_MAILBOX_TIMEOUT;

	ctrl = (data->param0 >> 12) & 0x0f;
	mbox0_reap_slots(dst);
	slot = mbox0_alloc_slot(dst, ctrl, &kind, &index);
	if (!slot)
	{
		adapter_diag.tx_slot_busy++;
		return BK_ERR_BUSY;
	}

	memset(slot, 0, sizeof(*slot));
	memcpy(&slot->data, data, sizeof(slot->data));
	mailbox_slots[dst].fifo_empty_since_ms = 0;
#if CONFIG_CACHE_ENABLE
	SCB_CleanDCache_by_Addr((u32 *)slot, sizeof(*slot));
#endif
	__DMB();
	message.data[0] = (u32)&slot->data;
	message.data[1] = sizeof(slot->data);
	message.dest_cpu = dst;
	ret_code = mbox0_drv_send_message(&message);
	if (ret_code != MBOX0_HAL_OK)
	{
		mbox0_release_slot(dst, kind, index);
		if (ctrl & MBOX0_CTRL_ACK_BOX)
			adapter_diag.ack_send_fail++;
		return BK_ERR_MAILBOX_NOT_INIT;
	}
	if (ctrl == MBOX0_CTRL_ACK_BOX)
	{
		adapter_diag.last_ack_header = slot->data.param0;
		adapter_diag.ack_sent++;
	}
	return BK_OK;
}

void bk_mailbox_tx_complete(mailbox_endpoint_t dst)
{
	mbox0_tx_slots_t *slots;
	u32 completed_order;
	u8 i;

	if (dst >= MBOX_CHNL_NUM)
		return;
	slots = &mailbox_slots[dst];
	if (!slots->cmd_active)
		return;
	completed_order = slots->cmd_order[slots->cmd_active - 1];
	for (i = 0; i < MBOX0_ACK_SLOT_NUM; i++)
	{
		if (slots->ack_used[i] && slots->ack_order[i] < completed_order)
			slots->ack_used[i] = 0;
	}
	for (i = 0; i < MBOX0_CMD_SLOT_NUM; i++)
	{
		if (slots->cmd_used[i] && slots->cmd_order[i] <= completed_order)
			slots->cmd_used[i] = 0;
	}
	if (slots->reset_used && slots->reset_order < completed_order)
		slots->reset_used = 0;
	if (slots->sync_used && slots->sync_order < completed_order)
		slots->sync_used = 0;
	slots->cmd_active = 0;
}

void bk_mailbox_tx_timeout(mailbox_endpoint_t dst)
{
	if (dst < MBOX_CHNL_NUM)
		mailbox_slots[dst].cmd_active = 0;
}

bk_err_t bk_mailbox_recv_callback_register(mailbox_endpoint_t src,
					   mailbox_endpoint_t dst,
					   mailbox_callback_t callback)
{
	if (dst != MAILBOX_CPU0 || src == MAILBOX_CPU0 || src >= MBOX_CHNL_NUM)
		return BK_ERR_MAILBOX_SRC_DST;
	mailbox_callback[src] = callback;
	return BK_OK;
}

bk_err_t bk_mailbox_recv_callback_unregister(mailbox_endpoint_t src,
					     mailbox_endpoint_t dst)
{
	return bk_mailbox_recv_callback_register(src, dst, NULL);
}

void bk_mailbox_get_diag(mbox0_adapter_diag_t *diag)
{
	if (diag)
		*diag = adapter_diag;
}

void bk_mailbox_rx_ready_callback_register(mailbox_rx_ready_t callback)
{
	upper_rx_ready = callback;
}

u8 bk_mailbox_ack_slots_used(mailbox_endpoint_t dst)
{
	u8 count = 0;
	u8 i;

	if (dst >= MBOX_CHNL_NUM)
		return 0;
	for (i = 0; i < MBOX0_ACK_SLOT_NUM; i++)
		count += mailbox_slots[dst].ack_used[i] != 0;
	return count;
}
