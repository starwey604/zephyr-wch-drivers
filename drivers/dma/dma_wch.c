/*
 * Copyright (c) 2024 Paul Wedeck <paulwedeck@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#define DT_DRV_COMPAT wch_wch_dma

#include <zephyr/drivers/dma.h>
#include <zephyr/drivers/clock_control.h>

#include <hal_ch32fun.h>

#define DMA_WCH_MAX_CHAN      11
#define DMA_WCH_MAX_CHAN_BASE 8

#define DMA_WCH_AIF        (DMA_GIF1 | DMA_TCIF1 | DMA_HTIF1 | DMA_TEIF1)
#define DMA_WCH_IF_OFF(ch) (4 * (ch))
#define DMA_WCH_MAX_BLOCK  UINT16_MAX

/* CH32V203 is single-core. IRQ exclusion also serializes reentrant callbacks. */
enum dma_wch_state {
	DMA_WCH_UNCONFIGURED,
	DMA_WCH_CONFIGURED,
	DMA_WCH_RUNNING,
	DMA_WCH_SUSPENDED,
};

struct dma_wch_chan_regs {
	volatile uint32_t CFGR;
	volatile uint32_t CNTR;
	volatile uint32_t PADDR;
	volatile uint32_t MADDR;
	volatile uint32_t reserved1;
};

struct dma_wch_regs {
	DMA_TypeDef base;
	struct dma_wch_chan_regs channels[DMA_WCH_MAX_CHAN];
	DMA_TypeDef ext;
};

struct dma_wch_config {
	struct dma_wch_regs *regs;
	uint32_t num_channels;
	const struct device *clock_dev;
	uint8_t clock_id;
	void (*irq_config_func)(const struct device *dev);
};

struct dma_wch_channel {
	void *user_data;
	dma_callback_t dma_cb;
	enum dma_wch_state state;
	uint32_t length;
	uint8_t width;
	bool error_callback_dis;
};

struct dma_wch_data {
	struct dma_context ctx;
	struct dma_wch_channel *channels;
};

static uint8_t dma_wch_get_ip(const struct device *dev, uint32_t chan)
{
	const struct dma_wch_config *config = dev->config;
	struct dma_wch_regs *regs = config->regs;
	uint32_t intfr;

	if (chan >= DMA_WCH_MAX_CHAN_BASE) {
		chan -= DMA_WCH_MAX_CHAN_BASE;
		intfr = regs->ext.INTFR;
		return (intfr >> DMA_WCH_IF_OFF(chan)) & DMA_WCH_AIF;
	}

	intfr = regs->base.INTFR;
	return (intfr >> DMA_WCH_IF_OFF(chan)) & DMA_WCH_AIF;
}

static bool dma_wch_busy(const struct device *dev, uint32_t ch)
{
	const struct dma_wch_data *data = dev->data;

	return data->channels[ch].state == DMA_WCH_RUNNING ||
	       data->channels[ch].state == DMA_WCH_SUSPENDED;
}

static void dma_wch_clear_flags(const struct device *dev, uint32_t ch)
{
	const struct dma_wch_config *config = dev->config;

	if (ch < DMA_WCH_MAX_CHAN_BASE) {
		config->regs->base.INTFCR = DMA_WCH_AIF << DMA_WCH_IF_OFF(ch);
	} else {
		config->regs->ext.INTFCR = DMA_WCH_AIF
					   << DMA_WCH_IF_OFF(ch - DMA_WCH_MAX_CHAN_BASE);
	}
}

static int dma_wch_init(const struct device *dev)
{
	const struct dma_wch_config *config = dev->config;
	clock_control_subsys_t clock_sys = (clock_control_subsys_t)(uintptr_t)config->clock_id;
	int ret;

	if (config->num_channels == 0 || config->num_channels > DMA_WCH_MAX_CHAN) {
		return -ENOTSUP;
	}

	if (!device_is_ready(config->clock_dev)) {
		return -ENODEV;
	}
	ret = clock_control_on(config->clock_dev, clock_sys);
	if (ret < 0) {
		return ret;
	}
	for (uint32_t ch = 0; ch < config->num_channels; ch++) {
		config->regs->channels[ch].CFGR = 0;
		dma_wch_clear_flags(dev, ch);
	}

	config->irq_config_func(dev);
	return 0;
}

/* Converts a transfer width in bytes to the corresponding bitfield */
static uint16_t dma_wch_width_index(uint32_t bytes)
{
	switch (bytes) {
	case 1:
		return 0;
	case 2:
		return 1;
	case 4:
		return 2;
	default:
		/* Callers validate widths before encoding them. */
		return 0;
	}
}

static int dma_wch_config(const struct device *dev, uint32_t ch, struct dma_config *dma_cfg)
{
	const struct dma_wch_config *config = dev->config;
	struct dma_wch_data *data = dev->data;
	struct dma_wch_regs *regs = config->regs;
	unsigned int key;
	int ret = 0;

	uint32_t cfgr = 0;
	uint32_t cntr = 0;
	uint32_t paddr = 0;
	uint32_t maddr = 0;

	if (config->num_channels <= ch || dma_cfg == NULL || dma_cfg->head_block == NULL) {
		return -EINVAL;
	}

	if (dma_cfg->block_count != 1 || dma_cfg->head_block->next_block != NULL ||
	    dma_cfg->dma_slot != 0 || dma_cfg->source_chaining_en || dma_cfg->dest_chaining_en ||
	    dma_cfg->source_handshake || dma_cfg->dest_handshake ||
	    dma_cfg->head_block->fifo_mode_control || dma_cfg->head_block->flow_control_mode) {
		return -ENOTSUP;
	}

	/* Equal widths make the API's byte length unambiguous; no packing/bursts. */
	uint32_t width = dma_cfg->source_data_size;

	if ((width != 1 && width != 2 && width != 4) || width != dma_cfg->dest_data_size ||
	    (dma_cfg->source_burst_length != 0 && dma_cfg->source_burst_length != width) ||
	    (dma_cfg->dest_burst_length != 0 && dma_cfg->dest_burst_length != width)) {
		return -ENOTSUP;
	}
	if (dma_cfg->head_block->block_size == 0 || dma_cfg->head_block->block_size % width != 0 ||
	    dma_cfg->head_block->block_size / width > DMA_WCH_MAX_BLOCK ||
	    dma_cfg->head_block->source_address % width != 0 ||
	    dma_cfg->head_block->dest_address % width != 0) {
		return -EINVAL;
	}
#ifdef CONFIG_DMA_64BIT
	if (dma_cfg->head_block->source_address > UINT32_MAX ||
	    dma_cfg->head_block->dest_address > UINT32_MAX) {
		return -EINVAL;
	}
#endif
	if (dma_cfg->head_block->source_gather_en || dma_cfg->head_block->dest_scatter_en ||
	    dma_cfg->head_block->source_reload_en || dma_cfg->channel_priority > 3 ||
	    (dma_cfg->head_block->source_addr_adj != DMA_ADDR_ADJ_INCREMENT &&
	     dma_cfg->head_block->source_addr_adj != DMA_ADDR_ADJ_NO_CHANGE) ||
	    (dma_cfg->head_block->dest_addr_adj != DMA_ADDR_ADJ_INCREMENT &&
	     dma_cfg->head_block->dest_addr_adj != DMA_ADDR_ADJ_NO_CHANGE) ||
	    dma_cfg->head_block->dest_reload_en) {
		return -ENOTSUP;
	}

	cntr = dma_cfg->head_block->block_size / width;

	switch (dma_cfg->channel_direction) {
	case MEMORY_TO_MEMORY:
		cfgr |= DMA_CFGR1_MEM2MEM;
		paddr = dma_cfg->head_block->source_address;
		maddr = dma_cfg->head_block->dest_address;

		if (dma_cfg->cyclic) {
			return -ENOTSUP;
		}
		break;
	case MEMORY_TO_PERIPHERAL:
		maddr = dma_cfg->head_block->source_address;
		paddr = dma_cfg->head_block->dest_address;
		cfgr |= DMA_CFGR1_DIR;
		break;
	case PERIPHERAL_TO_MEMORY:
		paddr = dma_cfg->head_block->source_address;
		maddr = dma_cfg->head_block->dest_address;
		break;
	default:
		return -ENOTSUP;
	}
	cfgr |= dma_cfg->channel_priority * DMA_CFGR1_PL_0;

	if (dma_cfg->channel_direction == MEMORY_TO_PERIPHERAL) {
		cfgr |= dma_wch_width_index(dma_cfg->source_data_size) * DMA_CFGR1_MSIZE_0;
		cfgr |= dma_wch_width_index(dma_cfg->dest_data_size) * DMA_CFGR1_PSIZE_0;

		cfgr |= (dma_cfg->head_block->dest_addr_adj == DMA_ADDR_ADJ_INCREMENT)
				? DMA_CFGR1_PINC
				: 0;
		cfgr |= (dma_cfg->head_block->source_addr_adj == DMA_ADDR_ADJ_INCREMENT)
				? DMA_CFGR1_MINC
				: 0;
	} else {
		cfgr |= dma_wch_width_index(dma_cfg->source_data_size) * DMA_CFGR1_PSIZE_0;
		cfgr |= dma_wch_width_index(dma_cfg->dest_data_size) * DMA_CFGR1_MSIZE_0;

		cfgr |= (dma_cfg->head_block->dest_addr_adj == DMA_ADDR_ADJ_INCREMENT)
				? DMA_CFGR1_MINC
				: 0;
		cfgr |= (dma_cfg->head_block->source_addr_adj == DMA_ADDR_ADJ_INCREMENT)
				? DMA_CFGR1_PINC
				: 0;
	}

	if (dma_cfg->cyclic) {
		cfgr |= DMA_CFGR1_CIRC;
	}

	/* Always service terminal events to clean state, even without a callback. */
	cfgr |= DMA_CFGR1_TEIE | DMA_CFGR1_TCIE;
	if (dma_cfg->dma_callback && dma_cfg->half_complete_callback_en) {
		cfgr |= DMA_CFGR1_HTIE;
	}

	key = irq_lock();

	if (dma_wch_busy(dev, ch)) {
		ret = -EBUSY;
		goto end;
	}

	data->channels[ch].user_data = dma_cfg->user_data;
	data->channels[ch].dma_cb = dma_cfg->dma_callback;
	data->channels[ch].state = DMA_WCH_CONFIGURED;
	data->channels[ch].width = width;
	data->channels[ch].length = dma_cfg->head_block->block_size;
	data->channels[ch].error_callback_dis = dma_cfg->error_callback_dis;

	regs->channels[ch].CFGR = 0;

	dma_wch_clear_flags(dev, ch);

	regs->channels[ch].PADDR = paddr;
	regs->channels[ch].MADDR = maddr;
	regs->channels[ch].CNTR = cntr;
	regs->channels[ch].CFGR = cfgr;
end:
	irq_unlock(key);
	return ret;
}

#ifdef CONFIG_DMA_64BIT
static int dma_wch_reload(const struct device *dev, uint32_t ch, uint64_t src, uint64_t dst,
			  size_t size)
#else
static int dma_wch_reload(const struct device *dev, uint32_t ch, uint32_t src, uint32_t dst,
			  size_t size)
#endif
{
	const struct dma_wch_config *config = dev->config;
	struct dma_wch_regs *regs = config->regs;
	uint32_t maddr, paddr;
	struct dma_wch_data *data = dev->data;
	int ret = 0;
	unsigned int key;

	if (config->num_channels <= ch) {
		return -EINVAL;
	}

	key = irq_lock();

	uint32_t width = data->channels[ch].width;

	if (data->channels[ch].state == DMA_WCH_UNCONFIGURED || size == 0 || size % width != 0 ||
	    size / width > DMA_WCH_MAX_BLOCK || src % width != 0 || dst % width != 0) {
		ret = -EINVAL;
		goto end;
	}
#ifdef CONFIG_DMA_64BIT
	if (src > UINT32_MAX || dst > UINT32_MAX) {
		ret = -EINVAL;
		goto end;
	}
#endif
	if (dma_wch_busy(dev, ch)) {
		ret = -EBUSY;
		goto end;
	}

	if (regs->channels[ch].CFGR & DMA_CFGR1_DIR) {
		maddr = src;
		paddr = dst;
	} else {
		maddr = dst;
		paddr = src;
	}

	regs->channels[ch].MADDR = maddr;
	regs->channels[ch].PADDR = paddr;
	regs->channels[ch].CNTR = size / width;
	data->channels[ch].length = size;
	dma_wch_clear_flags(dev, ch);
end:
	irq_unlock(key);
	return ret;
}

static int dma_wch_start(const struct device *dev, uint32_t ch)
{
	const struct dma_wch_config *config = dev->config;
	struct dma_wch_regs *regs = config->regs;
	struct dma_wch_data *data = dev->data;
	unsigned int key;

	if (config->num_channels <= ch) {
		return -EINVAL;
	}

	key = irq_lock();
	if (data->channels[ch].state == DMA_WCH_RUNNING) {
		irq_unlock(key);
		return 0;
	}
	if (data->channels[ch].state != DMA_WCH_CONFIGURED || regs->channels[ch].CNTR == 0) {
		irq_unlock(key);
		return -EINVAL;
	}
	dma_wch_clear_flags(dev, ch);
	data->channels[ch].state = DMA_WCH_RUNNING;
	regs->channels[ch].CFGR |= DMA_CFGR1_EN;
	irq_unlock(key);

	return 0;
}

static int dma_wch_stop(const struct device *dev, uint32_t ch)
{
	const struct dma_wch_config *config = dev->config;
	struct dma_wch_regs *regs = config->regs;
	struct dma_wch_data *data = dev->data;
	unsigned int key;

	if (config->num_channels <= ch) {
		return -EINVAL;
	}

	key = irq_lock();
	regs->channels[ch].CFGR &= ~DMA_CFGR1_EN;
	dma_wch_clear_flags(dev, ch);
	if (data->channels[ch].state != DMA_WCH_UNCONFIGURED) {
		data->channels[ch].state = DMA_WCH_CONFIGURED;
	}
	irq_unlock(key);

	return 0;
}

static int dma_wch_resume(const struct device *dev, uint32_t ch)
{
	const struct dma_wch_config *config = dev->config;
	struct dma_wch_regs *regs = config->regs;
	struct dma_wch_data *data = dev->data;
	int ret = 0;
	unsigned int key;

	if (config->num_channels <= ch) {
		return -EINVAL;
	}

	key = irq_lock();

	if (data->channels[ch].state == DMA_WCH_RUNNING) {
		goto end;
	}
	if (data->channels[ch].state != DMA_WCH_SUSPENDED) {
		ret = -EINVAL;
		goto end;
	}

	regs->channels[ch].CFGR |= DMA_CFGR1_EN;
	data->channels[ch].state = DMA_WCH_RUNNING;
end:
	irq_unlock(key);
	return ret;
}

static int dma_wch_suspend(const struct device *dev, uint32_t ch)
{
	const struct dma_wch_config *config = dev->config;
	struct dma_wch_regs *regs = config->regs;
	struct dma_wch_data *data = dev->data;
	int ret = 0;
	unsigned int key;

	if (config->num_channels <= ch) {
		return -EINVAL;
	}

	key = irq_lock();

	if (data->channels[ch].state == DMA_WCH_SUSPENDED) {
		goto end;
	}
	if (data->channels[ch].state != DMA_WCH_RUNNING) {
		ret = -EINVAL;
		goto end;
	}

	regs->channels[ch].CFGR &= ~DMA_CFGR1_EN;
	data->channels[ch].state = DMA_WCH_SUSPENDED;
end:
	irq_unlock(key);
	return ret;
}

static int dma_wch_get_status(const struct device *dev, uint32_t ch, struct dma_status *status)
{
	const struct dma_wch_config *config = dev->config;
	struct dma_wch_regs *regs = config->regs;
	const struct dma_wch_data *data = dev->data;
	uint32_t cfgr;
	unsigned int key;

	if (config->num_channels <= ch || status == NULL) {
		return -EINVAL;
	}

	key = irq_lock();
	cfgr = regs->channels[ch].CFGR;
	*status = (struct dma_status){0};

	status->busy = dma_wch_busy(dev, ch);
	if (cfgr & DMA_CFGR1_MEM2MEM) {
		status->dir = MEMORY_TO_MEMORY;
	} else if (cfgr & DMA_CFGR1_DIR) {
		status->dir = MEMORY_TO_PERIPHERAL;
	} else {
		status->dir = PERIPHERAL_TO_MEMORY;
	}

	status->pending_length = regs->channels[ch].CNTR * data->channels[ch].width;
	if ((cfgr & DMA_CFGR1_CIRC) && data->channels[ch].length != 0) {
		uint32_t position = (data->channels[ch].length - status->pending_length) %
				    data->channels[ch].length;

		if (cfgr & DMA_CFGR1_DIR) {
			status->read_position = (cfgr & DMA_CFGR1_MINC) ? position : 0;
			status->write_position = (cfgr & DMA_CFGR1_PINC) ? position : 0;
		} else {
			status->read_position = (cfgr & DMA_CFGR1_PINC) ? position : 0;
			status->write_position = (cfgr & DMA_CFGR1_MINC) ? position : 0;
		}
	}
	irq_unlock(key);
	return 0;
}

static int dma_wch_get_attribute(const struct device *dev, uint32_t type, uint32_t *value)
{
	ARG_UNUSED(dev);
	if (value == NULL) {
		return -EINVAL;
	}
	switch (type) {
	case DMA_ATTR_BUFFER_ADDRESS_ALIGNMENT:
	case DMA_ATTR_BUFFER_SIZE_ALIGNMENT:
	case DMA_ATTR_COPY_ALIGNMENT:
	case DMA_ATTR_MAX_BLOCK_COUNT:
		*value = 1;
		return 0;
	default:
		return -EINVAL;
	}
}

/* The filter parameter is a pointer to a zero-based uint32_t channel index.
 * NULL requests any available channel. Allocation itself is owned by Zephyr.
 */
static bool dma_wch_chan_filter(const struct device *dev, int ch, void *param)
{
	const struct dma_wch_config *config = dev->config;

	return ch >= 0 && (uint32_t)ch < config->num_channels &&
	       (param == NULL || *(const uint32_t *)param == (uint32_t)ch);
}

static void dma_wch_chan_release(const struct device *dev, uint32_t ch)
{
	const struct dma_wch_config *config = dev->config;
	struct dma_wch_data *data = dev->data;
	unsigned int key;

	if (ch >= config->num_channels) {
		return;
	}
	key = irq_lock();
	config->regs->channels[ch].CFGR = 0;
	dma_wch_clear_flags(dev, ch);
	data->channels[ch] = (struct dma_wch_channel){0};
	irq_unlock(key);
}

static void dma_wch_isr(const struct device *dev, uint32_t ch)
{
	const struct dma_wch_config *config = dev->config;
	struct dma_wch_data *data = dev->data;
	struct dma_wch_channel *channel = &data->channels[ch];
	struct dma_wch_chan_regs *regs = &config->regs->channels[ch];
	unsigned int key = irq_lock();
	uint8_t ip = dma_wch_get_ip(dev, ch) & ~DMA_GIF1;
	uint32_t cfgr = regs->CFGR;
	dma_callback_t cb = channel->dma_cb;
	void *user_data = channel->user_data;

	/* Clear only the snapshot: do not erase a flag arriving during service. */
	if (ch < DMA_WCH_MAX_CHAN_BASE) {
		config->regs->base.INTFCR = ip << DMA_WCH_IF_OFF(ch);
	} else {
		config->regs->ext.INTFCR = ip << DMA_WCH_IF_OFF(ch - DMA_WCH_MAX_CHAN_BASE);
	}
	if (channel->state != DMA_WCH_RUNNING && channel->state != DMA_WCH_SUSPENDED) {
		goto end;
	}

	/* Error dominates simultaneous HT/TC. Cleanup is independent of callback policy. */
	if (ip & DMA_TEIF1) {
		regs->CFGR &= ~DMA_CFGR1_EN;
		channel->state = DMA_WCH_CONFIGURED;
		if (cb != NULL && !channel->error_callback_dis) {
			cb(dev, user_data, ch, -EIO);
		}
		goto end;
	}
	if (ip & DMA_TCIF1) {
		if (!(cfgr & DMA_CFGR1_CIRC)) {
			regs->CFGR &= ~DMA_CFGR1_EN;
			channel->state = DMA_WCH_CONFIGURED;
		}
		/* Coalesced HT+TC reports TC only; old half events cannot be replayed
		 * safely if the callback stops/reconfigures this channel.
		 */
		if (cb != NULL && (cfgr & DMA_CFGR1_TCIE)) {
			cb(dev, user_data, ch, DMA_STATUS_COMPLETE);
		}
	} else if ((ip & DMA_HTIF1) && (cfgr & DMA_CFGR1_HTIE) && cb != NULL) {
		cb(dev, user_data, ch, DMA_STATUS_HALF_COMPLETE);
	}
end:
	irq_unlock(key);
}

static DEVICE_API(dma, dma_wch_driver_api) = {
	.config = dma_wch_config,
	.reload = dma_wch_reload,
	.start = dma_wch_start,
	.stop = dma_wch_stop,
	.resume = dma_wch_resume,
	.suspend = dma_wch_suspend,
	.get_status = dma_wch_get_status,
	.get_attribute = dma_wch_get_attribute,
	.chan_filter = dma_wch_chan_filter,
	.chan_release = dma_wch_chan_release,
};

#define GENERATE_ISR(ch, _)                                                                        \
	__used static void dma_wch_isr##ch(const struct device *dev)                               \
	{                                                                                          \
		dma_wch_isr(dev, ch);                                                              \
	}

LISTIFY(DMA_WCH_MAX_CHAN, GENERATE_ISR, ())

#define IRQ_CONFIGURE(n, idx)                                                                      \
	do {                                                                                       \
		IRQ_CONNECT(DT_INST_IRQ_BY_IDX(idx, n, irq), DT_INST_IRQ_BY_IDX(idx, n, priority), \
			    dma_wch_isr##n, DEVICE_DT_INST_GET(idx), 0);                           \
		irq_enable(DT_INST_IRQ_BY_IDX(idx, n, irq));                                       \
	} while (false)

#define CONFIGURE_ALL_IRQS(idx, n) LISTIFY(n, IRQ_CONFIGURE, (;), idx)

#define WCH_DMA_INIT(idx)                                                                          \
	BUILD_ASSERT(DT_NUM_IRQS(DT_DRV_INST(idx)) == DT_INST_PROP(idx, dma_channels),             \
		     "WCH DMA requires one IRQ per channel");                                      \
	static void dma_wch##idx##_irq_config(const struct device *dev)                            \
	{                                                                                          \
		CONFIGURE_ALL_IRQS(idx, DT_NUM_IRQS(DT_DRV_INST(idx)));                            \
	}                                                                                          \
	static const struct dma_wch_config dma_wch##idx##_config = {                               \
		.regs = (struct dma_wch_regs *)DT_INST_REG_ADDR(idx),                              \
		.num_channels = DT_INST_PROP(idx, dma_channels),                                   \
		.clock_dev = DEVICE_DT_GET(DT_INST_CLOCKS_CTLR(idx)),                              \
		.clock_id = DT_INST_CLOCKS_CELL(idx, id),                                          \
		.irq_config_func = dma_wch##idx##_irq_config,                                      \
	};                                                                                         \
	static struct dma_wch_channel dma_wch##idx##_channels[DT_INST_PROP(idx, dma_channels)];    \
	ATOMIC_DEFINE(dma_wch_atomic##idx, DT_INST_PROP(idx, dma_channels));                       \
	static struct dma_wch_data dma_wch##idx##_data = {                                         \
		.ctx =                                                                             \
			{                                                                          \
				.magic = DMA_MAGIC,                                                \
				.atomic = dma_wch_atomic##idx,                                     \
				.dma_channels = DT_INST_PROP(idx, dma_channels),                   \
			},                                                                         \
		.channels = dma_wch##idx##_channels,                                               \
	};                                                                                         \
                                                                                                   \
	DEVICE_DT_INST_DEFINE(idx, dma_wch_init, NULL, &dma_wch##idx##_data,                       \
			      &dma_wch##idx##_config, PRE_KERNEL_1, CONFIG_DMA_INIT_PRIORITY,      \
			      &dma_wch_driver_api);

DT_INST_FOREACH_STATUS_OKAY(WCH_DMA_INIT)
