/* SPDX-License-Identifier: Apache-2.0 */
#include <zephyr/ztest.h>
#include "uart_wch_usart.c"

/* Fault-injectable DMA provider. Allocation and UART/DMA API wrappers are
 * real Zephyr code; no model transfers bytes or infers on-wire completion.
 */
struct mock_channel {
	struct dma_config config;
	struct dma_block_config block;
	uint32_t pending;
	bool running;
};
static struct mock_channel dma_channels[7];
ATOMIC_DEFINE(dma_bits, 7);
static struct dma_context dma_context = {
	.magic = DMA_MAGIC,
	.dma_channels = 7,
	.atomic = dma_bits,
};
static int configure_result;
static int start_result;
static int status_result;
static int clock_result;
static unsigned int irq_setup_count;

static bool mock_filter(const struct device *dev, int channel, void *param)
{
	ARG_UNUSED(dev);
	return param == NULL || *(uint32_t *)param == (uint32_t)channel;
}

static int mock_configure(const struct device *dev, uint32_t channel, struct dma_config *config)
{
	ARG_UNUSED(dev);
	zassert_true(channel < ARRAY_SIZE(dma_channels));
	const struct device *uart = config->user_data;
	const struct usart_wch_config *uart_config = uart->config;

	zassert_false(uart_config->regs->CTLR3 & USART_CTLR3_DMAT);
	if (configure_result != 0) {
		return configure_result;
	}
	dma_channels[channel].config = *config;
	dma_channels[channel].block = *config->head_block;
	dma_channels[channel].pending = config->head_block->block_size;
	return 0;
}

static int mock_start(const struct device *dev, uint32_t channel)
{
	ARG_UNUSED(dev);
	/* Model even a provider which partially enabled DMA before failing. */
	dma_channels[channel].running = true;
	return start_result;
}

static int mock_stop(const struct device *dev, uint32_t channel)
{
	ARG_UNUSED(dev);
	const struct device *uart = dma_channels[channel].config.user_data;

	if (uart != NULL) {
		const struct usart_wch_config *config = uart->config;

		zassert_false(config->regs->CTLR3 & USART_CTLR3_DMAT,
			      "Gate UART requests before stopping DMA");
	}
	dma_channels[channel].running = false;
	return 0;
}

static int mock_status(const struct device *dev, uint32_t channel, struct dma_status *status)
{
	ARG_UNUSED(dev);
	if (status_result != 0) {
		return status_result;
	}
	*status = (struct dma_status){
		.pending_length = dma_channels[channel].pending,
		.busy = dma_channels[channel].running,
	};
	return 0;
}

static DEVICE_API(dma, mock_dma_api) = {
	.config = mock_configure,
	.start = mock_start,
	.stop = mock_stop,
	.get_status = mock_status,
	.chan_filter = mock_filter,
};
DEVICE_DEFINE(mock_dma, "mock_dma", NULL, NULL, &dma_context, NULL, PRE_KERNEL_1, 0, &mock_dma_api);

static int clock_on(const struct device *dev, clock_control_subsys_t subsys)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(subsys);
	return clock_result;
}

static int clock_rate(const struct device *dev, clock_control_subsys_t subsys, uint32_t *rate)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(subsys);
	*rate = 144000000;
	return 0;
}

static DEVICE_API(clock_control, mock_clock_api) = {.on = clock_on, .get_rate = clock_rate};
DEVICE_DEFINE(mock_clock, "mock_clock", NULL, NULL, NULL, NULL, PRE_KERNEL_1, 0, &mock_clock_api);

static void irq_setup(const struct device *dev)
{
	ARG_UNUSED(dev);
	irq_setup_count++;
}

static USART_TypeDef registers[2];
static struct usart_wch_data uart_data[2];
static const struct usart_wch_config configs[2] = {
	{.regs = &registers[0],
	 .clock_dev = DEVICE_GET(mock_clock),
	 .current_speed = 3000000,
	 .parity = UART_CFG_PARITY_NONE,
	 .stop_bits = UART_CFG_STOP_BITS_1,
	 .data_bits = 8,
	 .dma_tx_dev = DEVICE_GET(mock_dma),
	 .dma_rx_dev = DEVICE_GET(mock_dma),
	 .dma_tx_channel = 3,
	 .dma_rx_channel = 4,
	 .irq_config_func = irq_setup},
	{.regs = &registers[1],
	 .clock_dev = DEVICE_GET(mock_clock),
	 .current_speed = 3000000,
	 .parity = UART_CFG_PARITY_NONE,
	 .stop_bits = UART_CFG_STOP_BITS_1,
	 .data_bits = 8,
	 .dma_tx_dev = DEVICE_GET(mock_dma),
	 .dma_rx_dev = DEVICE_GET(mock_dma),
	 .dma_tx_channel = 6,
	 .dma_rx_channel = 5,
	 .irq_config_func = irq_setup},
};
DEVICE_DEFINE(uart0, "uart0", NULL, NULL, &uart_data[0], &configs[0], POST_KERNEL, 50,
	      &usart_wch_driver_api);
DEVICE_DEFINE(uart1, "uart1", NULL, NULL, &uart_data[1], &configs[1], POST_KERNEL, 50,
	      &usart_wch_driver_api);
static const struct device *const ports[] = {DEVICE_GET(uart0), DEVICE_GET(uart1)};
static uint8_t buffer[UINT16_MAX + 1];
static uint8_t next_buffer[8];
struct capture {
	struct uart_event events[8];
	size_t count;
	struct k_sem notified;
	bool restart;
	int restart_result;
};
static struct capture captures[2];

static void on_event(const struct device *dev, struct uart_event *event, void *user)
{
	struct capture *capture = user;

	zassert_true(capture->count < ARRAY_SIZE(capture->events));
	capture->events[capture->count++] = *event;
	if (capture->restart && capture->count == 1) {
		capture->restart_result = uart_tx(dev, next_buffer, sizeof(next_buffer), 100000);
	}
	k_sem_give(&capture->notified);
}

static void cleanup(void *fixture)
{
	ARG_UNUSED(fixture);
	ARRAY_FOR_EACH(ports, i) {
		if (uart_data[i].tx.reserved) {
			captures[i].restart = false;
			(void)uart_tx_abort(ports[i]);
			struct k_work_sync sync;

			(void)k_work_cancel_delayable_sync(&uart_data[i].tx.timeout_work, &sync);
		}
	}
}

static void before(void *fixture)
{
	ARG_UNUSED(fixture);
	memset(dma_channels, 0, sizeof(dma_channels));
	memset(dma_bits, 0, sizeof(dma_bits));
	memset(registers, 0, sizeof(registers));
	memset(uart_data, 0, sizeof(uart_data));
	memset(captures, 0, sizeof(captures));
	configure_result = 0;
	start_result = 0;
	status_result = 0;
	clock_result = 0;
	pinctrl_result = 0;
	irq_setup_count = 0;
	ARRAY_FOR_EACH(ports, i) {
		k_sem_init(&captures[i].notified, 0, 8);
		zassert_ok(usart_wch_init(ports[i]));
		registers[i].STATR = USART_STATR_TC | USART_STATR_TXE;
		zassert_ok(uart_callback_set(ports[i], on_event, &captures[i]));
	}
}

static void dma_event(unsigned int port, int status)
{
	uint32_t channel = configs[port].dma_tx_channel;
	struct mock_channel *dma = &dma_channels[channel];

	if (status == DMA_STATUS_COMPLETE) {
		dma->pending = 0;
		dma->running = false;
	}
	/* Real WCH DMA callbacks also retain an outer IRQ exclusion scope. */
	unsigned int key = irq_lock();

	dma->config.dma_callback(DEVICE_GET(mock_dma), dma->config.user_data, channel, status);
	irq_unlock(key);
}

static void tc_event(unsigned int port)
{
	registers[port].STATR |= USART_STATR_TC;
	usart_wch_isr(ports[port]);
}

static void start(unsigned int port, size_t length)
{
	zassert_ok(uart_tx(ports[port], buffer, length, SYS_FOREVER_US));
	/* The plain RAM register model cannot apply W0C automatically. Verify
	 * that production wrote only TC=0, then restore the modeled real flags.
	 */
	zassert_equal(registers[port].STATR, (uint16_t)~USART_STATR_TC);
	registers[port].STATR = USART_STATR_TXE;
}

ZTEST(wch_uart_tx, test_init_reserves_all_four_channels)
{
	zassert_equal(irq_setup_count, 2);
	for (uint32_t channel = 3; channel <= 6; channel++) {
		zassert_equal(dma_request_channel(DEVICE_GET(mock_dma), &channel), -EINVAL);
	}
	zassert_equal(registers[0].BRR, 48);
	zassert_equal(registers[1].BRR, 48);
}

ZTEST(wch_uart_tx, test_rx_claim_failure_rolls_back_tx)
{
	dma_release_channel(DEVICE_GET(mock_dma), 3);
	struct usart_wch_data scratch = {0};
	const struct device candidate = {.data = &scratch, .config = &configs[0]};

	zassert_equal(usart_wch_init(&candidate), -EBUSY);
	uint32_t channel = 3;

	zassert_equal(dma_request_channel(DEVICE_GET(mock_dma), &channel), 3);
	zassert_false(scratch.tx.reserved);
}

ZTEST(wch_uart_tx, test_init_failures_before_reservation)
{
	struct usart_wch_data scratch = {0};
	struct usart_wch_config config = configs[0];
	USART_TypeDef regs = {0};
	const struct device candidate = {.data = &scratch, .config = &config};

	config.regs = &regs;
	clock_result = -EIO;
	zassert_equal(usart_wch_init(&candidate), -EIO);
	clock_result = 0;
	pinctrl_result = -EINVAL;
	zassert_equal(usart_wch_init(&candidate), -EINVAL);
	pinctrl_result = 0;
	config.data_bits = 9;
	zassert_equal(usart_wch_init(&candidate), -ENOTSUP);
	zassert_false(scratch.tx.reserved);
	zassert_equal(regs.CTLR1, 0);
}

ZTEST(wch_uart_tx, test_validate_and_busy)
{
	zassert_equal(uart_tx(ports[0], NULL, 1, -1), -EINVAL);
	zassert_equal(uart_tx(ports[0], buffer, 0, -1), -EINVAL);
	zassert_equal(uart_tx(ports[0], buffer, UINT16_MAX + 1, -1), -EMSGSIZE);
	zassert_equal(uart_tx(ports[0], buffer, 1, -2), -EINVAL);
	zassert_ok(uart_callback_set(ports[0], NULL, NULL));
	zassert_equal(uart_tx(ports[0], buffer, 1, -1), -EACCES);
	zassert_ok(uart_callback_set(ports[0], on_event, &captures[0]));
	start(0, UINT16_MAX);
	zassert_equal(uart_tx(ports[0], buffer, 1, -1), -EBUSY);
	zassert_equal(uart_callback_set(ports[0], NULL, NULL), -EBUSY);
	zassert_equal(captures[0].count, 0);
}

ZTEST(wch_uart_tx, test_dma_descriptor_and_usart_tc_boundary)
{
	start(0, 32);
	struct mock_channel *dma = &dma_channels[3];

	zassert_equal(dma->config.channel_direction, MEMORY_TO_PERIPHERAL);
	zassert_equal(dma->config.source_data_size, 1);
	zassert_equal(dma->block.source_address, (uintptr_t)buffer);
	zassert_equal(dma->block.dest_address, (uintptr_t)&registers[0].DATAR);
	zassert_equal(dma->block.dest_addr_adj, DMA_ADDR_ADJ_NO_CHANGE);
	zassert_true(registers[0].CTLR3 & USART_CTLR3_DMAT);
	dma_event(0, DMA_STATUS_COMPLETE);
	zassert_equal(captures[0].count, 0);
	zassert_equal(uart_data[0].tx.phase, WCH_TX_WAIT_TC);
	zassert_false(registers[0].CTLR3 & USART_CTLR3_DMAT);
	tc_event(0);
	zassert_equal(captures[0].count, 1);
	zassert_equal(captures[0].events[0].type, UART_TX_DONE);
	zassert_equal(captures[0].events[0].data.tx.len, 32);
	zassert_equal_ptr(captures[0].events[0].data.tx.buf, buffer);
	tc_event(0);
	dma_event(0, DMA_STATUS_COMPLETE);
	zassert_equal(captures[0].count, 1);
}

ZTEST(wch_uart_tx, test_delayed_dma_irq_and_early_uart_irq)
{
	start(0, 1);
	tc_event(0); /* No completion until DMA also reports completion. */
	zassert_equal(captures[0].count, 0);
	dma_event(0, DMA_STATUS_COMPLETE);
	zassert_equal(captures[0].count, 1);
	zassert_equal(captures[0].events[0].type, UART_TX_DONE);
}

ZTEST(wch_uart_tx, test_setup_failure_rolls_back_without_event)
{
	configure_result = -EIO;
	zassert_equal(uart_tx(ports[0], buffer, 32, 100000), -EIO);
	configure_result = 0;
	start_result = -EIO;
	zassert_equal(uart_tx(ports[0], buffer, 32, 100000), -EIO);
	zassert_false(dma_channels[3].running);
	zassert_false(registers[0].CTLR3 & USART_CTLR3_DMAT);
	zassert_equal(uart_data[0].tx.phase, WCH_TX_IDLE);
	zassert_equal(captures[0].count, 0);
	start_result = 0;
	start(0, 32);
}

ZTEST(wch_uart_tx, test_abort_progress_drain_and_late_interrupts)
{
	start(0, 32);
	dma_channels[3].pending = 12;
	zassert_ok(uart_tx_abort(ports[0]));
	zassert_equal(captures[0].events[0].type, UART_TX_ABORTED);
	zassert_equal(captures[0].events[0].data.tx.len, 20);
	zassert_equal(uart_tx_abort(ports[0]), -EFAULT);
	zassert_equal(uart_tx(ports[0], next_buffer, 8, -1), -EBUSY);
	dma_event(0, DMA_STATUS_COMPLETE);
	tc_event(0);
	zassert_equal(captures[0].count, 1);
	start(0, 8);
}

ZTEST(wch_uart_tx, test_abort_wait_tc_and_zero_progress)
{
	start(0, 32);
	zassert_ok(uart_tx_abort(ports[0]));
	zassert_equal(captures[0].events[0].data.tx.len, 0);
	start(0, 32); /* No byte was queued, so no tail needs draining. */
	dma_event(0, DMA_STATUS_COMPLETE);
	zassert_ok(uart_tx_abort(ports[0]));
	zassert_equal(captures[0].events[1].type, UART_TX_ABORTED);
	zassert_equal(captures[0].events[1].data.tx.len, 32);
}

ZTEST(wch_uart_tx, test_dma_error_beats_uart_tc)
{
	start(0, 32);
	dma_channels[3].pending = 16;
	registers[0].STATR |= USART_STATR_TC;
	dma_event(0, -EIO);
	zassert_equal(captures[0].count, 1);
	zassert_equal(captures[0].events[0].type, UART_TX_ABORTED);
	zassert_equal(captures[0].events[0].data.tx.len, 16);
	tc_event(0);
	zassert_equal(captures[0].count, 1);
}

ZTEST(wch_uart_tx, test_callback_can_start_next_tx)
{
	captures[0].restart = true;
	start(0, 32);
	dma_event(0, DMA_STATUS_COMPLETE);
	tc_event(0);
	zassert_equal(captures[0].restart_result, 0);
	zassert_equal(uart_data[0].tx.phase, WCH_TX_DMA);
	zassert_equal_ptr(uart_data[0].tx.buffer, next_buffer);
	zassert_true(registers[0].CTLR3 & USART_CTLR3_DMAT);
	registers[0].STATR = USART_STATR_TXE;
	dma_event(0, DMA_STATUS_COMPLETE);
	tc_event(0);
	zassert_equal(captures[0].count, 2);
	zassert_equal_ptr(captures[0].events[1].data.tx.buf, next_buffer);
}

ZTEST(wch_uart_tx, test_independent_ports)
{
	start(0, 32);
	start(1, 16);
	dma_event(1, DMA_STATUS_COMPLETE);
	tc_event(1);
	zassert_equal(captures[0].count, 0);
	zassert_equal(captures[1].count, 1);
	zassert_equal(uart_data[0].tx.phase, WCH_TX_DMA);
	zassert_ok(uart_tx_abort(ports[0]));
	zassert_equal(captures[0].events[0].type, UART_TX_ABORTED);
}

ZTEST(wch_uart_tx, test_real_timeout_and_reentry)
{
	captures[0].restart = true;
	zassert_ok(uart_tx(ports[0], buffer, 32, 1000));
	registers[0].STATR = USART_STATR_TXE;
	zassert_ok(k_sem_take(&captures[0].notified, K_SECONDS(1)));
	zassert_equal(captures[0].events[0].type, UART_TX_ABORTED);
	zassert_equal(captures[0].restart_result, 0);
	zassert_equal_ptr(uart_data[0].tx.buffer, next_buffer);
	zassert_equal(captures[0].count, 1);
}

ZTEST(wch_uart_tx, test_stale_work_checks_new_deadline)
{
	zassert_ok(uart_tx(ports[0], buffer, 32, 100000));
	zassert_ok(uart_tx_abort(ports[0]));
	zassert_ok(uart_tx(ports[0], next_buffer, 8, 100000));
	usart_wch_tx_timeout(&uart_data[0].tx.timeout_work.work);
	zassert_equal(captures[0].count, 1);
	zassert_equal(uart_data[0].tx.phase, WCH_TX_DMA);
	/* A new forever operation also ignores the old worker. */
	zassert_ok(uart_tx_abort(ports[0]));
	start(0, 8);
	usart_wch_tx_timeout(&uart_data[0].tx.timeout_work.work);
	zassert_equal(captures[0].count, 2);
	zassert_equal(uart_data[0].tx.phase, WCH_TX_DMA);
}

ZTEST(wch_uart_tx, test_timeout_after_completion_is_noop)
{
	zassert_ok(uart_tx(ports[0], buffer, 32, 100000));
	registers[0].STATR = USART_STATR_TXE;
	dma_event(0, DMA_STATUS_COMPLETE);
	tc_event(0);
	usart_wch_tx_timeout(&uart_data[0].tx.timeout_work.work);
	zassert_equal(captures[0].count, 1);
	zassert_equal(captures[0].events[0].type, UART_TX_DONE);
}

ZTEST(wch_uart_tx, test_async_rx_is_explicitly_unsupported)
{
	zassert_equal(uart_rx_enable(ports[0], buffer, 32, -1), -ENOTSUP);
	zassert_equal(uart_rx_buf_rsp(ports[0], buffer, 32), -ENOTSUP);
	zassert_equal(uart_rx_disable(ports[0]), -ENOTSUP);
}

ZTEST(wch_uart_tx, test_polling_boundary_and_rx)
{
	uart_poll_out(ports[0], 0x5a);
	zassert_equal(registers[0].DATAR, 0x5a);
	/* Model the STATR-read/DATAR-write clearing TC on real hardware. */
	registers[0].STATR = USART_STATR_TXE;
	zassert_equal(uart_tx(ports[0], buffer, 32, -1), -EBUSY);
	tc_event(0);
	start(0, 32);
	unsigned char value;

	zassert_equal(uart_poll_in(ports[0], &value), -1);
	registers[0].DATAR = 0xa5;
	registers[0].STATR |= USART_STATR_RXNE;
	zassert_ok(uart_poll_in(ports[0], &value));
	zassert_equal(value, 0xa5);
}

ZTEST(wch_uart_tx, test_unknown_progress_fails_closed)
{
	start(0, 32);
	status_result = -EIO;
	zassert_ok(uart_tx_abort(ports[0]));
	zassert_equal(captures[0].events[0].data.tx.len, 0);
	zassert_equal(uart_tx(ports[0], buffer, 8, -1), -EBUSY);
	status_result = 0;
	tc_event(0);
	start(0, 32);
	dma_channels[3].pending = 33; /* Corrupt status must not underflow count. */
	zassert_ok(uart_tx_abort(ports[0]));
	zassert_equal(captures[0].events[1].data.tx.len, 0);
	zassert_equal(uart_tx(ports[0], buffer, 8, -1), -EBUSY);
}

ZTEST(wch_uart_tx, test_timeout_waiting_for_tc)
{
	start(0, 32);
	dma_event(0, DMA_STATUS_COMPLETE);
	uart_data[0].tx.deadline = -1; /* Expired even when this test runs at boot. */
	usart_wch_tx_timeout(&uart_data[0].tx.timeout_work.work);
	zassert_equal(captures[0].count, 1);
	zassert_equal(captures[0].events[0].type, UART_TX_ABORTED);
	zassert_equal(captures[0].events[0].data.tx.len, 32);
	tc_event(0);
	zassert_equal(captures[0].count, 1);
}

ZTEST(wch_uart_tx, test_nonterminal_and_wrong_channel_callbacks)
{
	start(0, 32);
	usart_wch_tx_dma_callback(DEVICE_GET(mock_dma), (void *)ports[0], 4, DMA_STATUS_COMPLETE);
	dma_event(0, DMA_STATUS_HALF_COMPLETE);
	zassert_equal(captures[0].count, 0);
	zassert_equal(uart_data[0].tx.phase, WCH_TX_DMA);
	zassert_true(registers[0].CTLR3 & USART_CTLR3_DMAT);
}

ZTEST_SUITE(wch_uart_tx, NULL, NULL, before, cleanup, NULL);
