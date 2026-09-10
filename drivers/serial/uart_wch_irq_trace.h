/* SPDX-License-Identifier: Apache-2.0 */
#ifndef WCH_UART_IRQ_TRACE_H
#define WCH_UART_IRQ_TRACE_H

/* Included only in the opt-in IRQ driver. Single core; one ISR owns each port.
 * No new UART status reads: observing STATR itself can affect clear sequences.
 * Raw SysTick time wraps every CMP+1 cycles. Deltas assume < one timer period;
 * they are callback/service durations, NOT hardware interrupt pending latency.
 */
#define WCH_TRACE_EVENTS 32U
#define WCH_TRACE_ERROR_MASK (USART_STATR_PE | USART_STATR_FE | USART_STATR_NE | \
			      USART_STATR_ORE | USART_STATR_LBD)

struct wch_irq_event {
	uint32_t tick, kind, status, irq, rx, tx, duration, detail;
};

struct wch_irq_port_trace {
	uint32_t regs, baud, irq_count, rx_bytes, tx_bytes;
	uint32_t api_error_or, fifo_error_or, api_error_reads, fifo_error_reads;
	uint32_t max_isr_ticks, slow_isrs, max_rx_service_ticks, rx_service_over_char;
	uint32_t entry_tick, last_status, events_written, frozen;
	uint32_t first_error_site, first_error_status, first_error_tick;
	uint32_t first_error_rx, first_error_tx;
	uint32_t char_ticks, timing_samples, sample_countdown;
	struct wch_irq_event events[WCH_TRACE_EVENTS];
};

/* Versioned debugger ABI: eight header words, then port_count fixed records. */
volatile struct {
	uint32_t magic, version, port_count, port_words;
	uint32_t event_count, period_ticks, nominal_hz, slow_ticks;
	struct wch_irq_port_trace ports[DT_NUM_INST_STATUS_OKAY(DT_DRV_COMPAT)];
} wch_uart_irq_trace = {
	.magic = 0x57495452, .version = 1,
	.port_count = DT_NUM_INST_STATUS_OKAY(DT_DRV_COMPAT),
	.port_words = sizeof(struct wch_irq_port_trace) / sizeof(uint32_t),
	.event_count = WCH_TRACE_EVENTS,
	.period_ticks = CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC / CONFIG_SYS_CLOCK_TICKS_PER_SEC + 1U,
	.nominal_hz = CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC,
	.slow_ticks = CONFIG_WCH_UART_IRQ_TRACE_SLOW_US *
		(CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC / 1000000U),
};

BUILD_ASSERT(offsetof(struct wch_irq_port_trace, events) == 25U * sizeof(uint32_t));

static inline uint32_t wch_trace_tick(void)
{
	return (uint32_t)((SysTick_Type *)DT_REG_ADDR(DT_NODELABEL(systick)))->CNT;
}

static inline uint32_t wch_trace_delta(uint32_t now, uint32_t before)
{
	return now >= before ? now - before : wch_uart_irq_trace.period_ticks - before + now;
}

static void wch_trace_event(volatile struct wch_irq_port_trace *t, uint32_t kind,
			    uint32_t status, uint32_t tick, uint32_t duration)
{
	if (t->frozen) {
		return;
	}
	volatile struct wch_irq_event *e = &t->events[t->events_written % WCH_TRACE_EVENTS];

	e->tick = tick;
	e->kind = kind;
	e->status = status;
	e->irq = t->irq_count;
	e->rx = t->rx_bytes;
	e->tx = t->tx_bytes;
	e->duration = duration;
	e->detail = t->max_rx_service_ticks;
	t->events_written++;
}

/* site 3: err_check snapshot, site 4: fifo_read snapshot before DATAR. */
static inline void wch_trace_status(volatile struct wch_irq_port_trace *t,
			     uint32_t status, uint32_t site)
{
	uint32_t errors = status & WCH_TRACE_ERROR_MASK;

	if (t->entry_tick != UINT32_MAX) {
		t->last_status = status;
	}
	if (!errors) {
		return;
	}
	if (site == 3) {
		t->api_error_or |= errors;
		t->api_error_reads++;
	} else {
		t->fifo_error_or |= errors;
		t->fifo_error_reads++;
	}
	if (!t->frozen) {
		uint32_t tick = wch_trace_tick();

		t->first_error_site = site;
		t->first_error_status = status;
		t->first_error_tick = tick;
		t->first_error_rx = t->rx_bytes;
		t->first_error_tx = t->tx_bytes;
		wch_trace_event(t, site, status, tick, t->entry_tick == UINT32_MAX ?
				UINT32_MAX : wch_trace_delta(tick, t->entry_tick));
		t->frozen = 1;
	}
}

static inline void wch_trace_enter(volatile struct wch_irq_port_trace *t)
{
	t->irq_count++;
	if (t->sample_countdown == 0U) {
		t->sample_countdown = CONFIG_WCH_UART_IRQ_TRACE_SAMPLE_INTERVAL - 1U;
		t->entry_tick = wch_trace_tick();
		t->timing_samples++;
	} else {
		t->sample_countdown--;
		t->entry_tick = UINT32_MAX;
	}
}

static inline void wch_trace_exit(volatile struct wch_irq_port_trace *t)
{
	if (t->entry_tick == UINT32_MAX) {
		return;
	}
	uint32_t tick = wch_trace_tick();
	uint32_t duration = wch_trace_delta(tick, t->entry_tick);

	if (duration > t->max_isr_ticks) {
		t->max_isr_ticks = duration;
	}
	if (duration >= wch_uart_irq_trace.slow_ticks) {
		t->slow_isrs++;
		wch_trace_event(t, 2, t->last_status, tick, duration);
	} else if (CONFIG_WCH_UART_IRQ_TRACE_SAMPLE_INTERVAL > 1 || (t->irq_count & 127U) == 0U) {
		wch_trace_event(t, 1, t->last_status, tick, duration);
	}
}
#endif
