#ifndef __VENDOR_BEKEN_CHIPS_BK7258_HARDWARE_BK7258_MBOX_H
#define __VENDOR_BEKEN_CHIPS_BK7258_HARDWARE_BK7258_MBOX_H

#include <stdint.h>

#define BK7258_MBOX_FIFO_SIZE 8u
#define BK7258_MBOX_IRQ       79
#define BK7258_MBOX_CMD_FIFO  1u
#define BK7258_MBOX_ACK_FIFO  0u

typedef struct
{
  uint8_t src_cpu;
  uint8_t dest_cpu;
  uint32_t data[2];
} bk7258_mbox_message_t;

typedef void (*bk7258_mbox_callback_t)(const bk7258_mbox_message_t *message);

int bk7258_mbox_init(void);
int bk7258_mbox_send(uint8_t destination, const uint32_t data[2]);
int bk7258_mbox_send_message(uint8_t command, uint8_t logical_channel,
                             uint32_t param1, uint32_t param2,
                             uint32_t param3);

int bk7258_mbox_uart_write(const uint8_t *data, uint16_t length);
bool bk7258_mbox_uart_txready(void);
bool bk7258_mbox_uart_txempty(void);
int bk7258_mbox_receive(bk7258_mbox_message_t *message);
uint32_t bk7258_mbox_rx_status(void);
void bk7258_mbox_set_callback(bk7258_mbox_callback_t callback);

int bk7258_mailbox_init(void);
int bk7258_mailbox_send_pwc(uint8_t command, uint32_t p1, uint32_t p2,
                            uint32_t p3);
void bk7258_mailbox_set_pwc_rx(void (*callback)(const void *message));

#endif
