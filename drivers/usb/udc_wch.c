/*
 * Copyright (c) 2025 Bootlin
 * SPDX-License-Identifier: Apache-2.0
 */
/* Based on the USBFS register operations in BOJIT/zephyr dc53b3104fbbe6d,
 * drivers/usb/udc/udc_wch.c. Transfer/control/lifetime handling is rewritten
 * for Zephyr 4.4's udc_setup_received API. See docs/usb-udc.md.
 */
#define DT_DRV_COMPAT wch_ch32v203_usbfs
#include "udc_common.h"
#include <zephyr/sys/sys_io.h>
#include <zephyr/irq.h>
#include <string.h>

/* USBFS byte register offsets (not the separate USBD controller). */
#define WCH_CTRL        0x00
#define WCH_PORT        0x01
#define WCH_INT_EN      0x02
#define WCH_ADDR        0x03
#define WCH_MIS_ST      0x05
#define WCH_INT_FG      0x06
#define WCH_INT_ST      0x07
#define WCH_RX_LEN      0x08
#define WCH_MODE1       0x0c
#define WCH_MODE2       0x0d
#define WCH_DMA(ep)     (0x10 + (ep) * 4)
#define WCH_TX_LEN(ep)  (0x30 + (ep) * 4)
#define WCH_TX_CTL(ep)  (0x32 + (ep) * 4)
#define WCH_RX_CTL(ep)  (0x33 + (ep) * 4)
#define WCH_RESET       BIT(0)
#define WCH_TRANSFER    BIT(1)
#define WCH_SUSPEND     BIT(2)
#define WCH_FIFO_OV     BIT(4)
#define WCH_IRQS        (WCH_RESET | WCH_TRANSFER | WCH_SUSPEND | WCH_FIFO_OV)
#define WCH_ACK         0
#define WCH_NAK         2
#define WCH_STALL       3
#define WCH_TOGGLE      BIT(2)
#define WCH_TOG_OK      BIT(6)
#define WCH_TOKEN_OUT   0x00
#define WCH_TOKEN_IN    0x20
#define WCH_TOKEN_SETUP 0x30
#define WCH_PACKET      64

struct wch_udc_config {
	uintptr_t base;
	struct udc_ep_config *eps;
	int (*prepare)(const struct device *dev);
	void (*unprepare)(const struct device *dev);
	void (*irq_config)(const struct device *dev);
	const struct device *clock;
	uint32_t clock_id;
	uint32_t pll_hz;
	bool release_sdi;
};

struct wch_udc_data {
	struct k_work work;
	const struct device *dev;
	/* Never DMA directly to a net_buf: SETUP may preempt EP0 at any time,
	 * and short OUT allocations can be smaller than a hardware packet.
	 */
	uint8_t packet[3][WCH_PACKET] __aligned(4);
	uint8_t in_length[3];
	bool running;
	uint32_t saved_sdi;
	uint32_t saved_usbpre;
};

static uint8_t wch_read(const struct device *dev, unsigned int offset)
{
	const struct wch_udc_config *config = dev->config;

	return sys_read8(config->base + offset);
}

static void wch_write(const struct device *dev, unsigned int offset, uint8_t value)
{
	const struct wch_udc_config *config = dev->config;

	sys_write8(value, config->base + offset);
}

static void wch_ack(const struct device *dev, uint8_t flags)
{
#ifdef CONFIG_WCH_UDC_NATIVE_TEST
	/* RAM has no W1C behavior; only this MMIO side effect is modeled. */
	wch_write(dev, WCH_INT_FG, wch_read(dev, WCH_INT_FG) & ~flags);
#else
	wch_write(dev, WCH_INT_FG, flags);
#endif
}

static void wch_response(const struct device *dev, uint8_t ep, uint8_t response)
{
	unsigned int offset = USB_EP_DIR_IS_IN(ep) ? WCH_TX_CTL(USB_EP_GET_IDX(ep))
						   : WCH_RX_CTL(USB_EP_GET_IDX(ep));

	wch_write(dev, offset, (wch_read(dev, offset) & WCH_TOGGLE) | response);
}

static bool wch_setup_pending(const struct device *dev)
{
	return (wch_read(dev, WCH_INT_FG) & WCH_TRANSFER) &&
	       ((wch_read(dev, WCH_INT_ST) & 0x3f) == WCH_TOKEN_SETUP);
}

static void wch_kick(const struct device *dev, struct udc_ep_config *cfg)
{
	const struct wch_udc_config *config = dev->config;
	struct wch_udc_data *priv = udc_get_private(dev);
	struct net_buf *buf = udc_buf_peek(cfg);
	unsigned int idx = USB_EP_GET_IDX(cfg->addr);

	if (!priv->running || !cfg->stat.enabled || cfg->stat.halted || udc_ep_is_busy(cfg) ||
	    buf == NULL || (idx == 0 && wch_setup_pending(dev))) {
		return;
	}
	if (udc_get_buf_info(buf)->setup) {
		/* SETUP is always received into dedicated EP0 staging RAM. */
		return;
	}
	if (USB_EP_DIR_IS_IN(cfg->addr)) {
		size_t len = MIN(buf->len, cfg->mps);

		memcpy(priv->packet[idx], buf->data, len);
		priv->in_length[idx] = len;
		sys_write16(len, config->base + WCH_TX_LEN(idx));
	}
	udc_ep_set_busy(cfg, true);
	wch_response(dev, cfg->addr, WCH_ACK);
}

/* Quiesce this endpoint and discard its latched completion before cancellation
 * or reuse. SETUP is never discarded here; it supersedes both EP0 directions.
 */
static void wch_transfer(const struct device *dev, uint8_t token);

static int wch_quiesce(const struct device *dev, struct udc_ep_config *cfg, bool cancel)
{
	struct wch_udc_data *priv = udc_get_private(dev);
	unsigned int key = irq_lock();
	uint8_t expected = USB_EP_GET_IDX(cfg->addr) |
			   (USB_EP_DIR_IS_IN(cfg->addr) ? WCH_TOKEN_IN : WCH_TOKEN_OUT);

	wch_response(dev, cfg->addr, WCH_NAK);
	/* Finish an already accepted hardware token before reusing staging RAM.
	 * SIE_FREE timing must be measured on silicon. Fail without returning
	 * the loan if it does not settle within this bounded packet window.
	 */
	for (unsigned int retry = 0; priv->running && !(wch_read(dev, WCH_MIS_ST) & BIT(5));
	     retry++) {
		if (retry == 100) {
			irq_unlock(key);
			return -EBUSY;
		}
		k_busy_wait(1);
	}
	uint8_t token = wch_read(dev, WCH_INT_ST);
	if ((wch_read(dev, WCH_INT_FG) & WCH_TRANSFER) && (token & 0x3f) == expected) {
		if (!cancel) {
			/* A functional halt must preserve an already ACKed packet's
			 * progress, unlike cancellation of the whole request.
			 */
			wch_transfer(dev, token);
			wch_response(dev, cfg->addr, WCH_NAK);
		}
		wch_ack(dev, WCH_TRANSFER);
	}
	udc_ep_set_busy(cfg, false);
	irq_unlock(key);
	return 0;
}

static void wch_complete(const struct device *dev, struct udc_ep_config *cfg, int err)
{
	struct net_buf *buf = udc_buf_get(cfg);

	udc_ep_set_busy(cfg, false);
	if (buf != NULL) {
		udc_submit_ep_event(dev, buf, err);
	}
}

static void wch_transfer(const struct device *dev, uint8_t token)
{
	const struct wch_udc_config *config = dev->config;
	struct wch_udc_data *priv = udc_get_private(dev);
	unsigned int idx = token & 0x0f;
	uint8_t pid = token & 0x30;
	uint8_t ep = idx | (pid == WCH_TOKEN_IN ? USB_EP_DIR_IN : 0);
	struct udc_ep_config *cfg = udc_get_ep_cfg(dev, ep);
	struct net_buf *buf;

	if (pid == WCH_TOKEN_SETUP && idx == 0) {
		struct udc_ep_config *in = udc_get_ep_cfg(dev, USB_CONTROL_EP_IN);
		struct udc_ep_config *out = udc_get_ep_cfg(dev, USB_CONTROL_EP_OUT);

		wch_write(dev, WCH_TX_CTL(0), WCH_TOGGLE | WCH_NAK);
		wch_write(dev, WCH_RX_CTL(0), WCH_TOGGLE | WCH_NAK);
		in->stat.halted = false;
		out->stat.halted = false;
		/* Called in UDC workqueue/thread context, never from the ISR. */
		udc_setup_received(dev, sys_read16(config->base + WCH_RX_LEN) == 8 ? priv->packet[0]
										   : NULL);
		return;
	}
	if (idx > 2 || cfg == NULL || !udc_ep_is_busy(cfg) || cfg->stat.halted ||
	    (pid != WCH_TOKEN_IN && pid != WCH_TOKEN_OUT)) {
		return;
	}
	buf = udc_buf_peek(cfg);
	if (buf == NULL || udc_get_buf_info(buf)->setup) {
		return;
	}
	/* Ignore duplicate OUT DATA toggles without consuming an application byte. */
	if (pid == WCH_TOKEN_OUT && !(token & WCH_TOG_OK)) {
		return;
	}
	wch_response(dev, ep, WCH_NAK);
	unsigned int ctl = pid == WCH_TOKEN_IN ? WCH_TX_CTL(idx) : WCH_RX_CTL(idx);

	wch_write(dev, ctl, wch_read(dev, ctl) ^ WCH_TOGGLE);
	if (pid == WCH_TOKEN_IN) {
		net_buf_pull(buf, priv->in_length[idx]);
		if (buf->len == 0 && udc_ep_buf_has_zlp(buf)) {
			udc_ep_buf_clear_zlp(buf);
			/* An already empty request is itself the requested ZLP. */
			if (priv->in_length[idx] == cfg->mps) {
				udc_ep_set_busy(cfg, false);
				wch_kick(dev, cfg);
				return;
			}
		}
		if (buf->len == 0) {
			wch_complete(dev, cfg, 0);
		} else {
			udc_ep_set_busy(cfg, false);
		}
	} else {
		size_t len = sys_read16(config->base + WCH_RX_LEN);

		if (len > cfg->mps || len > net_buf_tailroom(buf)) {
			wch_complete(dev, cfg, -EMSGSIZE);
		} else {
			net_buf_add_mem(buf, priv->packet[idx], len);
			if (len < cfg->mps || net_buf_tailroom(buf) == 0) {
				wch_complete(dev, cfg, 0);
			} else {
				udc_ep_set_busy(cfg, false);
			}
		}
	}
}

static void wch_reset(const struct device *dev)
{
	const struct wch_udc_config *config = dev->config;
	struct udc_data *data = dev->data;

	wch_write(dev, WCH_ADDR, 0);
	wch_write(dev, WCH_MODE1, 0);
	wch_write(dev, WCH_MODE2, 0);
	for (unsigned int i = 0; i < 4; i++) {
		struct udc_ep_config *cfg = &config->eps[i];
		unsigned int idx = USB_EP_GET_IDX(cfg->addr);

		wch_response(dev, cfg->addr, WCH_NAK);
		udc_ep_set_busy(cfg, false);
		cfg->stat.halted = false;
		cfg->stat.enabled = idx == 0;
		udc_ep_cancel_queued(dev, cfg);
	}
	data->setup_pending = false;
	data->setup_valid = false;
	if (udc_is_suspended(dev)) {
		udc_set_suspended(dev, false);
	}
	wch_write(dev, WCH_TX_CTL(0), WCH_NAK);
	wch_write(dev, WCH_RX_CTL(0), WCH_ACK);
	udc_submit_event(dev, UDC_EVT_RESET, 0);
}

static int wch_disable(const struct device *dev);

static void wch_work(struct k_work *work)
{
	struct wch_udc_data *priv = CONTAINER_OF(work, struct wch_udc_data, work);
	const struct device *dev = priv->dev;
	const struct wch_udc_config *config = dev->config;

	udc_lock_internal(dev, K_FOREVER);
	if (priv->running) {
		uint8_t flags = wch_read(dev, WCH_INT_FG);

		/* INT_BUSY holds transactions while TRANSFER is pending. Do not
		 * clear it until staging data has been consumed. RESET wins over
		 * a coalesced old completion; never deliver it to a new request.
		 */
		if (flags & WCH_RESET) {
			wch_reset(dev);
			wch_ack(dev, flags);
		} else if (flags & WCH_FIFO_OV) {
			/* Overflow also beats completion: no corrupted success event. */
			wch_disable(dev);
			udc_submit_event(dev, UDC_EVT_ERROR, -EOVERFLOW);
		} else {
			if (flags & WCH_TRANSFER) {
				wch_transfer(dev, wch_read(dev, WCH_INT_ST));
				wch_ack(dev, WCH_TRANSFER);
			}
			if (flags & WCH_SUSPEND) {
				bool suspended = wch_read(dev, WCH_MIS_ST) & BIT(2);

				udc_set_suspended(dev, suspended);
				udc_submit_event(dev, suspended ? UDC_EVT_SUSPEND : UDC_EVT_RESUME,
						 0);
				wch_ack(dev, WCH_SUSPEND);
			}
		}
		if (priv->running) {
			for (unsigned int i = 0; i < 4; i++) {
				wch_kick(dev, &config->eps[i]);
			}
			wch_write(dev, WCH_INT_EN, WCH_IRQS);
		}
	}
	udc_unlock_internal(dev);
}

static void wch_isr(const struct device *dev)
{
	struct wch_udc_data *priv = udc_get_private(dev);

	/* No unbounded ISR queue: leave hardware flags latched, mask IRQ and
	 * use the controller's INT_BUSY NAK until the worker consumes them.
	 */
	wch_write(dev, WCH_INT_EN, 0);
	(void)k_work_submit_to_queue(udc_get_work_q(), &priv->work);
}

static int wch_enqueue(const struct device *dev, struct udc_ep_config *cfg, struct net_buf *buf)
{
	struct wch_udc_data *priv = udc_get_private(dev);

	if (!priv->running) {
		return -EIO;
	}
	udc_buf_put(cfg, buf);
	wch_kick(dev, cfg);
	return 0;
}

static int wch_dequeue(const struct device *dev, struct udc_ep_config *cfg)
{
	int ret = wch_quiesce(dev, cfg, true);

	if (ret != 0) {
		return ret;
	}
	udc_ep_cancel_queued(dev, cfg);
	return 0;
}

static int wch_ep_enable(const struct device *dev, struct udc_ep_config *cfg)
{
	uint8_t ep = cfg->addr;

	if (cfg->mps != 64 && USB_EP_GET_IDX(ep) == 0) {
		return -ENOTSUP;
	}
	if (ep == 0x81) {
		wch_write(dev, WCH_MODE1, BIT(6)); /* EP1 TX only: no shared RX offset */
	} else if (ep == 0x02) {
		wch_write(dev, WCH_MODE2, BIT(3)); /* EP2 RX only */
	}
	unsigned int ctl = USB_EP_DIR_IS_IN(ep) ? WCH_TX_CTL(USB_EP_GET_IDX(ep))
						: WCH_RX_CTL(USB_EP_GET_IDX(ep));

	wch_write(dev, ctl, ep == 0 ? WCH_ACK : WCH_NAK);
	cfg->stat.halted = false;
	udc_ep_set_busy(cfg, false);
	return 0;
}

static int wch_ep_disable(const struct device *dev, struct udc_ep_config *cfg)
{
	int ret = wch_dequeue(dev, cfg);

	if (ret != 0) {
		return ret;
	}
	if (cfg->addr == 0x81) {
		wch_write(dev, WCH_MODE1, 0);
	} else if (cfg->addr == 0x02) {
		wch_write(dev, WCH_MODE2, 0);
	}
	return 0;
}

static int wch_halt(const struct device *dev, struct udc_ep_config *cfg)
{
	struct udc_data *data = dev->data;
	int ret = wch_quiesce(dev, cfg, false);

	if (ret != 0) {
		return ret;
	}
	if (USB_EP_GET_IDX(cfg->addr) == 0 && (wch_setup_pending(dev) || data->setup_pending)) {
		/* A newer SETUP superseded the stack's old protocol-stall request. */
		return 0;
	}
	wch_response(dev, cfg->addr, WCH_STALL);
	cfg->stat.halted = true;
	return 0;
}

static int wch_clear_halt(const struct device *dev, struct udc_ep_config *cfg)
{
	unsigned int offset = USB_EP_DIR_IS_IN(cfg->addr) ? WCH_TX_CTL(USB_EP_GET_IDX(cfg->addr))
							  : WCH_RX_CTL(USB_EP_GET_IDX(cfg->addr));

	wch_write(dev, offset, WCH_NAK); /* Reset to DATA0 even without a queued buffer. */
	cfg->stat.halted = false;
	wch_kick(dev, cfg);
	return 0;
}

static int wch_address(const struct device *dev, uint8_t addr)
{
	/* caps.addr_before_status=false: stack calls only after status ACK. */
	if (addr > 127) {
		return -EINVAL;
	}
	wch_write(dev, WCH_ADDR, addr);
	return 0;
}

static int wch_wakeup(const struct device *dev)
{
	ARG_UNUSED(dev);
	return -ENOTSUP;
}

static enum udc_bus_speed wch_speed(const struct device *dev)
{
	ARG_UNUSED(dev);
	return UDC_BUS_SPEED_FS;
}

static int wch_init(const struct device *dev)
{
	const struct wch_udc_config *config = dev->config;
	struct wch_udc_data *priv = udc_get_private(dev);
	int ret = config->prepare(dev);

	if (ret != 0) {
		return ret;
	}
	wch_write(dev, WCH_CTRL, BIT(2) | BIT(1)); /* Reset SIE, clear FIFO. */
	k_busy_wait(10);
	wch_write(dev, WCH_CTRL, 0);
	wch_write(dev, WCH_INT_EN, 0);
	wch_write(dev, WCH_ADDR, 0);
	wch_write(dev, WCH_MODE1, 0);
	wch_write(dev, WCH_MODE2, 0);
	wch_ack(dev, 0xff);
	for (unsigned int i = 0; i < 3; i++) {
		sys_write32((uint32_t)(uintptr_t)priv->packet[i], config->base + WCH_DMA(i));
	}
	ret = udc_ep_enable_internal(dev, 0, USB_EP_TYPE_CONTROL, 64, 0);
	if (ret == 0) {
		ret = udc_ep_enable_internal(dev, 0x80, USB_EP_TYPE_CONTROL, 64, 0);
	}
	if (ret != 0) {
		(void)udc_ep_disable_internal(dev, 0);
		config->unprepare(dev);
		return ret;
	}
	config->irq_config(dev);
	return 0;
}

static int wch_enable(const struct device *dev)
{
	struct wch_udc_data *priv = udc_get_private(dev);

	priv->running = true;
	wch_write(dev, WCH_ADDR, 0);
	wch_write(dev, WCH_RX_CTL(0), WCH_ACK);
	wch_write(dev, WCH_TX_CTL(0), WCH_NAK);
	wch_ack(dev, 0xff);
	wch_write(dev, WCH_INT_EN, WCH_IRQS);
	wch_write(dev, WCH_PORT, BIT(7) | BIT(0));
	/* Pull-up + INT_BUSY + endpoint DMA. No DMA1 allocation. */
	wch_write(dev, WCH_CTRL, BIT(5) | BIT(3) | BIT(0));
	return 0;
}

static int wch_disable(const struct device *dev)
{
	const struct wch_udc_config *config = dev->config;
	struct wch_udc_data *priv = udc_get_private(dev);
	struct udc_data *data = dev->data;

	priv->running = false;
	wch_write(dev, WCH_INT_EN, 0);
	wch_write(dev, WCH_CTRL, 0);
	wch_write(dev, WCH_PORT, 0);
	wch_ack(dev, 0xff);
	for (unsigned int i = 0; i < 4; i++) {
		wch_dequeue(dev, &config->eps[i]);
	}
	data->setup_pending = false;
	data->setup_valid = false;
	/* A queued worker sees running=false under the same UDC mutex. */
	return 0;
}

static int wch_shutdown(const struct device *dev)
{
	const struct wch_udc_config *config = dev->config;

	wch_disable(dev);
	(void)udc_ep_disable_internal(dev, 0);
	(void)udc_ep_disable_internal(dev, 0x80);
	config->unprepare(dev);
	return 0;
}

static int wch_preinit(const struct device *dev)
{
	const struct wch_udc_config *config = dev->config;
	struct wch_udc_data *priv = udc_get_private(dev);
	struct udc_data *data = dev->data;
	const uint8_t addresses[] = {0, 0x80, 0x81, 0x02};

	k_mutex_init(&data->mutex);
	priv->dev = dev;
	k_work_init(&priv->work, wch_work);
	data->caps.mps0 = UDC_MPS0_64;
	for (unsigned int i = 0; i < ARRAY_SIZE(addresses); i++) {
		struct udc_ep_config *cfg = &config->eps[i];

		cfg->addr = addresses[i];
		cfg->caps.mps = 64;
		cfg->caps.in = USB_EP_DIR_IS_IN(cfg->addr);
		cfg->caps.out = !cfg->caps.in;
		cfg->caps.control = i < 2;
		cfg->caps.bulk = i >= 2;
		int ret = udc_register_ep(dev, cfg);

		if (ret != 0) {
			return ret;
		}
	}
	return 0;
}

static void wch_lock(const struct device *dev)
{
	udc_lock_internal(dev, K_FOREVER);
}
static void wch_unlock(const struct device *dev)
{
	udc_unlock_internal(dev);
}

static const struct udc_api wch_api = {
	.lock = wch_lock,
	.unlock = wch_unlock,
	.device_speed = wch_speed,
	.init = wch_init,
	.enable = wch_enable,
	.disable = wch_disable,
	.shutdown = wch_shutdown,
	.set_address = wch_address,
	.host_wakeup = wch_wakeup,
	.ep_enable = wch_ep_enable,
	.ep_disable = wch_ep_disable,
	.ep_set_halt = wch_halt,
	.ep_clear_halt = wch_clear_halt,
	.ep_enqueue = wch_enqueue,
	.ep_dequeue = wch_dequeue,
};

#ifndef CONFIG_WCH_UDC_NATIVE_TEST
#include "udc_wch_ch32v203.h"
#endif
