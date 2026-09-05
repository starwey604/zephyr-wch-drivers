/* SPDX-License-Identifier: Apache-2.0 */
#ifndef WCH_UART_RX_STATE_H_
#define WCH_UART_RX_STATE_H_

enum wch_uart_rx_phase {
	WCH_RX_IDLE,
	WCH_RX_ACTIVE,
	WCH_RX_STOPPING
};

struct wch_uart_rx_note {
	struct uart_event event;
	uint32_t generation;
};

struct wch_uart_rx {
	enum wch_uart_rx_phase phase;
	uint8_t *buffer;
	size_t length;
	uint8_t *next;
	size_t next_length;
	struct dma_block_config block;
	struct dma_config dma;
	/* One outstanding request permits one queued buffer. A new request is
	 * opened only when delivered, bounding pending events even on reentry.
	 */
	bool request_open;
	bool dispatching;
	uint32_t generation;
	struct wch_uart_rx_note notes[12];
	uint8_t head;
	uint8_t count;
};
#endif
