/* SPDX-License-Identifier: Apache-2.0 */
#include <zephyr/kernel.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/sys/printk.h>
#include <hal_ch32fun.h>

#define QUEUE_SIZE 1024U

struct port_diag {
	uint32_t callbacks;
	uint32_t rx_bytes;
	uint32_t tx_bytes; /* Includes boot banner; all transmitted using TX IRQ. */
	uint32_t errors;
	uint32_t overflow;
	uint32_t failed;
};

/* Stable array of 32-bit words, inspected only AFTER wire tests. */
volatile struct {
	uint32_t magic;
	uint32_t stage;
	uint32_t cfgr0;
	uint32_t brr[2];
	struct port_diag port[2];
} wch_irq_diag;

struct port_state {
	const struct device *dev;
	volatile struct port_diag *diag;
	uint8_t queue[QUEUE_SIZE];
	uint32_t head;
	uint32_t tail;
};

static struct port_state ports[] = {
	{.dev = DEVICE_DT_GET(DT_ALIAS(serial0)), .diag = &wch_irq_diag.port[0]},
	{.dev = DEVICE_DT_GET(DT_ALIAS(serial1)), .diag = &wch_irq_diag.port[1]},
};

static void fail(struct port_state *p)
{
	p->diag->failed = 1;
	uart_irq_rx_disable(p->dev);
	uart_irq_tx_disable(p->dev);
	uart_irq_err_disable(p->dev);
}

static void irq_handler(const struct device *dev, void *user_data)
{
	struct port_state *p = user_data;

	p->diag->callbacks++;
	uart_irq_update(dev);
	/* Fail closed on errors: do not hide loss or spin on sticky ORE. */
	int errors = uart_err_check(dev);

	if (errors != 0) {
		p->diag->errors |= (uint32_t)errors;
		fail(p);
		return;
	}
	if (uart_irq_rx_ready(dev)) {
		uint8_t byte;

		while (uart_fifo_read(dev, &byte, 1) == 1) {
			p->diag->rx_bytes++;
			if (p->head - p->tail == QUEUE_SIZE) {
				p->diag->overflow++;
				fail(p);
				return;
			}
			p->queue[p->head++ % QUEUE_SIZE] = byte;
		}
		if (p->head != p->tail) {
			uart_irq_tx_enable(dev);
		}
	}
	if (uart_irq_tx_ready(dev)) {
		while (p->head != p->tail &&
		       uart_fifo_fill(dev, &p->queue[p->tail % QUEUE_SIZE], 1) == 1) {
			p->tail++;
			p->diag->tx_bytes++;
		}
		if (p->head == p->tail) {
			uart_irq_tx_disable(dev);
		}
	}
}

int main(void)
{
	wch_irq_diag.magic = 0x57434849;
	wch_irq_diag.cfgr0 = RCC->CFGR0;
	wch_irq_diag.brr[0] = USART1->BRR;
	wch_irq_diag.brr[1] = USART2->BRR;
	wch_irq_diag.stage = 1;
	for (size_t i = 0; i < ARRAY_SIZE(ports); ++i) {
		struct port_state *p = &ports[i];

		if (!device_is_ready(p->dev) ||
		    uart_irq_callback_user_data_set(p->dev, irq_handler, p) != 0) {
			wch_irq_diag.stage = 0xff;
			return -ENODEV;
		}
		/* Seed before IRQs; thereafter only this port's ISR owns its queue. */
		p->head = snprintk((char *)p->queue, sizeof(p->queue),
			"WCH IRQ HIL v1 UART%u cfgr0=%08x brr=%08x USB/DMA off\r\n",
			(unsigned int)i + 1, wch_irq_diag.cfgr0, wch_irq_diag.brr[i]);
		uart_irq_rx_enable(p->dev);
		uart_irq_err_enable(p->dev);
		uart_irq_tx_enable(p->dev);
	}
	wch_irq_diag.stage = 2;
	k_sleep(K_FOREVER);
	return 0;
}
