/* SPDX-License-Identifier: Apache-2.0 */
#include <zephyr/drivers/uart.h>
#include <zephyr/ztest.h>
#include <hal_ch32fun.h>

static const struct device *const ports[] = {
	DEVICE_DT_GET(DT_ALIAS(test_uart0)),
	DEVICE_DT_GET(DT_ALIAS(test_uart1)),
};
static uint8_t payloads[2][128];
struct result {
	struct k_sem done;
	struct uart_event event;
	unsigned int count;
};
static struct result results[2];
volatile struct {
	uint32_t magic, stage;
	struct {
		uint32_t done, aborted, bytes, last_length, last_type, tc_seen, bad_buffer;
	} port[2];
} wch_tx_hil_diag;

static void callback(const struct device *dev, struct uart_event *event, void *user)
{
	struct result *result = user;

	ARG_UNUSED(dev);
	size_t port = result - results;
	wch_tx_hil_diag.port[port].last_type = event->type;
	if (event->type == UART_TX_DONE || event->type == UART_TX_ABORTED) {
		wch_tx_hil_diag.port[port].last_length = event->data.tx.len;
		wch_tx_hil_diag.port[port].bytes += event->data.tx.len;
		wch_tx_hil_diag.port[port].bad_buffer += event->data.tx.buf != payloads[port];
		if (event->type == UART_TX_DONE) {
			wch_tx_hil_diag.port[port].done++;
			wch_tx_hil_diag.port[port].tc_seen +=
				((port ? USART2 : USART1)->STATR & USART_STATR_TC) != 0;
		} else {
			wch_tx_hil_diag.port[port].aborted++;
		}
	}
	result->event = *event;
	result->count++;
	k_sem_give(&result->done);
}

static void cleanup(void *fixture)
{
	ARG_UNUSED(fixture);
	ARRAY_FOR_EACH(ports, i) {
		(void)uart_tx_abort(ports[i]);
		(void)uart_callback_set(ports[i], NULL, NULL);
	}
}

ZTEST(wch_uart_tx_hardware, test_two_port_tx_completion)
{
	wch_tx_hil_diag.magic = 0x57545848;
	wch_tx_hil_diag.stage = 1;
	ARRAY_FOR_EACH(ports, i) {
		zassert_true(device_is_ready(ports[i]));
		k_sem_init(&results[i].done, 0, 1);
		zassert_ok(uart_callback_set(ports[i], callback, &results[i]));
		for (size_t j = 0; j < sizeof(payloads[i]); j++) {
			payloads[i][j] = (uint8_t)(j ^ (i ? 0xa5 : 0x5a));
		}
	}
	const size_t lengths[] = {1, 128};

	ARRAY_FOR_EACH(lengths, n) {
		ARRAY_FOR_EACH(ports, i) {
			results[i].count = 0;
			k_sem_reset(&results[i].done);
			zassert_ok(uart_tx(ports[i], payloads[i], lengths[n], 100000));
		}
		ARRAY_FOR_EACH(ports, i) {
			zassert_ok(k_sem_take(&results[i].done, K_SECONDS(1)), "Missing TX event");
			zassert_equal(results[i].count, 1);
			zassert_equal(results[i].event.type, UART_TX_DONE);
			zassert_equal(results[i].event.data.tx.len, lengths[n]);
			zassert_equal_ptr(results[i].event.data.tx.buf, payloads[i]);
			zassert_equal(uart_tx_abort(ports[i]), -EFAULT);
		}
	}
	ARRAY_FOR_EACH(ports, i) {
		zassert_equal(wch_tx_hil_diag.port[i].tc_seen, ARRAY_SIZE(lengths));
	}
	wch_tx_hil_diag.stage = 2;
}

/* No RX loopback is claimed. Capture PA9/PA2 externally to verify bytes/baud. */
ZTEST_SUITE(wch_uart_tx_hardware, NULL, NULL, NULL, cleanup, NULL);
