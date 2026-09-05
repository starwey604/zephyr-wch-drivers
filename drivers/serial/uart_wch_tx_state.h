/* SPDX-License-Identifier: Apache-2.0 */
#ifndef WCH_UART_TX_STATE_H_
#define WCH_UART_TX_STATE_H_

enum wch_uart_tx_phase {
	WCH_TX_IDLE,
	WCH_TX_DMA,
	WCH_TX_WAIT_TC,
};

struct wch_uart_tx {
	enum wch_uart_tx_phase phase;
	uart_callback_t callback;
	void *user_data;
	const struct device *uart;
	const uint8_t *buffer;
	size_t length;
	struct dma_block_config block;
	struct dma_config dma;
	struct k_work_delayable timeout_work;
	int64_t deadline;
	bool reserved;
	bool needs_tc;
};

/* Local snapshot: no application callback is invoked holding the UART lock. */
struct wch_uart_tx_notification {
	uart_callback_t callback;
	void *user_data;
	struct uart_event event;
};
#endif
