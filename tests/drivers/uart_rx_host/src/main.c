/* SPDX-License-Identifier: Apache-2.0 */
#include <zephyr/kernel.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/sys/printk.h>
#include <hal_ch32fun.h>

struct round_diag {
	uint32_t ready, bytes, released, requests, disabled, stopped, bad_event, match;
};

/* Header: magic, stage, CFGR0, BRR1/2, round, passed rounds, error (8 words). */
volatile struct {
	uint32_t magic, stage, cfgr0, brr[2], round, passed;
	int32_t error;
	struct round_diag results[5][2];
} wch_rx_host_diag;

struct port_state {
	const struct device *dev;
	struct k_sem ready, disabled;
	uint8_t first[1024], next[32];
	volatile struct round_diag *diag;
	uint32_t releases;
	size_t capacity;
};
static struct port_state ports[] = {
	{.dev = DEVICE_DT_GET(DT_ALIAS(serial0))},
	{.dev = DEVICE_DT_GET(DT_ALIAS(serial1))},
};

static void callback(const struct device *dev, struct uart_event *event, void *user)
{
	struct port_state *p = user;
	volatile struct round_diag *d = p->diag;
	bool chain = wch_rx_host_diag.round == 3;
	uint32_t mask;

	switch (event->type) {
	case UART_RX_BUF_REQUEST:
		d->requests++;
		if (chain && d->requests == 1 && uart_rx_buf_rsp(dev, p->next, sizeof(p->next))) {
			d->bad_event++;
		}
		break;
	case UART_RX_RDY:
		d->ready++;
		d->bytes += event->data.rx.len;
		if (event->data.rx.offset || event->data.rx.len == 0 ||
		    (event->data.rx.buf != p->first && (!chain || event->data.rx.buf != p->next)) ||
		    event->data.rx.len > (event->data.rx.buf == p->first ? p->capacity : 32)) {
			d->bad_event++;
		}
		k_sem_give(&p->ready);
		break;
	case UART_RX_BUF_RELEASED:
		d->released++;
		mask = event->data.rx_buf.buf == p->first ? 1 :
		       (chain && event->data.rx_buf.buf == p->next ? 2 : 0);
		if (!mask || (p->releases & mask)) {
			d->bad_event++;
		}
		p->releases |= mask;
		break;
	case UART_RX_DISABLED:
		d->disabled++;
		k_sem_give(&p->disabled);
		break;
	case UART_RX_STOPPED:
		d->stopped |= event->data.rx_stop.reason;
		d->bad_event++; /* Even a zero-reason stop is not a normal completion. */
		break;
	default:
		d->bad_event++;
		break;
	}
}

static void announce(size_t port, const char *kind)
{
	char text[64];

	snprintk(text, sizeof(text), "WCH RX %s UART%u round=%u\r\n", kind,
		 (unsigned int)port + 1, wch_rx_host_diag.round);
	/* Control/handshake only. All measured incoming payload uses DMA. */
	for (char *c = text; *c; c++) {
		uart_poll_out(ports[port].dev, *c);
	}
}

static uint8_t expected(size_t offset, size_t port, size_t round)
{
	return (uint8_t)(offset * 73U + (offset >> 8) * 19U + (port + 1U) * 41U + round * 7U);
}

static int run(void)
{
	const size_t lengths[] = {1, 32, 7, 39, 1024};

	ARRAY_FOR_EACH(ports, i) {
		if (!device_is_ready(ports[i].dev)) {
			return -ENODEV;
		}
		k_sem_init(&ports[i].ready, 0, 2);
		k_sem_init(&ports[i].disabled, 0, 1);
		int ret = uart_callback_set(ports[i].dev, callback, &ports[i]);

		if (ret) {
			return ret;
		}
	}
	ARRAY_FOR_EACH(lengths, round) {
		wch_rx_host_diag.round = round;
		ARRAY_FOR_EACH(ports, i) {
			struct port_state *p = &ports[i];

			p->diag = &wch_rx_host_diag.results[round][i];
			p->releases = 0;
			p->capacity = round == 2 || round == 3 ? 32 : lengths[round];
			memset(p->first, 0, sizeof(p->first));
			memset(p->next, 0, sizeof(p->next));
			k_sem_reset(&p->ready);
			k_sem_reset(&p->disabled);
			int ret = uart_rx_enable(p->dev, p->first, p->capacity, SYS_FOREVER_US);

			if (ret) {
				return ret;
			}
		}
		ARRAY_FOR_EACH(ports, i) {
			announce(i, "READY");
		}
		if (round == 3) {
			ARRAY_FOR_EACH(ports, i) {
				if (k_sem_take(&ports[i].ready, K_SECONDS(2))) {
					return -ETIMEDOUT;
				}
			}
			ARRAY_FOR_EACH(ports, i) {
				announce(i, "NEXT");
			}
		}
		if (round == 2 || round == 3) {
			/* Bounded host-send window, not driver inactivity/IDLE support. */
			k_sleep(K_MSEC(500));
			ARRAY_FOR_EACH(ports, i) {
				int ret = uart_rx_disable(ports[i].dev);

				if (ret) {
					return ret;
				}
			}
		}
		ARRAY_FOR_EACH(ports, i) {
			struct port_state *p = &ports[i];
			volatile struct round_diag *d = p->diag;

			if (k_sem_take(&p->disabled, K_SECONDS(2))) {
				return -ETIMEDOUT;
			}
			bool match = true;

			for (size_t j = 0; j < lengths[round]; j++) {
				uint8_t got = round == 3 && j >= 32 ? p->next[j - 32] : p->first[j];

				match &= got == expected(j, i, round);
			}
			d->match = match;
			uint32_t events = round == 3 ? 2 : 1;

			if (!match || d->bytes != lengths[round] || d->ready != events ||
			    d->released != events || d->requests != events || d->disabled != 1 ||
			    d->stopped || d->bad_event || p->releases != (round == 3 ? 3 : 1)) {
				return -EIO;
			}
		}
		wch_rx_host_diag.passed++;
		k_sleep(K_MSEC(10));
	}
	return 0;
}

int main(void)
{
	wch_rx_host_diag.magic = 0x57525848;
	wch_rx_host_diag.stage = 1;
	wch_rx_host_diag.cfgr0 = RCC->CFGR0;
	wch_rx_host_diag.brr[0] = USART1->BRR;
	wch_rx_host_diag.brr[1] = USART2->BRR;
	wch_rx_host_diag.error = run();
	ARRAY_FOR_EACH(ports, i) {
		/* Stop on failures too; retain callbacks until loans are released. */
		if (device_is_ready(ports[i].dev)) {
			(void)uart_rx_disable(ports[i].dev);
			(void)uart_callback_set(ports[i].dev, NULL, NULL);
		}
	}
	wch_rx_host_diag.stage = wch_rx_host_diag.error ? 0xff : 2;
	k_sleep(K_FOREVER);
	return 0;
}
