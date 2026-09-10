/*
 * Copyright (c) 2024 Google LLC.
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Downstream import: BOJIT/zephyr, driver/ch32_usb,
 * dc53b3104fbbe6db5d35d3276d281ffdc3da6483.
 * See docs/upstream.md for provenance and local changes.
 * Polling/interrupt baseline plus opt-in async DMA extensions.
 */

#define DT_DRV_COMPAT wch_usart

#include <errno.h>

#include <zephyr/drivers/uart.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/irq.h>

#include <hal_ch32fun.h>

#include "uart_wch_contract.h"

#ifdef CONFIG_WCH_UART_IRQ_TRACE
#include "uart_wch_irq_trace.h"
#define USART_WCH_TRACE_CONFIG(idx) .trace = &wch_uart_irq_trace.ports[idx],
#else
#define USART_WCH_TRACE_CONFIG(idx)
#endif

#ifdef CONFIG_WCH_UART_ASYNC_TX
#include <zephyr/drivers/dma.h>
#include "uart_wch_tx_state.h"
#ifdef CONFIG_WCH_UART_ASYNC_RX
#include "uart_wch_rx_state.h"
#endif
#endif

#ifdef CONFIG_WCH_UART_DMA_PREPARE
#include "uart_wch_dma_dt.h"
BUILD_ASSERT(CONFIG_DMA_INIT_PRIORITY < CONFIG_SERIAL_INIT_PRIORITY,
	     "DMA must initialize before UART DMA preparation");
#endif

struct usart_wch_config {
	USART_TypeDef *regs;
#ifdef CONFIG_WCH_UART_IRQ_TRACE
	volatile struct wch_irq_port_trace *trace;
#endif
	const struct device *clock_dev;
	uint32_t current_speed;
	uint8_t parity;
	uint8_t stop_bits;
	uint8_t data_bits;
	bool hw_flow_control;
	uint8_t clock_id;
	const struct pinctrl_dev_config *pin_cfg;
#ifdef CONFIG_WCH_UART_DMA_PREPARE
	const struct device *dma_tx_dev;
	const struct device *dma_rx_dev;
#endif
#ifdef CONFIG_WCH_UART_ASYNC_TX
	uint32_t dma_tx_channel;
	uint32_t dma_rx_channel;
#endif
#if defined(CONFIG_UART_INTERRUPT_DRIVEN) || defined(CONFIG_WCH_UART_ASYNC_TX)
	void (*irq_config_func)(const struct device *dev);
#endif
};

struct usart_wch_data {
	uart_irq_callback_user_data_t cb;
	void *user_data;
#ifdef CONFIG_WCH_UART_ASYNC_TX
	struct k_spinlock lock;
	struct wch_uart_tx tx;
#ifdef CONFIG_WCH_UART_ASYNC_RX
	struct wch_uart_rx rx;
#endif
#endif
};

#ifdef CONFIG_WCH_UART_ASYNC_TX
#ifdef CONFIG_WCH_UART_ASYNC_RX
#include "uart_wch_rx_impl.h"
#endif
#include "uart_wch_tx_impl.h"
#endif

static int usart_wch_init(const struct device *dev)
{
	const struct usart_wch_config *config = dev->config;
	USART_TypeDef *regs = config->regs;
	uint32_t ctlr1 = USART_CTLR1_TE | USART_CTLR1_RE | USART_CTLR1_UE;
	uint32_t clock_rate;
	clock_control_subsys_t clock_sys = (clock_control_subsys_t)(uintptr_t)config->clock_id;
	uint32_t divn;
	int err;

	/* Reject unsupported framing instead of silently configuring another format. */
	if (config->parity != UART_CFG_PARITY_NONE || config->stop_bits != UART_CFG_STOP_BITS_1 ||
	    config->data_bits != 8 || config->hw_flow_control) {
		return -ENOTSUP;
	}
	if (!device_is_ready(config->clock_dev)) {
		return -ENODEV;
	}
#ifdef CONFIG_WCH_UART_DMA_PREPARE
	if (!device_is_ready(config->dma_tx_dev) || !device_is_ready(config->dma_rx_dev)) {
		return -ENODEV;
	}
#endif
	err = clock_control_on(config->clock_dev, clock_sys);
	if (err != 0) {
		return err;
	}

	err = clock_control_get_rate(config->clock_dev, clock_sys, &clock_rate);
	if (err != 0) {
		return err;
	}
	err = wch_uart_brr(clock_rate, config->current_speed, &divn);
	if (err != 0) {
		return err;
	}

	err = pinctrl_apply_state(config->pin_cfg, PINCTRL_STATE_DEFAULT);
	if (err != 0) {
		return err;
	}
	/* Configure while disabled; enable only after clocks/pins/config are valid. */
#ifdef CONFIG_WCH_UART_ASYNC_TX
	err = usart_wch_tx_init(dev);
	if (err != 0) {
		return err;
	}
#endif
	regs->CTLR1 = 0;
	regs->CTLR2 = 0;
	regs->CTLR3 = 0;
	regs->BRR = divn;
	regs->CTLR1 = ctlr1;

#ifdef CONFIG_WCH_UART_IRQ_TRACE
	config->trace->regs = (uint32_t)(uintptr_t)regs;
	config->trace->baud = config->current_speed;
	config->trace->char_ticks = (CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC / config->current_speed) * 10U;
#endif

#if defined(CONFIG_UART_INTERRUPT_DRIVEN) || defined(CONFIG_WCH_UART_ASYNC_TX)
	config->irq_config_func(dev);
#endif

	return 0;
}

static int usart_wch_poll_in(const struct device *dev, unsigned char *ch)
{
	const struct usart_wch_config *config = dev->config;
	USART_TypeDef *regs = config->regs;

#ifdef CONFIG_WCH_UART_ASYNC_RX
	struct usart_wch_data *data = dev->data;
	k_spinlock_key_t key = k_spin_lock(&data->lock);
	int ret = -1;

	if (data->rx.phase != WCH_RX_IDLE) {
		ret = -EBUSY;
	} else if (regs->STATR & USART_STATR_RXNE) {
		*ch = regs->DATAR;
		ret = 0;
	}
	k_spin_unlock(&data->lock, key);
	return ret;
#else
	if ((regs->STATR & USART_STATR_RXNE) == 0) {
		return -1;
	}

	*ch = regs->DATAR;
	return 0;
#endif
}

static void usart_wch_poll_out(const struct device *dev, unsigned char ch)
{
#ifdef CONFIG_WCH_UART_ASYNC_TX
	usart_wch_tx_poll_out(dev, ch);
#else
	const struct usart_wch_config *config = dev->config;
	USART_TypeDef *regs = config->regs;

	while ((regs->STATR & USART_STATR_TXE) == 0) {
	}

	regs->DATAR = ch;
#endif
}

static int usart_wch_err_check(const struct device *dev)
{
	const struct usart_wch_config *config = dev->config;
	USART_TypeDef *regs = config->regs;
	uint32_t statr = regs->STATR;

#ifdef CONFIG_WCH_UART_IRQ_TRACE
	wch_trace_status(config->trace, statr, 3);
#endif
	enum uart_rx_stop_reason errors = 0;

	if ((statr & USART_STATR_PE) != 0) {
		errors |= UART_ERROR_PARITY;
	}
	if ((statr & USART_STATR_LBD) != 0) {
		errors |= UART_BREAK;
	}
	if ((statr & USART_STATR_FE) != 0) {
		errors |= UART_ERROR_FRAMING;
	}
	if ((statr & USART_STATR_NE) != 0) {
		errors |= UART_ERROR_NOISE;
	}
	if ((statr & USART_STATR_ORE) != 0) {
		errors |= UART_ERROR_OVERRUN;
	}

	return errors;
}

#ifdef CONFIG_UART_INTERRUPT_DRIVEN

static void usart_wch_isr(const struct device *dev)
{
	struct usart_wch_data *data = dev->data;

#ifdef CONFIG_WCH_UART_IRQ_TRACE
	const struct usart_wch_config *config = dev->config;

	wch_trace_enter(config->trace);
#endif

	if (data->cb) {
		data->cb(dev, data->user_data);
	}
#ifdef CONFIG_WCH_UART_IRQ_TRACE
	wch_trace_exit(config->trace);
#endif
}

static int usart_wch_fifo_fill(const struct device *dev, const uint8_t *tx_data, int len)
{
	const struct usart_wch_config *config = dev->config;
	USART_TypeDef *regs = config->regs;

	if (!len || !(regs->STATR & USART_STATR_TXE)) {
		return 0;
	}

	regs->DATAR = tx_data[0];
#ifdef CONFIG_WCH_UART_IRQ_TRACE
	config->trace->tx_bytes++;
#endif
	return 1;
}

static int usart_wch_fifo_read(const struct device *dev, uint8_t *rx_data, const int size)
{
	const struct usart_wch_config *config = dev->config;
	USART_TypeDef *regs = config->regs;

	if (!size) {
		return 0;
	}
	uint32_t status = regs->STATR;

	if (!(status & USART_STATR_RXNE)) {
#ifdef CONFIG_WCH_UART_IRQ_TRACE
		wch_trace_status(config->trace, status, 4);
#endif
		return 0;
	}

	rx_data[0] = regs->DATAR;
#ifdef CONFIG_WCH_UART_IRQ_TRACE
	/* Log only AFTER DATAR; retain the status that preceded the existing read. */
	volatile struct wch_irq_port_trace *t = config->trace;

	t->rx_bytes++;
	if (t->entry_tick != UINT32_MAX) {
		uint32_t service = wch_trace_delta(wch_trace_tick(), t->entry_tick);

		if (service > t->max_rx_service_ticks) {
			t->max_rx_service_ticks = service;
		}
		if (service >= t->char_ticks) {
			t->rx_service_over_char++;
		}
	}
	wch_trace_status(t, status, 4);
#endif
	return 1;
}

static void usart_wch_irq_tx_enable(const struct device *dev)
{
	const struct usart_wch_config *config = dev->config;
	USART_TypeDef *regs = config->regs;

	regs->CTLR1 |= (USART_CTLR1_TXEIE | USART_CTLR1_TCIE);
}

static void usart_wch_irq_tx_disable(const struct device *dev)
{
	const struct usart_wch_config *config = dev->config;
	USART_TypeDef *regs = config->regs;

	regs->CTLR1 &= ~(USART_CTLR1_TXEIE | USART_CTLR1_TCIE);
}

static int usart_wch_irq_tx_ready(const struct device *dev)
{
	const struct usart_wch_config *config = dev->config;
	USART_TypeDef *regs = config->regs;

	return (regs->STATR & USART_STATR_TXE) != 0 && (regs->CTLR1 & USART_CTLR1_TXEIE) != 0;
}

static void usart_wch_irq_rx_enable(const struct device *dev)
{
	const struct usart_wch_config *config = dev->config;
	USART_TypeDef *regs = config->regs;

	regs->CTLR1 |= USART_CTLR1_RXNEIE;
}

static void usart_wch_irq_rx_disable(const struct device *dev)
{
	const struct usart_wch_config *config = dev->config;
	USART_TypeDef *regs = config->regs;

	regs->CTLR1 &= ~USART_CTLR1_RXNEIE;
}

static int usart_wch_irq_tx_complete(const struct device *dev)
{
	const struct usart_wch_config *config = dev->config;
	USART_TypeDef *regs = config->regs;

	return (regs->STATR & USART_STATR_TC) > 0;
}

static int usart_wch_irq_rx_ready(const struct device *dev)
{
	const struct usart_wch_config *config = dev->config;
	USART_TypeDef *regs = config->regs;

	return (regs->STATR & USART_STATR_RXNE) > 0;
}

static void usart_wch_irq_err_enable(const struct device *dev)
{
	const struct usart_wch_config *config = dev->config;
	USART_TypeDef *regs = config->regs;

	regs->CTLR1 |= USART_CTLR1_PEIE;
	regs->CTLR2 |= USART_CTLR2_LBDIE;
	regs->CTLR3 |= USART_CTLR3_EIE;
}

static void usart_wch_irq_err_disable(const struct device *dev)
{
	const struct usart_wch_config *config = dev->config;
	USART_TypeDef *regs = config->regs;

	regs->CTLR1 &= ~USART_CTLR1_PEIE;
	regs->CTLR2 &= ~USART_CTLR2_LBDIE;
	regs->CTLR3 &= ~USART_CTLR3_EIE;
}

static int usart_wch_irq_is_pending(const struct device *dev)
{
	const struct usart_wch_config *config = dev->config;
	USART_TypeDef *regs = config->regs;
	uint16_t ctlr1 = regs->CTLR1;
	uint16_t ctlr2 = regs->CTLR2;
	uint16_t ctlr3 = regs->CTLR3;
	uint16_t statr = regs->STATR;
	uint16_t stat_mask = 0;

	stat_mask |= (ctlr1 & USART_CTLR1_TXEIE) ? USART_STATR_TXE : 0;
	stat_mask |= (ctlr1 & USART_CTLR1_TCIE) ? USART_STATR_TC : 0;
	stat_mask |= (ctlr1 & USART_CTLR1_RXNEIE) ? USART_STATR_RXNE | USART_STATR_ORE : 0;
	stat_mask |= (ctlr1 & USART_CTLR1_IDLEIE) ? USART_STATR_IDLE : 0;
	stat_mask |= (ctlr1 & USART_CTLR1_PEIE) ? USART_STATR_PE : 0;

	stat_mask |= (ctlr2 & USART_CTLR2_LBDIE) ? USART_STATR_LBD : 0;
	stat_mask |=
		(ctlr3 & USART_CTLR3_EIE) ? USART_STATR_NE | USART_STATR_ORE | USART_STATR_FE : 0;
	stat_mask |= (ctlr3 & USART_CTLR3_CTSIE) ? USART_STATR_CTS : 0;

	return (statr & stat_mask) > 0;
}

static void usart_wch_irq_callback_set(const struct device *dev, uart_irq_callback_user_data_t cb,
				       void *user_data)
{
	struct usart_wch_data *data = dev->data;

	data->cb = cb;
	data->user_data = user_data;
}
#endif /*CONFIG_UART_INTERRUPT_DRIVEN*/

static DEVICE_API(uart, usart_wch_driver_api) = {
	.poll_in = usart_wch_poll_in,
	.poll_out = usart_wch_poll_out,
	.err_check = usart_wch_err_check,
#ifdef CONFIG_WCH_UART_ASYNC_TX
	.callback_set = usart_wch_async_callback_set,
	.tx = usart_wch_tx,
	.tx_abort = usart_wch_tx_abort,
#ifdef CONFIG_WCH_UART_ASYNC_RX
	.rx_enable = usart_wch_rx_enable,
	.rx_buf_rsp = usart_wch_rx_buf_rsp,
	.rx_disable = usart_wch_rx_disable,
#else
	.rx_enable = usart_wch_rx_unsupported,
	.rx_buf_rsp = usart_wch_rx_buf_unsupported,
	.rx_disable = usart_wch_rx_disable_unsupported,
#endif
#endif
#ifdef CONFIG_UART_INTERRUPT_DRIVEN
	.fifo_fill = usart_wch_fifo_fill,
	.fifo_read = usart_wch_fifo_read,
	.irq_tx_enable = usart_wch_irq_tx_enable,
	.irq_tx_disable = usart_wch_irq_tx_disable,
	.irq_tx_ready = usart_wch_irq_tx_ready,
	.irq_rx_enable = usart_wch_irq_rx_enable,
	.irq_rx_disable = usart_wch_irq_rx_disable,
	.irq_tx_complete = usart_wch_irq_tx_complete,
	.irq_rx_ready = usart_wch_irq_rx_ready,
	.irq_err_enable = usart_wch_irq_err_enable,
	.irq_err_disable = usart_wch_irq_err_disable,
	.irq_is_pending = usart_wch_irq_is_pending,
	.irq_callback_set = usart_wch_irq_callback_set,
#endif
};

#if defined(CONFIG_UART_INTERRUPT_DRIVEN) || defined(CONFIG_WCH_UART_ASYNC_TX)
#define USART_WCH_IRQ_HANDLER_DECL(idx)                                                            \
	static void usart_wch_irq_config_func_##idx(const struct device *dev);

#define USART_WCH_IRQ_HANDLER_FUNC(idx) .irq_config_func = usart_wch_irq_config_func_##idx,

#define USART_WCH_IRQ_HANDLER(idx)                                                                 \
	static void usart_wch_irq_config_func_##idx(const struct device *dev)                      \
	{                                                                                          \
		IRQ_CONNECT(DT_INST_IRQN(idx), DT_INST_IRQ(idx, priority), usart_wch_isr,          \
			    DEVICE_DT_INST_GET(idx), 0);                                           \
		irq_enable(DT_INST_IRQN(idx));                                                     \
	}
#else
#define USART_WCH_IRQ_HANDLER_DECL(idx)
#define USART_WCH_IRQ_HANDLER_FUNC(idx)
#define USART_WCH_IRQ_HANDLER(idx)
#endif

#ifdef CONFIG_WCH_UART_DMA_PREPARE
#define USART_WCH_DMA_CHECK(idx) WCH_UART_DMA_DT_CHECK(idx)
#define USART_WCH_DMA_CONFIG(idx)                                                                  \
	.dma_tx_dev = DEVICE_DT_GET(WCH_DMA_CTLR(idx, tx)),                                        \
	.dma_rx_dev = DEVICE_DT_GET(WCH_DMA_CTLR(idx, rx)),
#else
#define USART_WCH_DMA_CHECK(idx)
#define USART_WCH_DMA_CONFIG(idx)
#endif

#ifdef CONFIG_WCH_UART_ASYNC_TX
#define USART_WCH_TX_CONFIG(idx)                                                                   \
	.dma_tx_channel = WCH_DMA_CH(idx, tx), .dma_rx_channel = WCH_DMA_CH(idx, rx),
#else
#define USART_WCH_TX_CONFIG(idx)
#endif

#define USART_WCH_INIT(idx)                                                                        \
	USART_WCH_DMA_CHECK(idx)                                                                   \
	PINCTRL_DT_INST_DEFINE(idx);                                                               \
	USART_WCH_IRQ_HANDLER_DECL(idx)                                                            \
	static struct usart_wch_data usart_wch_##idx##_data;                                       \
	static const struct usart_wch_config usart_wch_##idx##_config = {                          \
		.regs = (USART_TypeDef *)DT_INST_REG_ADDR(idx),                                    \
		USART_WCH_TRACE_CONFIG(idx)                                                       \
		.current_speed = DT_INST_PROP(idx, current_speed),                                 \
		.parity = DT_INST_ENUM_IDX(idx, parity),                                           \
		.stop_bits = DT_INST_ENUM_IDX_OR(idx, stop_bits, UART_CFG_STOP_BITS_1),            \
		.data_bits = DT_INST_PROP_OR(idx, data_bits, 8),                                   \
		.hw_flow_control = DT_INST_PROP(idx, hw_flow_control),                             \
		.clock_dev = DEVICE_DT_GET(DT_INST_CLOCKS_CTLR(idx)),                              \
		.clock_id = DT_INST_CLOCKS_CELL(idx, id),                                          \
		.pin_cfg = PINCTRL_DT_INST_DEV_CONFIG_GET(idx),                                    \
		USART_WCH_DMA_CONFIG(idx) USART_WCH_TX_CONFIG(idx)                                 \
			USART_WCH_IRQ_HANDLER_FUNC(idx)};                                          \
	DEVICE_DT_INST_DEFINE(idx, &usart_wch_init, NULL, &usart_wch_##idx##_data,                 \
			      &usart_wch_##idx##_config, PRE_KERNEL_1,                             \
			      CONFIG_SERIAL_INIT_PRIORITY, &usart_wch_driver_api);                 \
	USART_WCH_IRQ_HANDLER(idx)

DT_INST_FOREACH_STATUS_OKAY(USART_WCH_INIT)
