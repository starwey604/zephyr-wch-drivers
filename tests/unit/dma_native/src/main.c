/* SPDX-License-Identifier: Apache-2.0 */
#include <zephyr/ztest.h>

/* Exercise the production implementation with real Zephyr DMA APIs and atomic
 * allocation, replacing only MMIO/HAL. native_sim has no matching DT instance.
 */
#include "dma_wch.c"

static struct dma_wch_regs registers;
static struct dma_wch_channel channels[DMA_WCH_MAX_CHAN];
ATOMIC_DEFINE(allocation, DMA_WCH_MAX_CHAN);
static struct dma_wch_data data = {
	.ctx = {.magic = DMA_MAGIC, .dma_channels = DMA_WCH_MAX_CHAN, .atomic = allocation},
	.channels = channels,
};
static const struct dma_wch_config config = {
	.regs = &registers,
	.num_channels = DMA_WCH_MAX_CHAN,
};
DEVICE_DEFINE(test_dma, "test_dma", NULL, NULL, &data, &config, POST_KERNEL,
	      CONFIG_DMA_INIT_PRIORITY, &dma_wch_driver_api);
static const struct device *const dev = DEVICE_GET(test_dma);
static struct dma_block_config block;
static struct dma_config transfer;
static int events[8];
static size_t event_count;
static bool stop_in_callback;
static bool restart_in_callback;
static int clock_result;
static unsigned int irq_setup_count;

static int clock_on(const struct device *device, clock_control_subsys_t subsys)
{
	ARG_UNUSED(device);
	ARG_UNUSED(subsys);
	return clock_result;
}

static DEVICE_API(clock_control, test_clock_api) = {.on = clock_on};
DEVICE_DEFINE(test_clock, "test_clock", NULL, NULL, NULL, NULL, PRE_KERNEL_1, 0, &test_clock_api);

static void irq_setup(const struct device *device)
{
	ARG_UNUSED(device);
	irq_setup_count++;
}

ZTEST(wch_dma, test_init_rejects_missing_clock)
{
	/* Also ensures the production init function is compiled in the native test. */
	zassert_equal(dma_wch_init(dev), -ENODEV);
}

ZTEST(wch_dma, test_init_clock_failure_and_reset)
{
	struct dma_wch_config cfg = config;
	const struct device device = {.config = &cfg, .data = &data};

	cfg.clock_dev = DEVICE_GET(test_clock);
	cfg.irq_config_func = irq_setup;
	clock_result = -EIO;
	irq_setup_count = 0;
	zassert_equal(dma_wch_init(&device), -EIO);
	zassert_equal(irq_setup_count, 0);
	clock_result = 0;
	registers.channels[0].CFGR = DMA_CFGR1_EN;
	zassert_ok(dma_wch_init(&device));
	zassert_equal(irq_setup_count, 1);
	zassert_equal(registers.channels[0].CFGR, 0);
	cfg.num_channels = 0;
	zassert_equal(dma_wch_init(&device), -ENOTSUP);
	cfg.num_channels = DMA_WCH_MAX_CHAN + 1;
	zassert_equal(dma_wch_init(&device), -ENOTSUP);
}

static void callback(const struct device *device, void *user, uint32_t ch, int status)
{
	zassert_equal_ptr(device, dev);
	zassert_equal_ptr(user, &event_count);
	zassert_true(event_count < ARRAY_SIZE(events));
	events[event_count++] = status;
	if (stop_in_callback) {
		zassert_ok(dma_stop(device, ch));
	}
	if (restart_in_callback && event_count == 1) {
		block.block_size = 16;
		zassert_ok(dma_config(device, ch, &transfer));
		zassert_ok(dma_start(device, ch));
	}
}

static void apply_clear(DMA_TypeDef *bank)
{
	uint32_t flags = bank->INTFCR;

	for (unsigned int ch = 0; ch < DMA_WCH_MAX_CHAN_BASE; ch++) {
		if (flags & (DMA_GIF1 << DMA_WCH_IF_OFF(ch))) {
			flags |= DMA_WCH_AIF << DMA_WCH_IF_OFF(ch);
		}
	}
	bank->INTFR &= ~flags;
	bank->INTFCR = 0;
}

static void inject(uint32_t ch, uint32_t flags)
{
	DMA_TypeDef *bank = ch < DMA_WCH_MAX_CHAN_BASE ? &registers.base : &registers.ext;
	uint32_t idx = ch % DMA_WCH_MAX_CHAN_BASE;

	apply_clear(bank);
	bank->INTFR |= flags << DMA_WCH_IF_OFF(idx);
	dma_wch_isr(dev, ch);
	apply_clear(bank);
}

static void before(void *fixture)
{
	ARG_UNUSED(fixture);
	memset(&registers, 0, sizeof(registers));
	memset(channels, 0, sizeof(channels));
	memset(allocation, 0, sizeof(allocation));
	event_count = 0;
	stop_in_callback = false;
	restart_in_callback = false;
	block = (struct dma_block_config){
		.source_address = 0x20000000,
		.dest_address = 0x40013804,
		.block_size = 32,
		.source_addr_adj = DMA_ADDR_ADJ_INCREMENT,
		.dest_addr_adj = DMA_ADDR_ADJ_NO_CHANGE,
	};
	transfer = (struct dma_config){
		.channel_direction = MEMORY_TO_PERIPHERAL,
		.source_data_size = 1,
		.dest_data_size = 1,
		.block_count = 1,
		.head_block = &block,
		.dma_callback = callback,
		.user_data = &event_count,
	};
}

ZTEST(wch_dma, test_fixed_channel_allocation_and_release)
{
	uint32_t requested[] = {3, 4, 6, 5};

	ARRAY_FOR_EACH(requested, i) {
		zassert_equal(dma_request_channel(dev, &requested[i]), requested[i]);
		zassert_equal(dma_request_channel(dev, &requested[i]), -EINVAL);
	}
	uint32_t invalid = DMA_WCH_MAX_CHAN;

	zassert_equal(dma_request_channel(dev, &invalid), -EINVAL);
	zassert_equal(dma_request_channel(dev, NULL), 0);
	zassert_ok(dma_config(dev, 3, &transfer));
	zassert_ok(dma_start(dev, 3));
	dma_release_channel(dev, 3);
	zassert_equal(registers.channels[3].CFGR, 0);
	inject(3, DMA_TCIF1 | DMA_TEIF1);
	zassert_equal(event_count, 0);
	zassert_equal(dma_start(dev, 3), -EINVAL);
	zassert_equal(dma_request_channel(dev, &requested[0]), 3);
}

ZTEST(wch_dma, test_lengths_widths_and_reload)
{
	for (uint32_t width = 1; width <= 4; width *= 2) {
		transfer.source_data_size = width;
		transfer.dest_data_size = width;
		block.block_size = UINT16_MAX * width;
		zassert_ok(dma_config(dev, 0, &transfer));
		zassert_equal(registers.channels[0].CNTR, UINT16_MAX);
		struct dma_status status;

		zassert_ok(dma_get_status(dev, 0, &status));
		zassert_equal(status.pending_length, block.block_size);
		block.block_size += width;
		zassert_equal(dma_config(dev, 0, &transfer), -EINVAL);
		zassert_equal(dma_reload(dev, 0, 0x20000000, 0x40013804, block.block_size),
			      -EINVAL);
		zassert_ok(dma_reload(dev, 0, 0x20000000, 0x40013804, 16));
		zassert_equal(registers.channels[0].CNTR, 16 / width);
	}
	block.block_size = 0;
	zassert_equal(dma_config(dev, 0, &transfer), -EINVAL);
	block.block_size = 31;
	zassert_equal(dma_config(dev, 0, &transfer), -EINVAL);
	block.block_size = 32;
	block.source_address++;
	zassert_equal(dma_config(dev, 0, &transfer), -EINVAL);
	block.source_address = UINT64_C(0x100000000);
	zassert_equal(dma_config(dev, 0, &transfer), -EINVAL);
	zassert_equal(dma_reload(dev, 0, block.source_address, 0x40013804, 32), -EINVAL);
	block.source_address = 0x20000000;
	transfer.source_data_size = 3;
	zassert_equal(dma_config(dev, 0, &transfer), -ENOTSUP);
	transfer.source_data_size = 2;
	zassert_equal(dma_config(dev, 0, &transfer), -ENOTSUP);
}

ZTEST(wch_dma, test_invalid_configuration_and_state)
{
	zassert_equal(dma_start(dev, 0), -EINVAL);
	zassert_equal(dma_resume(dev, 0), -EINVAL);
	zassert_equal(dma_suspend(dev, 0), -EINVAL);
	zassert_equal(dma_reload(dev, 0, 0, 0, 1), -EINVAL);
	zassert_equal(dma_config(dev, 0, NULL), -EINVAL);
	zassert_equal(dma_config(dev, DMA_WCH_MAX_CHAN, &transfer), -EINVAL);
	transfer.head_block = NULL;
	zassert_equal(dma_config(dev, 0, &transfer), -EINVAL);
	transfer.head_block = &block;
	transfer.block_count = 2;
	zassert_equal(dma_config(dev, 0, &transfer), -ENOTSUP);
	transfer.block_count = 1;
	transfer.channel_direction = MEMORY_TO_MEMORY;
	transfer.cyclic = true;
	zassert_equal(dma_config(dev, 0, &transfer), -ENOTSUP);
	transfer.cyclic = false;
	zassert_ok(dma_config(dev, 0, &transfer));
	zassert_ok(dma_start(dev, 0));
	zassert_ok(dma_start(dev, 0));
	zassert_equal(dma_config(dev, 0, &transfer), -EBUSY);
	zassert_equal(dma_reload(dev, 0, 0, 0, 32), -EBUSY);
	zassert_ok(dma_suspend(dev, 0));
	zassert_ok(dma_suspend(dev, 0));
	zassert_equal(dma_start(dev, 0), -EINVAL);
	zassert_equal(dma_config(dev, 0, &transfer), -EBUSY);
	zassert_ok(dma_resume(dev, 0));
	zassert_ok(dma_resume(dev, 0));
	zassert_ok(dma_stop(dev, 0));
	zassert_ok(dma_stop(dev, 0));
	zassert_equal(dma_resume(dev, 0), -EINVAL);
}

ZTEST(wch_dma, test_normal_and_circular_completion)
{
	struct dma_status status;

	/* Includes the inherited extended IRQ bank; target CH32V203 DMA1 uses 0..6. */
	for (uint32_t ch = 0; ch < DMA_WCH_MAX_CHAN; ch++) {
		for (unsigned int cyclic = 0; cyclic <= 1; cyclic++) {
			transfer.cyclic = cyclic;
			event_count = 0;
			zassert_ok(dma_config(dev, ch, &transfer));
			zassert_ok(dma_start(dev, ch));
			registers.channels[ch].CNTR = cyclic ? block.block_size : 0;
			inject(ch, DMA_TCIF1);
			zassert_equal(event_count, 1);
			zassert_equal(events[0], DMA_STATUS_COMPLETE);
			zassert_equal(!!(registers.channels[ch].CFGR & DMA_CFGR1_EN), cyclic);
			zassert_ok(dma_get_status(dev, ch, &status));
			zassert_equal(status.busy, cyclic);
			if (cyclic) {
				inject(ch, DMA_TCIF1);
				zassert_equal(event_count, 2);
				registers.channels[ch].CNTR = 12;
				zassert_ok(dma_get_status(dev, ch, &status));
				zassert_equal(status.read_position, 20);
				zassert_equal(status.write_position, 0);
			}
			zassert_ok(dma_stop(dev, ch));
		}
	}
}

ZTEST(wch_dma, test_error_precedence_and_suppression)
{
	for (unsigned int suppress = 0; suppress <= 1; suppress++) {
		transfer.error_callback_dis = suppress;
		transfer.cyclic = true;
		event_count = 0;
		zassert_ok(dma_config(dev, 0, &transfer));
		zassert_ok(dma_start(dev, 0));
		inject(0, DMA_TEIF1 | DMA_TCIF1 | DMA_HTIF1);
		zassert_equal(event_count, !suppress);
		if (!suppress) {
			zassert_equal(events[0], -EIO);
		}
		zassert_false(registers.channels[0].CFGR & DMA_CFGR1_EN);
		zassert_ok(dma_config(dev, 0, &transfer));
	}
	transfer.dma_callback = NULL;
	zassert_ok(dma_config(dev, 0, &transfer));
	zassert_ok(dma_start(dev, 0));
	inject(0, DMA_TEIF1);
	zassert_equal(channels[0].state, DMA_WCH_CONFIGURED);
}

ZTEST(wch_dma, test_half_callback_and_reentrant_stop)
{
	transfer.complete_callback_en = true;
	zassert_ok(dma_config(dev, 0, &transfer));
	zassert_false(registers.channels[0].CFGR & DMA_CFGR1_HTIE);
	transfer.half_complete_callback_en = true;
	transfer.cyclic = true;
	zassert_ok(dma_config(dev, 0, &transfer));
	zassert_ok(dma_start(dev, 0));
	inject(0, DMA_HTIF1);
	zassert_equal(event_count, 1);
	zassert_equal(events[0], DMA_STATUS_HALF_COMPLETE);
	stop_in_callback = true;
	inject(0, DMA_HTIF1 | DMA_TCIF1);
	zassert_equal(event_count, 2);
	zassert_equal(events[1], DMA_STATUS_COMPLETE);
	zassert_false(registers.channels[0].CFGR & DMA_CFGR1_EN);
	inject(0, DMA_TCIF1);
	zassert_equal(event_count, 2);
	zassert_ok(dma_reload(dev, 0, 0x20000000, 0x40013804, 16));
	zassert_ok(dma_start(dev, 0));
	inject(0, DMA_TCIF1);
	zassert_equal(event_count, 3);
}

ZTEST(wch_dma, test_reentrant_reconfiguration)
{
	restart_in_callback = true;
	zassert_ok(dma_config(dev, 0, &transfer));
	zassert_ok(dma_start(dev, 0));
	registers.channels[0].CNTR = 0;
	inject(0, DMA_TCIF1 | DMA_HTIF1);
	zassert_equal(event_count, 1);
	zassert_equal(channels[0].state, DMA_WCH_RUNNING);
	zassert_equal(registers.channels[0].CNTR, 16);
	zassert_true(registers.channels[0].CFGR & DMA_CFGR1_EN);
	registers.channels[0].CNTR = 0;
	inject(0, DMA_TCIF1);
	zassert_equal(event_count, 2);
	zassert_equal(channels[0].state, DMA_WCH_CONFIGURED);
}

ZTEST(wch_dma, test_stop_clears_pending_before_restart)
{
	zassert_ok(dma_config(dev, 0, &transfer));
	zassert_ok(dma_start(dev, 0));
	apply_clear(&registers.base);
	registers.base.INTFR = DMA_TCIF1 | DMA_TEIF1;
	zassert_ok(dma_stop(dev, 0));
	apply_clear(&registers.base);
	zassert_equal(registers.base.INTFR, 0);
	zassert_ok(dma_reload(dev, 0, 0x20000000, 0x40013804, 16));
	zassert_ok(dma_start(dev, 0));
	inject(0, 0); /* Late PFIC entry after peripheral flags were cleared. */
	zassert_equal(event_count, 0);
	inject(0, DMA_TCIF1);
	zassert_equal(event_count, 1);
}

ZTEST(wch_dma, test_rx_mapping_and_byte_progress)
{
	transfer.channel_direction = PERIPHERAL_TO_MEMORY;
	transfer.cyclic = true;
	transfer.source_data_size = 2;
	transfer.dest_data_size = 2;
	block.source_address = 0x40013804;
	block.dest_address = 0x20000000;
	block.source_addr_adj = DMA_ADDR_ADJ_NO_CHANGE;
	block.dest_addr_adj = DMA_ADDR_ADJ_INCREMENT;
	zassert_ok(dma_config(dev, 4, &transfer));
	zassert_equal(registers.channels[4].PADDR, block.source_address);
	zassert_equal(registers.channels[4].MADDR, block.dest_address);
	zassert_ok(dma_start(dev, 4));
	registers.channels[4].CNTR = 6;
	struct dma_status status;

	zassert_ok(dma_get_status(dev, 4, &status));
	zassert_equal(status.pending_length, 12);
	zassert_equal(status.read_position, 0);
	zassert_equal(status.write_position, 20);
	zassert_true(status.busy);
	/* TC pending in cyclic mode must not allow configuration/reload. */
	registers.base.INTFR = DMA_TCIF1 << DMA_WCH_IF_OFF(4);
	zassert_equal(dma_config(dev, 4, &transfer), -EBUSY);
	zassert_equal(dma_reload(dev, 4, block.source_address, block.dest_address, 32), -EBUSY);
}

ZTEST(wch_dma, test_irq_clear_preserves_other_channels)
{
	zassert_ok(dma_config(dev, 0, &transfer));
	zassert_ok(dma_start(dev, 0));
	apply_clear(&registers.base);
	registers.base.INTFR = DMA_TCIF1 | DMA_GIF1 | (DMA_TEIF1 << DMA_WCH_IF_OFF(1));
	dma_wch_isr(dev, 0);
	zassert_equal(registers.base.INTFCR, DMA_TCIF1);
	apply_clear(&registers.base);
	zassert_true(registers.base.INTFR & (DMA_TEIF1 << DMA_WCH_IF_OFF(1)));
	zassert_equal(event_count, 1);
}

ZTEST_SUITE(wch_dma, NULL, NULL, before, NULL, NULL);
