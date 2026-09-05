/* SPDX-License-Identifier: Apache-2.0 */

#ifndef WCH_UART_CONTRACT_H_
#define WCH_UART_CONTRACT_H_

#include <errno.h>
#include <stddef.h>
#include <stdint.h>

/* CH32V203: 16-bit BRR, oversampling by 16; byte-wide DMA CNTR is 16 bits. */
#define WCH_UART_DMA_MAX_BYTES UINT16_MAX

static inline int wch_uart_brr(uint32_t clock_hz, uint32_t baud, uint32_t *brr)
{
	if (brr == NULL || baud == 0 || clock_hz == 0) {
		return -EINVAL;
	}
	uint64_t divisor = ((uint64_t)clock_hz + baud / 2U) / baud;

	if ((uint64_t)baud * 16U > clock_hz || divisor > UINT16_MAX) {
		return -ERANGE;
	}
	*brr = (uint32_t)divisor;
	return 0;
}

/* Preparation helpers only: callers must separately check DMA-accessible memory. */
static inline int wch_uart_dma_buffer_check(const void *buffer, size_t length)
{
	if (buffer == NULL || length == 0) {
		return -EINVAL;
	}
	return length > WCH_UART_DMA_MAX_BYTES ? -EMSGSIZE : 0;
}

/* Non-cyclic, byte-wide DMA only. Does NOT determine UART wire TX completion. */
static inline int wch_uart_dma_rx_delta(size_t length, size_t pending, size_t reported,
				      size_t *offset, size_t *count)
{
	if (offset == NULL || count == NULL || offset == count || length == 0 ||
	    length > WCH_UART_DMA_MAX_BYTES || pending > length || reported > length - pending) {
		return -EINVAL;
	}
	*offset = reported;
	*count = length - pending - reported;
	return 0;
}

#endif /* WCH_UART_CONTRACT_H_ */
