/* SPDX-License-Identifier: Apache-2.0 */
#ifndef WCH_UART_TX_IMPL_H_
#define WCH_UART_TX_IMPL_H_

/* Internal implementation included after the UART device structures. Native
 * tests compile the same source; only HAL/pinctrl and the DMA provider are faked.
 */
static void usart_wch_tx_notify(const struct device *dev, struct wch_uart_tx_notification *note)
{
	if (note->callback != NULL) {
		note->callback(dev, &note->event, note->user_data);
	}
}

static void usart_wch_tx_finish_locked(const struct device *dev, enum uart_event_type type,
				       struct wch_uart_tx_notification *note)
{
	const struct usart_wch_config *config = dev->config;
	struct usart_wch_data *data = dev->data;
	struct wch_uart_tx *tx = &data->tx;
	struct dma_status status;
	size_t count = tx->phase == WCH_TX_WAIT_TC ? tx->length : 0;
	bool no_progress = false;

	/* Gate requests before quiescing DMA, then take a stable remaining count.
	 * WCH_DMA stop cannot fail for the validated, reserved channel index.
	 */
	config->regs->CTLR3 &= ~USART_CTLR3_DMAT;
	config->regs->CTLR1 &= ~USART_CTLR1_TCIE;
	(void)dma_stop(config->dma_tx_dev, config->dma_tx_channel);
	if (type == UART_TX_DONE) {
		count = tx->length;
	} else if (dma_get_status(config->dma_tx_dev, config->dma_tx_channel, &status) == 0 &&
		   status.pending_length <= tx->length) {
		/* DMA progress, not necessarily bytes fully shifted onto the wire. */
		count = tx->length - status.pending_length;
		no_progress = count == 0;
	}
	/* Leave a queued/running worker harmlessly armed. Cancelling a running
	 * worker here would prevent a callback from immediately scheduling the
	 * next TX deadline. Static device storage outlives that final wakeup.
	 */
	tx->deadline = 0;
	*note = (struct wch_uart_tx_notification){
		.callback = tx->callback,
		.user_data = tx->user_data,
		.event = {.type = type, .data.tx = {.buf = tx->buffer, .len = count}},
	};
	tx->needs_tc =
		type != UART_TX_DONE && !no_progress && !(config->regs->STATR & USART_STATR_TC);
	tx->buffer = NULL;
	tx->length = 0;
	tx->phase = WCH_TX_IDLE;
}

static void usart_wch_tx_dma_callback(const struct device *dma, void *user, uint32_t channel,
				      int status)
{
	const struct device *dev = user;
	const struct usart_wch_config *config = dev->config;
	struct usart_wch_data *data = dev->data;
	struct wch_uart_tx_notification note = {0};
	k_spinlock_key_t key = k_spin_lock(&data->lock);

	if (dma != config->dma_tx_dev || channel != config->dma_tx_channel ||
	    data->tx.phase != WCH_TX_DMA) {
		goto out;
	}
	if (status < 0) {
		usart_wch_tx_finish_locked(dev, UART_TX_ABORTED, &note);
	} else if (status == DMA_STATUS_COMPLETE) {
		config->regs->CTLR3 &= ~USART_CTLR3_DMAT;
		data->tx.phase = WCH_TX_WAIT_TC;
		/* TC may already be set if the DMA interrupt was delayed. Do not
		 * clear it here or the true final completion could be lost.
		 */
		if (config->regs->STATR & USART_STATR_TC) {
			usart_wch_tx_finish_locked(dev, UART_TX_DONE, &note);
		} else {
			config->regs->CTLR1 |= USART_CTLR1_TCIE;
		}
	}
out:
	k_spin_unlock(&data->lock, key);
	usart_wch_tx_notify(dev, &note);
}

static void usart_wch_isr(const struct device *dev)
{
	const struct usart_wch_config *config = dev->config;
	struct usart_wch_data *data = dev->data;
	struct wch_uart_tx_notification note = {0};
	k_spinlock_key_t key = k_spin_lock(&data->lock);

#ifdef CONFIG_WCH_UART_ASYNC_RX
	usart_wch_rx_isr_locked(dev);
#endif
	if (data->tx.phase == WCH_TX_WAIT_TC && (config->regs->CTLR1 & USART_CTLR1_TCIE) &&
	    (config->regs->STATR & USART_STATR_TC)) {
		usart_wch_tx_finish_locked(dev, UART_TX_DONE, &note);
	}
	k_spin_unlock(&data->lock, key);
	usart_wch_tx_notify(dev, &note);
#ifdef CONFIG_WCH_UART_ASYNC_RX
	usart_wch_rx_dispatch(dev);
#endif
}

static void usart_wch_tx_timeout(struct k_work *work)
{
	struct wch_uart_tx *tx =
		CONTAINER_OF(k_work_delayable_from_work(work), struct wch_uart_tx, timeout_work);
	const struct device *dev = tx->uart;
	struct usart_wch_data *data = dev->data;
	struct wch_uart_tx_notification note = {0};
	k_spinlock_key_t key = k_spin_lock(&data->lock);

	/* A previous transfer's work handler may still run when a new TX starts.
	 * Validate the current absolute deadline under the same lock, rather
	 * than letting an old invocation abort a newly borrowed buffer.
	 */
	if (tx->phase != WCH_TX_IDLE && tx->deadline != 0) {
		int64_t remaining = tx->deadline - k_uptime_ticks();

		if (remaining > 0) {
			(void)k_work_reschedule(&tx->timeout_work, K_TICKS(remaining));
		} else {
			usart_wch_tx_finish_locked(dev, UART_TX_ABORTED, &note);
		}
	}
	k_spin_unlock(&data->lock, key);
	usart_wch_tx_notify(dev, &note);
}

static int usart_wch_tx_init(const struct device *dev)
{
	const struct usart_wch_config *config = dev->config;
	struct usart_wch_data *data = dev->data;
	uint32_t tx_channel = config->dma_tx_channel;
	uint32_t rx_channel = config->dma_rx_channel;
	int channel = dma_request_channel(config->dma_tx_dev, &tx_channel);

	if (channel != (int)tx_channel) {
		if (channel >= 0) {
			dma_release_channel(config->dma_tx_dev, channel);
		}
		return -EBUSY;
	}
	channel = dma_request_channel(config->dma_rx_dev, &rx_channel);
	if (channel != (int)rx_channel) {
		if (channel >= 0) {
			dma_release_channel(config->dma_rx_dev, channel);
		}
		dma_release_channel(config->dma_tx_dev, tx_channel);
		return -EBUSY;
	}
	data->tx.uart = dev;
	data->tx.reserved = true;
	k_work_init_delayable(&data->tx.timeout_work, usart_wch_tx_timeout);
	return 0;
}

static int usart_wch_async_callback_set(const struct device *dev, uart_callback_t callback,
					void *user_data)
{
	struct usart_wch_data *data = dev->data;
	k_spinlock_key_t key = k_spin_lock(&data->lock);

	if (data->tx.phase != WCH_TX_IDLE
#ifdef CONFIG_WCH_UART_ASYNC_RX
	    || data->rx.phase != WCH_RX_IDLE
#endif
	) {
		k_spin_unlock(&data->lock, key);
		return -EBUSY;
	}
	data->tx.callback = callback;
	data->tx.user_data = user_data;
	k_spin_unlock(&data->lock, key);
	return 0;
}

static int usart_wch_tx(const struct device *dev, const uint8_t *buffer, size_t length,
			int32_t timeout)
{
	const struct usart_wch_config *config = dev->config;
	struct usart_wch_data *data = dev->data;
	struct wch_uart_tx *tx = &data->tx;
	int ret = wch_uart_dma_buffer_check(buffer, length);

	if (ret != 0 || timeout < SYS_FOREVER_US) {
		return ret != 0 ? ret : -EINVAL;
	}
	k_spinlock_key_t key = k_spin_lock(&data->lock);

	if (tx->phase != WCH_TX_IDLE || (tx->needs_tc && !(config->regs->STATR & USART_STATR_TC))) {
		ret = -EBUSY;
		goto out;
	}
	if (!tx->reserved || tx->callback == NULL) {
		ret = -EACCES;
		goto out;
	}
	tx->block = (struct dma_block_config){
		.source_address = (uintptr_t)buffer,
		.dest_address = (uintptr_t)&config->regs->DATAR,
		.block_size = length,
		.source_addr_adj = DMA_ADDR_ADJ_INCREMENT,
		.dest_addr_adj = DMA_ADDR_ADJ_NO_CHANGE,
	};
	tx->dma = (struct dma_config){
		.channel_direction = MEMORY_TO_PERIPHERAL,
		.source_data_size = 1,
		.dest_data_size = 1,
		.block_count = 1,
		.head_block = &tx->block,
		.user_data = (void *)dev,
		.dma_callback = usart_wch_tx_dma_callback,
	};
	/* Keep requests gated until all fallible setup has succeeded. */
	config->regs->CTLR3 &= ~USART_CTLR3_DMAT;
	config->regs->CTLR1 &= ~USART_CTLR1_TCIE;
	ret = dma_config(config->dma_tx_dev, config->dma_tx_channel, &tx->dma);
	if (ret != 0) {
		goto out;
	}
	ret = dma_start(config->dma_tx_dev, config->dma_tx_channel);
	if (ret != 0) {
		(void)dma_stop(config->dma_tx_dev, config->dma_tx_channel);
		goto out;
	}
	tx->buffer = buffer;
	tx->length = length;
	tx->phase = WCH_TX_DMA;
	tx->deadline = 0;
	if (timeout != SYS_FOREVER_US) {
		/* Tick-granularity software deadline, also provided without CTS. */
		int64_t ticks = MAX(1, k_us_to_ticks_ceil64(timeout));

		tx->deadline = k_uptime_ticks() + ticks;
		ret = k_work_reschedule(&tx->timeout_work, K_TICKS(ticks));
		if (ret < 0) {
			(void)dma_stop(config->dma_tx_dev, config->dma_tx_channel);
			tx->phase = WCH_TX_IDLE;
			tx->buffer = NULL;
			tx->length = 0;
			tx->deadline = 0;
			goto out;
		}
	}
	tx->needs_tc = false;
	/* W0C: clear TC only; do not read/modify/write other receive flags. */
	config->regs->STATR = (uint16_t)~USART_STATR_TC;
	config->regs->CTLR3 |= USART_CTLR3_DMAT;
	ret = 0;
out:
	k_spin_unlock(&data->lock, key);
	return ret;
}

static int usart_wch_tx_abort(const struct device *dev)
{
	struct usart_wch_data *data = dev->data;
	struct wch_uart_tx_notification note = {0};
	k_spinlock_key_t key = k_spin_lock(&data->lock);

	if (data->tx.phase == WCH_TX_IDLE) {
		k_spin_unlock(&data->lock, key);
		return -EFAULT;
	}
	usart_wch_tx_finish_locked(dev, UART_TX_ABORTED, &note);
	k_spin_unlock(&data->lock, key);
	usart_wch_tx_notify(dev, &note);
	return 0;
}

static void usart_wch_tx_poll_out(const struct device *dev, unsigned char ch)
{
	const struct usart_wch_config *config = dev->config;
	struct usart_wch_data *data = dev->data;

	/* Blocking polling must not interleave a byte inside an async buffer.
	 * Callers must not poll TX from an ISR while async TX owns this port.
	 */
	for (;;) {
		k_spinlock_key_t key = k_spin_lock(&data->lock);

		if (data->tx.phase == WCH_TX_IDLE && (config->regs->STATR & USART_STATR_TXE)) {
			config->regs->DATAR = ch;
			data->tx.needs_tc = true;
			k_spin_unlock(&data->lock, key);
			return;
		}
		k_spin_unlock(&data->lock, key);
	}
}

/* Real unsupported methods: Zephyr async RX wrappers dereference these slots. */
#ifndef CONFIG_WCH_UART_ASYNC_RX
static int usart_wch_rx_unsupported(const struct device *dev, uint8_t *buf, size_t len,
				    int32_t timeout)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(buf);
	ARG_UNUSED(len);
	ARG_UNUSED(timeout);
	return -ENOTSUP;
}

static int usart_wch_rx_buf_unsupported(const struct device *dev, uint8_t *buf, size_t len)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(buf);
	ARG_UNUSED(len);
	return -ENOTSUP;
}

static int usart_wch_rx_disable_unsupported(const struct device *dev)
{
	ARG_UNUSED(dev);
	return -ENOTSUP;
}
#endif
#endif
