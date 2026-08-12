#ifndef __MBOX0_ADAPTER_H__
#define __MBOX0_ADAPTER_H__

#include <driver/mailbox_types.h>
#include <common/bk_err.h>
#include "mbox0_drv.h"

typedef struct {
	u32 bad_sid;
	u32 bad_length;
	u32 bad_alignment;
	u32 bad_address;
	u32 descriptor_overflow;
	u32 rx_accepted;
	u32 tx_slot_busy;
	u32 tx_reaped;
	u32 last_rx_header;
	u32 last_cmd_header;
	u32 last_ack_header;
	u32 ack_sent;
	u32 cmd_received;
	u32 callback_called;
	u32 callback_missing;
	u32 ack_send_fail;
} mbox0_adapter_diag_t;

typedef bool (*mailbox_rx_ready_t)(void);

bk_err_t bk_mailbox_init(void);
bk_err_t bk_mailbox_deinit(void);
bk_err_t bk_mailbox_send(mailbox_data_t *data, mailbox_endpoint_t src, mailbox_endpoint_t dst, void *arg);
bk_err_t bk_mailbox_recv_callback_register(mailbox_endpoint_t src, mailbox_endpoint_t dst, mailbox_callback_t callback);
bk_err_t bk_mailbox_recv_callback_unregister(mailbox_endpoint_t src, mailbox_endpoint_t dst);
void bk_mailbox_tx_complete(mailbox_endpoint_t dst);
void bk_mailbox_tx_timeout(mailbox_endpoint_t dst);
void bk_mailbox_poll(void);
void bk_mailbox_get_diag(mbox0_adapter_diag_t *diag);
void bk_mailbox_rx_ready_callback_register(mailbox_rx_ready_t callback);
u8 bk_mailbox_ack_slots_used(mailbox_endpoint_t dst);

#endif
