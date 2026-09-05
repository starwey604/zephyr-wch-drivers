/* SPDX-License-Identifier: Apache-2.0 */
#include <zephyr/drivers/dma.h>
#include <zephyr/ztest.h>

BUILD_ASSERT(IS_ENABLED(CONFIG_WCH_DMA), "Exercise the external DMA driver");
BUILD_ASSERT(!IS_ENABLED(CONFIG_DMA_WCH), "Disable upstream DMA");

static const struct device *const controller =
	DEVICE_DT_GET(DT_DMAS_CTLR_BY_NAME(DT_ALIAS(test_uart0), tx));
static uint32_t source[64];
static uint32_t destination[64];
static int completion_status;
K_SEM_DEFINE(completed, 0, 1);

static void on_complete(const struct device *dev, void *user, uint32_t channel, int status)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(user);
	ARG_UNUSED(channel);
	completion_status = status;
	k_sem_give(&completed);
}

ZTEST(wch_dma_hardware, test_memory_copy)
{
	uint32_t requested = 0; /* DMA1 CH1: independent of the four USART requests. */

	zassert_true(device_is_ready(controller));
	int channel = dma_request_channel(controller, &requested);

	zassert_equal(channel, requested);
	for (uint32_t width = 1; width <= 4; width *= 2) {
		for (size_t i = 0; i < ARRAY_SIZE(source); i++) {
			source[i] = 0x12345678U ^ (i * 0x01010101U);
		}
		memset(destination, 0, sizeof(destination));
		k_sem_reset(&completed);
		completion_status = -EINPROGRESS;
		struct dma_block_config block = {
			.source_address = (uintptr_t)source,
			.dest_address = (uintptr_t)destination,
			.block_size = sizeof(source),
			.source_addr_adj = DMA_ADDR_ADJ_INCREMENT,
			.dest_addr_adj = DMA_ADDR_ADJ_INCREMENT,
		};
		struct dma_config config = {
			.channel_direction = MEMORY_TO_MEMORY,
			.source_data_size = width,
			.dest_data_size = width,
			.block_count = 1,
			.head_block = &block,
			.dma_callback = on_complete,
		};
		int ret = dma_config(controller, channel, &config);

		if (ret == 0) {
			ret = dma_start(controller, channel);
		}
		if (ret == 0) {
			ret = k_sem_take(&completed, K_SECONDS(1));
		}
		/* Always quiesce before asserting, including a missing IRQ/timeout. */
		int stop_ret = dma_stop(controller, channel);

		if (ret != 0 || completion_status != DMA_STATUS_COMPLETE || stop_ret != 0) {
			dma_release_channel(controller, channel);
			zassert_unreachable("DMA width %u: ret=%d callback=%d stop=%d", width, ret,
					    completion_status, stop_ret);
		}
		struct dma_status status;
		int status_ret = dma_get_status(controller, channel, &status);
		bool matches = memcmp(source, destination, sizeof(source)) == 0;

		if (status_ret != 0 || status.busy || status.pending_length != 0 || !matches) {
			dma_release_channel(controller, channel);
			zassert_unreachable("DMA width %u: status/progress/data mismatch", width);
		}
	}
	dma_release_channel(controller, channel);
}

ZTEST_SUITE(wch_dma_hardware, NULL, NULL, NULL, NULL, NULL);
