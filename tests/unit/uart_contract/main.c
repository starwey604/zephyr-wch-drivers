/* SPDX-License-Identifier: Apache-2.0 */

#include <stdio.h>
#include <string.h>
#include "uart_wch_contract.h"

/* Not assert(): checks remain active in Release/NDEBUG builds. */
#define CHECK(expr) do { if (!(expr)) { \
	fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); return 1; \
} } while (0)

static int test_baud(void)
{
	uint32_t brr = 0;

	CHECK(wch_uart_brr(144000000, 3000000, &brr) == 0 && brr == 48);
	CHECK(wch_uart_brr(144000000, 115200, &brr) == 0 && brr == 1250);
	CHECK(wch_uart_brr(8000000, 115200, &brr) == 0 && brr == 69);
	CHECK(wch_uart_brr(16, 1, &brr) == 0 && brr == 16);
	CHECK(wch_uart_brr(65535, 1, &brr) == 0 && brr == 65535);
	CHECK(wch_uart_brr(UINT32_MAX, 1000000, &brr) == 0 && brr == 4295);
	brr = 123;
	CHECK(wch_uart_brr(0, 115200, &brr) == -EINVAL);
	CHECK(wch_uart_brr(144000000, 0, &brr) == -EINVAL);
	CHECK(wch_uart_brr(144000000, 3000000, NULL) == -EINVAL);
	CHECK(wch_uart_brr(15, 1, &brr) == -ERANGE);
	CHECK(wch_uart_brr(65536, 1, &brr) == -ERANGE);
	CHECK(wch_uart_brr(UINT32_MAX, UINT32_MAX, &brr) == -ERANGE);
	CHECK(brr == 123);
	return 0;
}

static int test_buffer(void)
{
	uint8_t byte = 0;

	CHECK(wch_uart_dma_buffer_check(NULL, 1) == -EINVAL);
	CHECK(wch_uart_dma_buffer_check(&byte, 0) == -EINVAL);
	CHECK(wch_uart_dma_buffer_check(&byte, 1) == 0);
	/* Length validation only: these calls never dereference the buffer. */
	CHECK(wch_uart_dma_buffer_check(&byte, 65535) == 0);
	CHECK(wch_uart_dma_buffer_check(&byte, 65536) == -EMSGSIZE);
	CHECK(wch_uart_dma_buffer_check(&byte, SIZE_MAX) == -EMSGSIZE);
	return 0;
}

static int test_progress(void)
{
	size_t offset = 99, count = 99;

	CHECK(wch_uart_dma_rx_delta(8, 8, 0, &offset, &count) == 0 && count == 0);
	CHECK(wch_uart_dma_rx_delta(8, 5, 0, &offset, &count) == 0 && count == 3 && offset == 0);
	CHECK(wch_uart_dma_rx_delta(8, 5, 3, &offset, &count) == 0 && count == 0 && offset == 3);
	CHECK(wch_uart_dma_rx_delta(8, 0, 3, &offset, &count) == 0 && count == 5 && offset == 3);
	CHECK(wch_uart_dma_rx_delta(65535, 0, 0, &offset, &count) == 0 && count == 65535);
	CHECK(wch_uart_dma_rx_delta(0, 0, 0, &offset, &count) == -EINVAL);
	CHECK(wch_uart_dma_rx_delta(65536, 0, 0, &offset, &count) == -EINVAL);
	CHECK(wch_uart_dma_rx_delta(8, 9, 0, &offset, &count) == -EINVAL);
	CHECK(wch_uart_dma_rx_delta(8, 5, 4, &offset, &count) == -EINVAL);
	CHECK(wch_uart_dma_rx_delta(8, 0, 9, &offset, &count) == -EINVAL);
	CHECK(wch_uart_dma_rx_delta(8, 0, 0, NULL, &count) == -EINVAL);
	CHECK(wch_uart_dma_rx_delta(8, 0, 0, &offset, NULL) == -EINVAL);
	CHECK(wch_uart_dma_rx_delta(8, 0, 0, &count, &count) == -EINVAL);
	CHECK(offset == 0 && count == 65535);
	return 0;
}

int main(int argc, char **argv)
{
	if (argc != 2) {
		return 2;
	}
	if (strcmp(argv[1], "baud") == 0) {
		return test_baud();
	}
	if (strcmp(argv[1], "buffer") == 0) {
		return test_buffer();
	}
	if (strcmp(argv[1], "progress") == 0) {
		return test_progress();
	}
	return 2;
}
