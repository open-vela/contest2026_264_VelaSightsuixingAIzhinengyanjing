#ifndef _AP_CONSOLE_BRIDGE_H_
#define _AP_CONSOLE_BRIDGE_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <common/bk_err.h>
#include <common/bk_typedef.h>

typedef enum {
	AP_CONSOLE_OUTPUT_LOG = 0,
	AP_CONSOLE_OUTPUT_RAW,
} ap_console_output_mode_t;

typedef enum {
	AP_BRIDGE_READY_MBUART = 0x01,
	AP_BRIDGE_READY_IPC = 0x02,
	AP_BRIDGE_READY_PWC = 0x04,
} ap_console_ready_flag_t;

typedef struct {
	u32 rx_bytes;
	u32 rx_queue_fail;
	u32 tx_bytes;
	u32 tx_drop;
	u32 tx_partial;
	u32 link_down;
	u32 reset;
	u32 timeout;
	u16 tx_queued;
	u16 mb_tx_free;
	u16 mb_rx_ready;
	u16 mb_status;
	u8 link_ready;
	u8 output_mode;
	u8 active_busy;
	u8 active_channel;
	u8 active_sequence;
	u8 active_command;
	u8 ready_flags;
	u8 transport_state;
	u32 bad_envelope;
	u32 ack_retry;
	u32 ack_queue_full;
	u32 reset_bad;
} ap_console_bridge_stats_t;

bk_err_t ap_console_bridge_init(void);
u16 ap_console_bridge_write(const u8 *data, u16 length);
void ap_console_bridge_purge_tx(void);
bk_err_t ap_console_bridge_set_output_mode(ap_console_output_mode_t mode);
bool ap_console_bridge_is_ready(void);
void ap_console_bridge_get_stats(ap_console_bridge_stats_t *stats);
void ap_console_bridge_ready_update(ap_console_ready_flag_t flag, bool ready);
void ap_console_bridge_link_down(void);

#ifdef __cplusplus
}
#endif

#endif
