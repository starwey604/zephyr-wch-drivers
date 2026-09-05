/* SPDX-License-Identifier: Apache-2.0 */
#include <zephyr/drivers/uart.h>
#include <zephyr/ztest.h>

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

static void callback(const struct device *dev, struct uart_event *event, void *user)
{
	struct result *result = user;

	ARG_UNUSED(dev);
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
}

/* No RX loopback is claimed. Capture PA9/PA2 externally to verify bytes/baud. */
ZTEST_SUITE(wch_uart_tx_hardware, NULL, NULL, NULL, cleanup, NULL);
