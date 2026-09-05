/* SPDX-License-Identifier: Apache-2.0 */
/* Included in the shared fixture: actual production UART, fake DMA/HAL only. */
static uint8_t rx_buffers[2][2][32];
static int response_result;
static bool disable_on_ready;
static bool restart_on_disabled;

static void rx_event(unsigned int port, int status)
{
	struct mock_channel *channel = &dma_channels[configs[port].dma_rx_channel];

	if (status == DMA_STATUS_COMPLETE) {
		channel->pending = 0;
		channel->running = false;
	}
	unsigned int key = irq_lock();

	channel->config.dma_callback(DEVICE_GET(mock_dma), channel->config.user_data,
				     configs[port].dma_rx_channel, status);
	irq_unlock(key);
}

static void expect_rx(unsigned int port, size_t index, enum uart_event_type type, uint8_t *buf,
		      size_t len)
{
	zassert_true(index < captures[port].count);
	struct uart_event *event = &captures[port].events[index];

	zassert_equal(event->type, type, "event %zu: got %d expected %d", index, event->type, type);
	if (type == UART_RX_RDY) {
		zassert_equal_ptr(event->data.rx.buf, buf);
		zassert_equal(event->data.rx.offset, 0);
		zassert_equal(event->data.rx.len, len);
	} else if (type == UART_RX_BUF_RELEASED) {
		zassert_equal_ptr(event->data.rx_buf.buf, buf);
	}
}

static void begin_rx(unsigned int port)
{
	zassert_ok(uart_rx_enable(ports[port], rx_buffers[port][0], 32, SYS_FOREVER_US));
	expect_rx(port, 0, UART_RX_BUF_REQUEST, NULL, 0);
	zassert_true(registers[port].CTLR3 & USART_CTLR3_DMAR);
}

ZTEST(wch_uart_rx, test_validation_and_poll_exclusion)
{
	zassert_equal(uart_rx_enable(ports[0], NULL, 1, -1), -EINVAL);
	zassert_equal(uart_rx_enable(ports[0], buffer, 0, -1), -EINVAL);
	zassert_equal(uart_rx_enable(ports[0], buffer, 65536, -1), -EMSGSIZE);
	zassert_equal(uart_rx_enable(ports[0], buffer, 1, -2), -EINVAL);
	zassert_equal(uart_rx_enable(ports[0], buffer, 1, 0), -ENOTSUP);
	zassert_equal(uart_rx_enable(ports[0], buffer, 1, 100), -ENOTSUP);
	zassert_equal(uart_rx_disable(ports[0]), -EFAULT);
	zassert_equal(uart_rx_buf_rsp(ports[0], buffer, 1), -EACCES);
	zassert_ok(uart_callback_set(ports[0], NULL, NULL));
	zassert_equal(uart_rx_enable(ports[0], buffer, 32, -1), -EACCES);
	zassert_ok(uart_callback_set(ports[0], on_event, &captures[0]));
	begin_rx(0);
	zassert_equal(uart_rx_enable(ports[0], buffer, 32, -1), -EBUSY);
	zassert_equal(uart_callback_set(ports[0], NULL, NULL), -EBUSY);
	unsigned char ch = 0xff;

	registers[0].DATAR = 0x5a;
	registers[0].STATR |= USART_STATR_RXNE;
	zassert_equal(uart_poll_in(ports[0], &ch), -EBUSY);
	zassert_equal(ch, 0xff);
	zassert_equal(uart_rx_buf_rsp(ports[0], rx_buffers[0][0] + 1, 4), -EINVAL);
	zassert_ok(uart_rx_buf_rsp(ports[0], rx_buffers[0][1], 32));
	zassert_equal(uart_rx_buf_rsp(ports[0], buffer, 32), -EBUSY);
}

ZTEST(wch_uart_rx, test_dma_configuration_and_single_full_buffer)
{
	begin_rx(0);
	struct mock_channel *ch = &dma_channels[4];

	zassert_equal(ch->config.channel_direction, PERIPHERAL_TO_MEMORY);
	zassert_equal(ch->config.source_data_size, 1);
	zassert_equal(ch->config.dest_data_size, 1);
	zassert_equal(ch->block.source_address, (uintptr_t)&registers[0].DATAR);
	zassert_equal(ch->block.dest_address, (uintptr_t)rx_buffers[0][0]);
	zassert_equal(ch->block.source_addr_adj, DMA_ADDR_ADJ_NO_CHANGE);
	zassert_equal(ch->block.dest_addr_adj, DMA_ADDR_ADJ_INCREMENT);
	zassert_false(ch->config.cyclic);
	rx_event(0, DMA_STATUS_HALF_COMPLETE);
	zassert_equal(captures[0].count, 1);
	rx_event(0, DMA_STATUS_COMPLETE);
	zassert_equal(captures[0].count, 4);
	expect_rx(0, 1, UART_RX_RDY, rx_buffers[0][0], 32);
	expect_rx(0, 2, UART_RX_BUF_RELEASED, rx_buffers[0][0], 0);
	expect_rx(0, 3, UART_RX_DISABLED, NULL, 0);
	zassert_false(registers[0].CTLR3 & (USART_CTLR3_DMAR | USART_CTLR3_EIE));
	rx_event(0, DMA_STATUS_COMPLETE);
	zassert_equal(captures[0].count, 4);
}

ZTEST(wch_uart_rx, test_two_buffer_handoff)
{
	begin_rx(0);
	zassert_ok(uart_rx_buf_rsp(ports[0], rx_buffers[0][1], 32));
	rx_event(0, DMA_STATUS_COMPLETE);
	expect_rx(0, 1, UART_RX_RDY, rx_buffers[0][0], 32);
	expect_rx(0, 2, UART_RX_BUF_RELEASED, rx_buffers[0][0], 0);
	expect_rx(0, 3, UART_RX_BUF_REQUEST, NULL, 0);
	zassert_equal(dma_channels[4].block.dest_address, (uintptr_t)rx_buffers[0][1]);
	zassert_true(dma_channels[4].running);
	rx_event(0, DMA_STATUS_COMPLETE);
	expect_rx(0, 4, UART_RX_RDY, rx_buffers[0][1], 32);
	expect_rx(0, 5, UART_RX_BUF_RELEASED, rx_buffers[0][1], 0);
	expect_rx(0, 6, UART_RX_DISABLED, NULL, 0);
	zassert_equal(captures[0].count, 7);
}

ZTEST(wch_uart_rx, test_partial_disable_and_unused_buffer_release)
{
	begin_rx(0);
	zassert_ok(uart_rx_buf_rsp(ports[0], rx_buffers[0][1], 32));
	dma_channels[4].pending = 25;
	zassert_ok(uart_rx_disable(ports[0]));
	expect_rx(0, 1, UART_RX_RDY, rx_buffers[0][0], 7);
	expect_rx(0, 2, UART_RX_BUF_RELEASED, rx_buffers[0][0], 0);
	expect_rx(0, 3, UART_RX_BUF_RELEASED, rx_buffers[0][1], 0);
	expect_rx(0, 4, UART_RX_DISABLED, NULL, 0);
	zassert_equal(uart_rx_disable(ports[0]), -EFAULT);
	rx_event(0, -EIO);
	zassert_equal(captures[0].count, 5);
}

ZTEST(wch_uart_rx, test_empty_disable_has_no_ready)
{
	begin_rx(0);
	zassert_ok(uart_rx_disable(ports[0]));
	expect_rx(0, 1, UART_RX_BUF_RELEASED, rx_buffers[0][0], 0);
	expect_rx(0, 2, UART_RX_DISABLED, NULL, 0);
}

ZTEST(wch_uart_rx, test_initial_config_and_start_failure_no_loan)
{
	configure_result = -EINVAL;
	zassert_equal(uart_rx_enable(ports[0], buffer, 32, -1), -EINVAL);
	zassert_equal(captures[0].count, 0);
	configure_result = 0;
	start_result = -EIO;
	zassert_equal(uart_rx_enable(ports[0], buffer, 32, -1), -EIO);
	zassert_false(dma_channels[4].running);
	zassert_equal(captures[0].count, 0);
	zassert_equal(uart_data[0].rx.phase, WCH_RX_IDLE);
	start_result = 0;
	begin_rx(0);
}

ZTEST(wch_uart_rx, test_handoff_failure_releases_both_loans)
{
	begin_rx(0);
	zassert_ok(uart_rx_buf_rsp(ports[0], rx_buffers[0][1], 32));
	start_result = -EIO;
	rx_event(0, DMA_STATUS_COMPLETE);
	expect_rx(0, 1, UART_RX_RDY, rx_buffers[0][0], 32);
	expect_rx(0, 2, UART_RX_BUF_RELEASED, rx_buffers[0][0], 0);
	expect_rx(0, 3, UART_RX_STOPPED, NULL, 0);
	expect_rx(0, 4, UART_RX_BUF_RELEASED, rx_buffers[0][1], 0);
	expect_rx(0, 5, UART_RX_DISABLED, NULL, 0);
	zassert_equal(captures[0].count, 6);
}

ZTEST(wch_uart_rx, test_error_priority_and_late_dma_irq)
{
	begin_rx(0);
	dma_channels[4].pending = 27;
	registers[0].STATR |= USART_STATR_ORE | USART_STATR_FE;
	usart_wch_isr(ports[0]);
	expect_rx(0, 1, UART_RX_STOPPED, NULL, 0);
	zassert_equal(captures[0].events[1].data.rx_stop.reason,
		      UART_ERROR_OVERRUN | UART_ERROR_FRAMING);
	zassert_equal(captures[0].events[1].data.rx_stop.data.len, 5);
	expect_rx(0, 2, UART_RX_RDY, rx_buffers[0][0], 5);
	expect_rx(0, 3, UART_RX_BUF_RELEASED, rx_buffers[0][0], 0);
	expect_rx(0, 4, UART_RX_DISABLED, NULL, 0);
	rx_event(0, DMA_STATUS_COMPLETE);
	zassert_equal(captures[0].count, 5);
}

ZTEST(wch_uart_rx, test_dma_error_and_unknown_count)
{
	begin_rx(0);
	dma_channels[4].pending = 30;
	rx_event(0, -EIO);
	expect_rx(0, 1, UART_RX_STOPPED, NULL, 0);
	expect_rx(0, 2, UART_RX_RDY, rx_buffers[0][0], 2);
	zassert_equal(captures[0].events[1].data.rx_stop.reason, UART_ERROR_OVERRUN);
	captures[0].count = 0;
	begin_rx(0);
	status_result = -EIO;
	zassert_ok(uart_rx_disable(ports[0]));
	expect_rx(0, 1, UART_RX_STOPPED, NULL, 0);
	zassert_equal(captures[0].events[1].data.rx_stop.data.len, 0);
	expect_rx(0, 2, UART_RX_BUF_RELEASED, rx_buffers[0][0], 0);
	expect_rx(0, 3, UART_RX_DISABLED, NULL, 0);
}

static void reentrant_rx(const struct device *dev, struct uart_event *event, void *user)
{
	on_event(dev, event, user);
	if (event->type == UART_RX_BUF_REQUEST) {
		response_result = uart_rx_buf_rsp(dev, rx_buffers[0][1], 32);
	} else if (event->type == UART_RX_RDY && disable_on_ready) {
		disable_on_ready = false;
		response_result = uart_rx_disable(dev);
	} else if (event->type == UART_RX_DISABLED && restart_on_disabled) {
		restart_on_disabled = false;
		response_result = uart_rx_enable(dev, rx_buffers[0][0], 32, -1);
	}
}

ZTEST(wch_uart_rx, test_callback_response_disable_and_restart)
{
	disable_on_ready = true;
	restart_on_disabled = true;
	zassert_ok(uart_callback_set(ports[0], reentrant_rx, &captures[0]));
	begin_rx(0);
	zassert_ok(response_result);
	rx_event(0, DMA_STATUS_COMPLETE);
	zassert_ok(response_result);
	/* Stop from old RDY suppresses the queued new-buffer request. Old
	 * releases and DISABLED precede any callback for the restarted session.
	 */
	expect_rx(0, 1, UART_RX_RDY, rx_buffers[0][0], 32);
	expect_rx(0, 2, UART_RX_BUF_RELEASED, rx_buffers[0][0], 0);
	expect_rx(0, 3, UART_RX_BUF_RELEASED, rx_buffers[0][1], 0);
	expect_rx(0, 4, UART_RX_DISABLED, NULL, 0);
	expect_rx(0, 5, UART_RX_BUF_REQUEST, NULL, 0);
	zassert_equal(captures[0].count, 6);
	zassert_equal(uart_data[0].rx.phase, WCH_RX_ACTIVE);
	zassert_ok(uart_rx_disable(ports[0]));
}

ZTEST(wch_uart_rx, test_two_ports_full_duplex_independence)
{
	begin_rx(0);
	begin_rx(1);
	start(0, 32);
	start(1, 32);
	zassert_ok(uart_rx_disable(ports[0]));
	zassert_true(registers[0].CTLR3 & USART_CTLR3_DMAT);
	zassert_true(registers[1].CTLR3 & USART_CTLR3_DMAR);
	dma_event(1, DMA_STATUS_COMPLETE);
	tc_event(1);
	zassert_true(registers[1].CTLR3 & USART_CTLR3_DMAR);
	rx_event(1, DMA_STATUS_COMPLETE);
	zassert_equal(uart_data[1].rx.phase, WCH_RX_IDLE);
	zassert_equal(uart_data[0].tx.phase, WCH_TX_DMA);
}

static void completion_during_request(const struct device *dev, struct uart_event *event,
				      void *user)
{
	on_event(dev, event, user);
	if (event->type == UART_RX_BUF_REQUEST) {
		zassert_ok(uart_rx_buf_rsp(dev, rx_buffers[0][1], 32));
		/* Both DMA completions interrupt a still-running callback. No new
		 * request can be delivered before the first buffer release.
		 */
		rx_event(0, DMA_STATUS_COMPLETE);
		rx_event(0, DMA_STATUS_COMPLETE);
	}
}

ZTEST(wch_uart_rx, test_completions_during_callback_drop_stale_request)
{
	zassert_ok(uart_callback_set(ports[0], completion_during_request, &captures[0]));
	zassert_ok(uart_rx_enable(ports[0], rx_buffers[0][0], 32, -1));
	expect_rx(0, 0, UART_RX_BUF_REQUEST, NULL, 0);
	expect_rx(0, 1, UART_RX_RDY, rx_buffers[0][0], 32);
	expect_rx(0, 2, UART_RX_BUF_RELEASED, rx_buffers[0][0], 0);
	expect_rx(0, 3, UART_RX_RDY, rx_buffers[0][1], 32);
	expect_rx(0, 4, UART_RX_BUF_RELEASED, rx_buffers[0][1], 0);
	expect_rx(0, 5, UART_RX_DISABLED, NULL, 0);
	zassert_equal(captures[0].count, 6);
	zassert_equal(uart_data[0].rx.count, 0);
}

ZTEST(wch_uart_rx, test_uart_error_beats_dma_complete_and_handoff)
{
	begin_rx(0);
	zassert_ok(uart_rx_buf_rsp(ports[0], rx_buffers[0][1], 32));
	registers[0].STATR |= USART_STATR_PE | USART_STATR_NE;
	rx_event(0, DMA_STATUS_COMPLETE);
	expect_rx(0, 1, UART_RX_STOPPED, NULL, 0);
	zassert_equal(captures[0].events[1].data.rx_stop.reason,
		      UART_ERROR_PARITY | UART_ERROR_NOISE);
	expect_rx(0, 2, UART_RX_RDY, rx_buffers[0][0], 32);
	expect_rx(0, 3, UART_RX_BUF_RELEASED, rx_buffers[0][0], 0);
	expect_rx(0, 4, UART_RX_BUF_RELEASED, rx_buffers[0][1], 0);
	expect_rx(0, 5, UART_RX_DISABLED, NULL, 0);
	zassert_false(dma_channels[4].running);
}

ZTEST(wch_uart_rx, test_maximum_length_and_invalid_progress)
{
	zassert_ok(uart_rx_enable(ports[0], buffer, UINT16_MAX, -1));
	zassert_equal(dma_channels[4].block.block_size, UINT16_MAX);
	dma_channels[4].pending = UINT16_MAX + 1U;
	zassert_ok(uart_rx_disable(ports[0]));
	expect_rx(0, 1, UART_RX_STOPPED, NULL, 0);
	expect_rx(0, 2, UART_RX_BUF_RELEASED, buffer, 0);
	expect_rx(0, 3, UART_RX_DISABLED, NULL, 0);
	zassert_equal(captures[0].events[1].data.rx_stop.data.len, 0);
}

ZTEST(wch_uart_rx, test_wrong_channel_and_controller_ignored)
{
	begin_rx(0);
	usart_wch_rx_dma_callback(DEVICE_GET(mock_dma), (void *)ports[0], 5, -EIO);
	usart_wch_rx_dma_callback(DEVICE_GET(mock_clock), (void *)ports[0], 4, -EIO);
	zassert_equal(captures[0].count, 1);
	zassert_true(dma_channels[4].running);
}

ZTEST_SUITE(wch_uart_rx, NULL, NULL, before, cleanup, NULL);
