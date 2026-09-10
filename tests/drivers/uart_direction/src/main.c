/* SPDX-License-Identifier: Apache-2.0 */
#include <zephyr/kernel.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/sys/printk.h>
#include <hal_ch32fun.h>

#define LENGTH 1024U

struct port_diag {
	uint32_t callbacks, mode, rx, tx, errors, done, unexpected, reserved;
	uint8_t received[LENGTH];
};

/* Six header words and two (eight words + LENGTH bytes) port records. */
volatile struct {
	uint32_t magic, stage, cfgr0, brr[2], length;
	struct port_diag port[2];
} wch_direction_diag;

struct port_state {
	const struct device *dev;
	volatile struct port_diag *diag;
	uint8_t transmit[LENGTH];
};

static struct port_state ports[] = {
	{.dev = DEVICE_DT_GET(DT_ALIAS(serial0)), .diag = &wch_direction_diag.port[0]},
	{.dev = DEVICE_DT_GET(DT_ALIAS(serial1)), .diag = &wch_direction_diag.port[1]},
};

static void stop(struct port_state *p)
{
	uart_irq_rx_disable(p->dev);
	uart_irq_tx_disable(p->dev);
	uart_irq_err_disable(p->dev);
}

static void irq_handler(const struct device *dev, void *user_data)
{
	struct port_state *p = user_data;
	volatile struct port_diag *d = p->diag;

	d->callbacks++;
	uart_irq_update(dev);
	int errors = uart_err_check(dev);

	if (errors) {
		d->errors |= (uint32_t)errors;
		stop(p);
		return;
	}
	if (uart_irq_rx_ready(dev)) {
		uint8_t byte;

		while (uart_fifo_read(dev, &byte, 1) == 1) {
			if (d->mode == 0) {
				/* One command per reset. Command byte excluded from payload RX. */
				if (byte == 'R') {
					d->mode = 1;
				} else if (byte == 'B') {
					d->mode = 3; /* Store whole RX frame, then transmit it. */
				} else if (byte == 'E') {
					d->mode = 5; /* Immediate echo of the received prefix. */
				} else if (byte == 'T' || byte == 'D') {
					d->mode = byte == 'T' ? 2 : 4;
					if (d->mode == 2) {
						uart_irq_rx_disable(dev);
						uart_irq_err_disable(dev);
					}
					uart_irq_tx_enable(dev);
					break;
				} else {
					d->unexpected++;
					stop(p);
					return;
				}
			} else if (d->mode == 1 || d->mode == 3 || d->mode == 4 || d->mode == 5) {
				if (d->rx >= LENGTH) {
					d->unexpected++;
					stop(p);
					return;
				}
				d->received[d->rx++] = byte;
				if (d->mode == 5) {
					uart_irq_tx_enable(dev);
				}
				if (d->rx == LENGTH) {
					uart_irq_rx_disable(dev);
					uart_irq_err_disable(dev);
					if (d->mode == 3) {
						uart_irq_tx_enable(dev);
					} else if (d->mode == 1 || d->tx == LENGTH) {
						d->done = 1;
					}
					break;
				}
			}
		}
	}
	if (uart_irq_tx_ready(dev)) {
		while (d->tx < (d->mode == 5 ? d->rx : LENGTH)) {
			uint8_t byte = (d->mode == 3 || d->mode == 5) ?
				      d->received[d->tx] : p->transmit[d->tx];

			if (uart_fifo_fill(dev, &byte, 1) != 1) {
				break;
			}
			d->tx++;
		}
		if (d->mode == 5 && d->tx == d->rx) {
			uart_irq_tx_disable(dev);
		}
		if (d->tx == LENGTH) {
			/* DATAR-fed completion, NOT USART TC / last stop bit. */
			if (d->mode != 4 || d->rx == LENGTH) {
				d->done = 1;
			}
			uart_irq_tx_disable(dev);
		}
	}
}

int main(void)
{
	wch_direction_diag.magic = 0x57444952;
	wch_direction_diag.stage = 1;
	wch_direction_diag.cfgr0 = RCC->CFGR0;
	wch_direction_diag.brr[0] = USART1->BRR;
	wch_direction_diag.brr[1] = USART2->BRR;
	wch_direction_diag.length = LENGTH;
	for (size_t i = 0; i < ARRAY_SIZE(ports); ++i) {
		struct port_state *p = &ports[i];

		if (!device_is_ready(p->dev) ||
		    uart_irq_callback_user_data_set(p->dev, irq_handler, p)) {
			wch_direction_diag.stage = 0xff;
			return -ENODEV;
		}
		for (uint32_t j = 0; j < LENGTH; ++j) {
			p->transmit[j] = (uint8_t)(j * 73U + (j >> 8) * 19U + (i + 1U) * 41U);
		}
		char banner[96];

		snprintk(banner, sizeof(banner),
			 "WCH DIR HIL v1 UART%u n=1024 cfgr0=%08x brr=%08x\r\n",
			 (unsigned int)i + 1, wch_direction_diag.cfgr0, wch_direction_diag.brr[i]);
		/* Bootstrap only; all tested payload uses the IRQ APIs. */
		for (char *c = banner; *c; ++c) {
			uart_poll_out(p->dev, *c);
		}
		uart_irq_rx_enable(p->dev);
		uart_irq_err_enable(p->dev);
	}
	wch_direction_diag.stage = 2;
	k_sleep(K_FOREVER);
	return 0;
}
