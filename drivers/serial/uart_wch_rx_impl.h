/* SPDX-License-Identifier: Apache-2.0 */
#ifndef WCH_UART_RX_IMPL_H_
#define WCH_UART_RX_IMPL_H_

/* All state/register transitions use the shared UART lock. Events are drained
 * outside it, serially, including when callbacks disable/restart reception.
 */
static void usart_wch_rx_queue(struct wch_uart_rx *rx, struct uart_event event)
{
	__ASSERT_NO_MSG(rx->count < ARRAY_SIZE(rx->notes));
	rx->notes[(rx->head + rx->count) % ARRAY_SIZE(rx->notes)] =
		(struct wch_uart_rx_note){.event = event, .generation = rx->generation};
	rx->count++;
}

static void usart_wch_rx_dispatch(const struct device *dev)
{
	struct usart_wch_data *data = dev->data;
	struct wch_uart_rx *rx = &data->rx;
	k_spinlock_key_t key = k_spin_lock(&data->lock);

	if (rx->dispatching) {
		k_spin_unlock(&data->lock, key);
		return;
	}
	rx->dispatching = true;
	while (rx->count != 0) {
		struct wch_uart_rx_note note = rx->notes[rx->head];
		uart_callback_t callback = data->tx.callback;
		void *user = data->tx.user_data;

		rx->head = (rx->head + 1) % ARRAY_SIZE(rx->notes);
		rx->count--;
		if (note.event.type == UART_RX_BUF_REQUEST) {
			if (rx->phase != WCH_RX_ACTIVE || note.generation != rx->generation) {
				continue;
			}
			rx->request_open = true;
		} else if (note.event.type == UART_RX_DISABLED) {
			/* No new loan before all old releases have been delivered. */
			rx->phase = WCH_RX_IDLE;
		}
		k_spin_unlock(&data->lock, key);
		if (callback != NULL) {
			callback(dev, &note.event, user);
		}
		key = k_spin_lock(&data->lock);
	}
	rx->dispatching = false;
	k_spin_unlock(&data->lock, key);
}

static int usart_wch_rx_errors(uint16_t flags)
{
	return ((flags & USART_STATR_PE) ? UART_ERROR_PARITY : 0) |
	       ((flags & USART_STATR_FE) ? UART_ERROR_FRAMING : 0) |
	       ((flags & USART_STATR_NE) ? UART_ERROR_NOISE : 0) |
	       ((flags & USART_STATR_ORE) ? UART_ERROR_OVERRUN : 0);
}

static void usart_wch_rx_clear(const struct device *dev)
{
	const struct usart_wch_config *config = dev->config;
	USART_TypeDef *regs = config->regs;

	/* Session boundary only, with DMAR gated and DMA stopped. Do not use
	 * this sequence for IDLE during live DMA: it can consume an RX byte.
	 * Bytes outside the accepted DMA session are deliberately discarded.
	 */
	regs->CTLR1 &= ~USART_CTLR1_RE;
	(void)regs->STATR;
	(void)regs->DATAR;
	regs->CTLR1 |= USART_CTLR1_RE;
}

static void usart_wch_rx_gate(const struct device *dev)
{
	const struct usart_wch_config *config = dev->config;

	config->regs->CTLR3 &= ~(USART_CTLR3_DMAR | USART_CTLR3_EIE);
	config->regs->CTLR1 &= ~USART_CTLR1_PEIE;
	(void)dma_stop(config->dma_rx_dev, config->dma_rx_channel);
}

static size_t usart_wch_rx_count(const struct device *dev, int *reason)
{
	const struct usart_wch_config *config = dev->config;
	struct usart_wch_data *data = dev->data;
	struct dma_status status;

	if (dma_get_status(config->dma_rx_dev, config->dma_rx_channel, &status) != 0 ||
	    status.pending_length > data->rx.length) {
		/* No UART enum for DMA faults: report lost/unreliable reception as
		 * OVERRUN, with zero rather than inventing a received byte count.
		 */
		*reason |= UART_ERROR_OVERRUN;
		return 0;
	}
	return data->rx.length - status.pending_length;
}

static void usart_wch_rx_ready(struct wch_uart_rx *rx, size_t count)
{
	if (count != 0) {
		usart_wch_rx_queue(
			rx, (struct uart_event){
				    .type = UART_RX_RDY,
				    .data.rx = {.buf = rx->buffer, .offset = 0, .len = count},
			    });
	}
}

static void usart_wch_rx_release(struct wch_uart_rx *rx, uint8_t *buffer)
{
	usart_wch_rx_queue(rx, (struct uart_event){
				       .type = UART_RX_BUF_RELEASED,
				       .data.rx_buf.buf = buffer,
			       });
}

/* Caller has already quiesced DMA and supplied its stable committed count. */
static void usart_wch_rx_finish(const struct device *dev, size_t count, int reason)
{
	struct usart_wch_data *data = dev->data;
	struct wch_uart_rx *rx = &data->rx;

	if (reason != 0) {
		usart_wch_rx_queue(rx, (struct uart_event){
					       .type = UART_RX_STOPPED,
					       .data.rx_stop = {.reason = reason,
								.data = {.buf = rx->buffer,
									 .offset = 0,
									 .len = count}},
				       });
	}
	usart_wch_rx_ready(rx, count);
	usart_wch_rx_release(rx, rx->buffer);
	if (rx->next != NULL) {
		usart_wch_rx_release(rx, rx->next);
	}
	rx->buffer = NULL;
	rx->next = NULL;
	rx->length = 0;
	rx->next_length = 0;
	rx->request_open = false;
	rx->phase = WCH_RX_STOPPING;
	usart_wch_rx_clear(dev);
	usart_wch_rx_queue(rx, (struct uart_event){.type = UART_RX_DISABLED});
}

static void usart_wch_rx_dma_callback(const struct device *dma, void *user, uint32_t channel,
				      int status);

static int usart_wch_rx_start(const struct device *dev)
{
	const struct usart_wch_config *config = dev->config;
	struct usart_wch_data *data = dev->data;
	struct wch_uart_rx *rx = &data->rx;
	int ret;

	rx->block = (struct dma_block_config){
		.source_address = (uintptr_t)&config->regs->DATAR,
		.dest_address = (uintptr_t)rx->buffer,
		.block_size = rx->length,
		.source_addr_adj = DMA_ADDR_ADJ_NO_CHANGE,
		.dest_addr_adj = DMA_ADDR_ADJ_INCREMENT,
	};
	rx->dma = (struct dma_config){
		.channel_direction = PERIPHERAL_TO_MEMORY,
		.source_data_size = 1,
		.dest_data_size = 1,
		.block_count = 1,
		.head_block = &rx->block,
		.user_data = (void *)dev,
		.dma_callback = usart_wch_rx_dma_callback,
	};
	ret = dma_config(config->dma_rx_dev, config->dma_rx_channel, &rx->dma);
	if (ret == 0) {
		ret = dma_start(config->dma_rx_dev, config->dma_rx_channel);
	}
	if (ret != 0) {
		usart_wch_rx_gate(dev);
		return ret;
	}
	rx->generation++;
	rx->request_open = false;
	rx->phase = WCH_RX_ACTIVE;
	config->regs->CTLR1 |= USART_CTLR1_PEIE;
	config->regs->CTLR3 |= USART_CTLR3_EIE | USART_CTLR3_DMAR;
	usart_wch_rx_queue(rx, (struct uart_event){.type = UART_RX_BUF_REQUEST});
	return 0;
}

static void usart_wch_rx_dma_callback(const struct device *dma, void *user, uint32_t channel,
				      int status)
{
	const struct device *dev = user;
	const struct usart_wch_config *config = dev->config;
	struct usart_wch_data *data = dev->data;
	struct wch_uart_rx *rx = &data->rx;
	k_spinlock_key_t key = k_spin_lock(&data->lock);

	if (dma != config->dma_rx_dev || channel != config->dma_rx_channel ||
	    rx->phase != WCH_RX_ACTIVE || (status >= 0 && status != DMA_STATUS_COMPLETE)) {
		goto out;
	}
	usart_wch_rx_gate(dev);
	int reason = usart_wch_rx_errors(config->regs->STATR);
	if (status < 0) {
		reason |= UART_ERROR_OVERRUN;
	}
	if (reason != 0) {
		size_t count = usart_wch_rx_count(dev, &reason);

		usart_wch_rx_finish(dev, count, reason);
	} else if (rx->next == NULL) {
		usart_wch_rx_finish(dev, rx->length, 0);
	} else {
		usart_wch_rx_ready(rx, rx->length);
		usart_wch_rx_release(rx, rx->buffer);
		rx->buffer = rx->next;
		rx->length = rx->next_length;
		rx->next = NULL;
		rx->next_length = 0;
		/* Rearm before callbacks, but normal-mode DMA has a software gap.
		 * Do not clear DATAR across this in-session buffer boundary.
		 */
		if (usart_wch_rx_start(dev) != 0) {
			usart_wch_rx_finish(dev, 0, UART_ERROR_OVERRUN);
		}
	}
out:
	k_spin_unlock(&data->lock, key);
	usart_wch_rx_dispatch(dev);
}

static void usart_wch_rx_isr_locked(const struct device *dev)
{
	const struct usart_wch_config *config = dev->config;
	struct usart_wch_data *data = dev->data;
	int reason = usart_wch_rx_errors(config->regs->STATR);

	if (data->rx.phase == WCH_RX_ACTIVE && reason != 0) {
		usart_wch_rx_gate(dev);
		size_t count = usart_wch_rx_count(dev, &reason);

		usart_wch_rx_finish(dev, count, reason);
	}
}

static int usart_wch_rx_enable(const struct device *dev, uint8_t *buffer, size_t length,
			       int32_t timeout)
{
	struct usart_wch_data *data = dev->data;
	struct wch_uart_rx *rx = &data->rx;
	int ret = wch_uart_dma_buffer_check(buffer, length);

	if (ret != 0 || timeout < SYS_FOREVER_US) {
		return ret != 0 ? ret : -EINVAL;
	}
	/* Live IDLE flag clearing must be qualified on silicon first. */
	if (timeout != SYS_FOREVER_US) {
		return -ENOTSUP;
	}
	k_spinlock_key_t key = k_spin_lock(&data->lock);

	if (rx->phase != WCH_RX_IDLE) {
		ret = -EBUSY;
	} else if (!data->tx.reserved || data->tx.callback == NULL) {
		ret = -EACCES;
	} else {
		usart_wch_rx_gate(dev);
		usart_wch_rx_clear(dev);
		rx->buffer = buffer;
		rx->length = length;
		ret = usart_wch_rx_start(dev);
		if (ret != 0) {
			rx->buffer = NULL;
			rx->length = 0;
		}
	}
	k_spin_unlock(&data->lock, key);
	usart_wch_rx_dispatch(dev);
	return ret;
}

static int usart_wch_rx_buf_rsp(const struct device *dev, uint8_t *buffer, size_t length)
{
	struct usart_wch_data *data = dev->data;
	struct wch_uart_rx *rx = &data->rx;
	int ret = wch_uart_dma_buffer_check(buffer, length);

	if (ret != 0) {
		return ret;
	}
	k_spinlock_key_t key = k_spin_lock(&data->lock);

	if (rx->phase != WCH_RX_ACTIVE) {
		ret = -EACCES;
	} else if (rx->next != NULL || !rx->request_open) {
		ret = -EBUSY;
	} else if ((uintptr_t)buffer <= (uintptr_t)rx->buffer
			   ? (uintptr_t)rx->buffer - (uintptr_t)buffer < length
			   : (uintptr_t)buffer - (uintptr_t)rx->buffer < rx->length) {
		ret = -EINVAL;
	} else {
		rx->next = buffer;
		rx->next_length = length;
		rx->request_open = false;
	}
	k_spin_unlock(&data->lock, key);
	return ret;
}

static int usart_wch_rx_disable(const struct device *dev)
{
	const struct usart_wch_config *config = dev->config;
	struct usart_wch_data *data = dev->data;
	k_spinlock_key_t key = k_spin_lock(&data->lock);

	if (data->rx.phase != WCH_RX_ACTIVE) {
		k_spin_unlock(&data->lock, key);
		return -EFAULT;
	}
	usart_wch_rx_gate(dev);
	int reason = usart_wch_rx_errors(config->regs->STATR);
	size_t count = usart_wch_rx_count(dev, &reason);

	usart_wch_rx_finish(dev, count, reason);
	k_spin_unlock(&data->lock, key);
	usart_wch_rx_dispatch(dev);
	return 0;
}
#endif
