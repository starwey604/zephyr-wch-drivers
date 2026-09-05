/* SPDX-License-Identifier: Apache-2.0 */

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/dma.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/ztest.h>

#define UART0_NODE             DT_ALIAS(test_uart0)
#define UART1_NODE             DT_ALIAS(test_uart1)
#define DMA_CTLR(node, dir)    DT_DMAS_CTLR_BY_NAME(node, dir)
#define DMA_CHANNEL(node, dir) DT_DMAS_CELL_BY_NAME(node, dir, channel)

BUILD_ASSERT(IS_ENABLED(CONFIG_WCH_UART), "This test must exercise the external UART driver");
BUILD_ASSERT(IS_ENABLED(CONFIG_WCH_UART_DMA_PREPARE), "DMA preparation must be tested");
BUILD_ASSERT(IS_ENABLED(CONFIG_WCH_DMA), "Use the DMA driver with fixed-channel filtering");
BUILD_ASSERT(!IS_ENABLED(CONFIG_UART_WCH_USART), "Disable the upstream UART driver");
BUILD_ASSERT(DT_NODE_HAS_STATUS(UART0_NODE, okay), "test-uart0 must be enabled");
BUILD_ASSERT(DT_NODE_HAS_STATUS(UART1_NODE, okay), "test-uart1 must be enabled");
BUILD_ASSERT(!DT_SAME_NODE(UART0_NODE, UART1_NODE), "Two distinct UARTs are required");

static const struct device *const uarts[] = {
	DEVICE_DT_GET(UART0_NODE),
	DEVICE_DT_GET(UART1_NODE),
};

static const struct {
	const struct device *controller;
	uint32_t channel;
} dma_requests[] = {
	{DEVICE_DT_GET(DMA_CTLR(UART0_NODE, tx)), DMA_CHANNEL(UART0_NODE, tx)},
	{DEVICE_DT_GET(DMA_CTLR(UART0_NODE, rx)), DMA_CHANNEL(UART0_NODE, rx)},
	{DEVICE_DT_GET(DMA_CTLR(UART1_NODE, tx)), DMA_CHANNEL(UART1_NODE, tx)},
	{DEVICE_DT_GET(DMA_CTLR(UART1_NODE, rx)), DMA_CHANNEL(UART1_NODE, rx)},
};

ZTEST(wch_uart_dma, test_uart_devices_ready)
{
	ARRAY_FOR_EACH(uarts, i) {
		zassert_true(device_is_ready(uarts[i]), "UART %u is not ready", (unsigned)i);
	}
}

ZTEST(wch_uart_dma, test_dma_request_wiring)
{
	ARRAY_FOR_EACH(dma_requests, i) {
		zassert_true(device_is_ready(dma_requests[i].controller), "DMA is not ready");
		for (size_t j = i + 1; j < ARRAY_SIZE(dma_requests); j++) {
			zassert_false(dma_requests[i].controller == dma_requests[j].controller &&
					      dma_requests[i].channel == dma_requests[j].channel,
				      "UART DMA requests must not share a channel");
		}
	}
}

ZTEST(wch_uart_dma, test_async_dma_loopback_pending)
{
	/* Sustained traffic/IDLE qualification remains separate from paced RX. */
	TC_PRINT("SKIP: continuous RX/IDLE qualification requires hardware\n");
	ztest_test_skip();
}

ZTEST(wch_uart_dma, test_fixed_request_allocation)
{
	/* The prepare-only UART driver has not reserved these channels yet. */
	ARRAY_FOR_EACH(dma_requests, i) {
		uint32_t requested = dma_requests[i].channel;
		const struct device *controller = dma_requests[i].controller;
		int channel = dma_request_channel(controller, &requested);

#ifdef CONFIG_WCH_UART_ASYNC_TX
		if (channel >= 0) {
			dma_release_channel(controller, channel);
		}
		zassert_equal(channel, -EINVAL, "Async UART must reserve its fixed DMA channels");
		continue;
#endif
		if (channel != (int)requested) {
			if (channel >= 0) {
				dma_release_channel(controller, channel);
			}
			zassert_unreachable("Fixed channel %u was not allocated", requested);
		}
		int duplicate = dma_request_channel(controller, &requested);

		dma_release_channel(controller, channel);
		if (duplicate >= 0) {
			dma_release_channel(controller, duplicate);
		}
		zassert_equal(duplicate, -EINVAL, "A fixed channel must have one owner");
	}
}

ZTEST_SUITE(wch_uart_dma, NULL, NULL, NULL, NULL, NULL);
