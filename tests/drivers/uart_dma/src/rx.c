/* SPDX-License-Identifier: Apache-2.0 */
#include <zephyr/drivers/uart.h>
#include <zephyr/ztest.h>

/* Physical local loopbacks required: UART1 TX->RX and UART2 TX->RX.
 * Short, paced packets test ownership/data, NOT lossless streaming or baud.
 */
static const struct device *const ports[] = {
	DEVICE_DT_GET(DT_ALIAS(test_uart0)),
	DEVICE_DT_GET(DT_ALIAS(test_uart1)),
};
static uint8_t payload[2][64];
static uint8_t buffers[2][2][32];
struct rx_result {
	struct k_sem tx_done;
	struct k_sem rx_ready;
	struct k_sem disabled;
	size_t received;
	unsigned int released;
	unsigned int requests;
	int error;
	unsigned int port;
	bool queue_next;
};
static struct rx_result results[2];
/* Debugger-readable milestones: 0 not started, 1 running, 2 passed. Failure
 * also enters ztest_test_fail; a build alone leaves this marker unexecuted.
 */
volatile unsigned int wch_rx_hil_stage;

static void callback(const struct device *dev, struct uart_event *event, void *user)
{
	struct rx_result *result = user;

	switch (event->type) {
	case UART_TX_DONE:
		k_sem_give(&result->tx_done);
		break;
	case UART_TX_ABORTED:
	case UART_RX_STOPPED:
		result->error = -EIO;
		break;
	case UART_RX_BUF_REQUEST:
		if (result->queue_next && result->requests == 0) {
			result->error = uart_rx_buf_rsp(dev, buffers[result->port][1], 32);
		}
		result->requests++;
		break;
	case UART_RX_RDY:
		if (event->data.rx.offset != 0) {
			result->error = -EINVAL;
		}
		result->received += event->data.rx.len;
		k_sem_give(&result->rx_ready);
		break;
	case UART_RX_BUF_RELEASED:
		result->released++;
		break;
	case UART_RX_DISABLED:
		k_sem_give(&result->disabled);
		break;
	default:
		break;
	}
}

static void cleanup(void *fixture)
{
	ARG_UNUSED(fixture);
	ARRAY_FOR_EACH(ports, i) {
		(void)uart_rx_disable(ports[i]);
		(void)uart_tx_abort(ports[i]);
		(void)uart_callback_set(ports[i], NULL, NULL);
	}
}

ZTEST(wch_uart_rx_hardware, test_paced_dual_local_loopback)
{
	wch_rx_hil_stage = 1;
	/* Full 1-byte, full 32-byte, partial 7-byte, then 32+7 handoff/disable. */
	const size_t first_lengths[] = {1, 32, 7, 32};

	ARRAY_FOR_EACH(first_lengths, round) {
		bool chain = round == 3;
		size_t capacity = round == 0 ? 1 : 32;

		ARRAY_FOR_EACH(ports, i) {
			zassert_true(device_is_ready(ports[i]));
			results[i] = (struct rx_result){.port = i, .queue_next = chain};
			k_sem_init(&results[i].tx_done, 0, 1);
			k_sem_init(&results[i].rx_ready, 0, 2);
			k_sem_init(&results[i].disabled, 0, 1);
			memset(buffers[i], 0, sizeof(buffers[i]));
			for (size_t j = 0; j < sizeof(payload[i]); j++) {
				payload[i][j] = (uint8_t)(j ^ (i ? 0xa5 : 0x5a));
			}
			zassert_ok(uart_callback_set(ports[i], callback, &results[i]));
			zassert_ok(
				uart_rx_enable(ports[i], buffers[i][0], capacity, SYS_FOREVER_US));
		}
		ARRAY_FOR_EACH(ports, i) {
			zassert_ok(uart_tx(ports[i], payload[i], first_lengths[round], 100000));
		}
		ARRAY_FOR_EACH(ports, i) {
			zassert_ok(k_sem_take(&results[i].tx_done, K_SECONDS(1)));
			if (chain) {
				zassert_ok(k_sem_take(&results[i].rx_ready, K_SECONDS(1)));
			}
		}
		/* Intentional pacing: even the handoff test makes no gapless claim. */
		k_sleep(K_MSEC(2));
		if (chain) {
			ARRAY_FOR_EACH(ports, i) {
				zassert_ok(uart_tx(ports[i], payload[i] + 32, 7, 100000));
			}
			ARRAY_FOR_EACH(ports, i) {
				zassert_ok(k_sem_take(&results[i].tx_done, K_SECONDS(1)));
			}
			k_sleep(K_MSEC(2));
		}
		ARRAY_FOR_EACH(ports, i) {
			if (chain || round == 2) {
				zassert_ok(uart_rx_disable(ports[i]));
			}
			zassert_ok(k_sem_take(&results[i].disabled, K_SECONDS(1)));
			zassert_ok(results[i].error);
			zassert_equal(results[i].received, first_lengths[round] + (chain ? 7 : 0));
			zassert_equal(results[i].released, chain ? 2 : 1);
			zassert_mem_equal(buffers[i][0], payload[i], first_lengths[round]);
			if (chain) {
				zassert_mem_equal(buffers[i][1], payload[i] + 32, 7);
			}
		}
	}
	wch_rx_hil_stage = 2;
}

ZTEST_SUITE(wch_uart_rx_hardware, NULL, NULL, NULL, cleanup, NULL);
