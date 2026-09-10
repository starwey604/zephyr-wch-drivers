/* SPDX-License-Identifier: Apache-2.0 */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/* Test trace arithmetic/storage only, not USART/PFIC hardware or IRQ timing. */
typedef struct { uint32_t CNT; } SysTick_Type;
static SysTick_Type timer;
#define DT_REG_ADDR(node) ((uintptr_t)&timer)
#define DT_NODELABEL(node) node
#define DT_NUM_INST_STATUS_OKAY(compat) 2
#define CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC 144000000U
#define CONFIG_SYS_CLOCK_TICKS_PER_SEC 100U
#define CONFIG_WCH_UART_IRQ_TRACE_SLOW_US 8U
#define CONFIG_WCH_UART_IRQ_TRACE_SAMPLE_INTERVAL 127U
#define USART_STATR_PE 1U
#define USART_STATR_FE 2U
#define USART_STATR_NE 4U
#define USART_STATR_ORE 8U
#define USART_STATR_LBD 256U
#define BUILD_ASSERT(expr) _Static_assert(expr, #expr)
#include "uart_wch_irq_trace.h"
#define CHECK(expr) do { if (!(expr)) { fprintf(stderr, "FAIL %d: %s\n", __LINE__, #expr); exit(1); } } while (0)

int main(void)
{
	volatile struct wch_irq_port_trace *t = &wch_uart_irq_trace.ports[0];

	CHECK(wch_trace_delta(5, 1440000) == 6);
	CHECK(wch_trace_delta(99, 42) == 57);
	for (uint32_t i = 0; i < 40; ++i) {
		wch_trace_event(t, 1, 0, i, 7);
	}
	CHECK(t->events_written == 40);
	CHECK(t->events[7].tick == 39);
	timer.CNT = 20;
	t->entry_tick = 10;
	t->rx_bytes = 12;
	t->tx_bytes = 11;
	wch_trace_status(t, USART_STATR_ORE | 32U, 4);
	CHECK(t->fifo_error_or == USART_STATR_ORE);
	CHECK(t->api_error_or == 0);
	CHECK(t->first_error_status == (USART_STATR_ORE | 32U));
	CHECK(t->first_error_rx == 12 && t->first_error_tx == 11);
	CHECK(t->first_error_site == 4 && t->frozen == 1 && t->events_written == 41);
	wch_trace_status(t, USART_STATR_FE, 3);
	CHECK(t->api_error_or == USART_STATR_FE && t->api_error_reads == 1);
	CHECK(t->first_error_site == 4 && t->events_written == 41);
	timer.CNT = 2010;
	wch_trace_exit(t);
	CHECK(t->max_isr_ticks == 2000 && t->slow_isrs == 1 && t->events_written == 41);
	CHECK(wch_uart_irq_trace.ports[1].frozen == 0);
	t = &wch_uart_irq_trace.ports[1];
	wch_trace_enter(t);
	CHECK(t->entry_tick == timer.CNT && t->timing_samples == 1);
	for (uint32_t i = 0; i < 126; ++i) {
		wch_trace_enter(t);
		CHECK(t->entry_tick == UINT32_MAX);
	}
	wch_trace_exit(t);
	CHECK(t->max_isr_ticks == 0);
	wch_trace_enter(t);
	CHECK(t->entry_tick == timer.CNT && t->timing_samples == 2 && t->irq_count == 128);
	t->entry_tick = UINT32_MAX;
	wch_trace_status(t, USART_STATR_ORE, 4);
	CHECK(t->events[0].duration == UINT32_MAX && t->frozen == 1);
	puts("PASS wrap, ring overwrite, first-error freeze, site latches, slow timing, port isolation");
	return 0;
}
