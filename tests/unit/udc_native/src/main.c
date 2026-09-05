/* SPDX-License-Identifier: Apache-2.0 */
#include <zephyr/ztest.h>
#include "udc_wch.c"

static uint32_t registers[64];
static struct udc_ep_config endpoints[4];
static struct wch_udc_data priv;
static struct udc_data data = {.priv = &priv};
static int prepare_result;
static int unprepare_count;
static int prepare(const struct device *dev)
{
	ARG_UNUSED(dev);
	return prepare_result;
}
static void unprepare(const struct device *dev)
{
	ARG_UNUSED(dev);
	unprepare_count++;
}
static void irq_config(const struct device *dev)
{
	ARG_UNUSED(dev);
}
static const struct wch_udc_config config = {
	.base = (uintptr_t)registers,
	.eps = endpoints,
	.prepare = prepare,
	.unprepare = unprepare,
	.irq_config = irq_config,
};
DEVICE_DEFINE(test_udc, "test_udc", NULL, NULL, &data, &config, POST_KERNEL, 50, &wch_api);
static const struct device *dev = DEVICE_GET(test_udc);
static struct udc_event events[32];
static size_t event_count;

static int callback(const struct device *d, const struct udc_event *event)
{
	zassert_equal_ptr(d, dev);
	zassert_true(event_count < ARRAY_SIZE(events));
	events[event_count++] = *event;
	return 0;
}

static void before(void *fixture)
{
	ARG_UNUSED(fixture);
	memset(registers, 0, sizeof(registers));
	memset(&priv, 0, sizeof(priv));
	memset(&data, 0, sizeof(data));
	memset(endpoints, 0, sizeof(endpoints));
	data.priv = &priv;
	event_count = 0;
	unprepare_count = 0;
	prepare_result = 0;
	zassert_ok(wch_preinit(dev));
	zassert_ok(udc_init(dev, callback, &data));
	zassert_ok(udc_enable(dev));
	wch_write(dev, WCH_MIS_ST, BIT(5)); /* Explicitly model SIE idle. */
	zassert_ok(udc_ep_enable(dev, 0x81, USB_EP_TYPE_BULK, 64, 0));
	zassert_ok(udc_ep_enable(dev, 0x02, USB_EP_TYPE_BULK, 64, 0));
}

static void cleanup(void *fixture)
{
	ARG_UNUSED(fixture);
	if (udc_is_enabled(dev)) {
		zassert_ok(udc_disable(dev));
	}
	if (udc_is_initialized(dev)) {
		zassert_ok(udc_shutdown(dev));
	}
	struct k_work_sync sync;

	k_work_cancel_sync(&priv.work, &sync);
	for (size_t i = 0; i < event_count; i++) {
		if (events[i].type == UDC_EVT_EP_REQUEST) {
			net_buf_unref(events[i].buf);
		}
	}
}

static struct net_buf *enqueue(uint8_t ep, size_t len)
{
	struct net_buf *buf = udc_ep_buf_alloc(dev, ep, len);

	zassert_not_null(buf);
	if (USB_EP_DIR_IS_IN(ep)) {
		for (size_t i = 0; i < len; i++) {
			net_buf_add_u8(buf, i ^ 0x5a);
		}
	}
	zassert_ok(udc_ep_enqueue(dev, buf));
	return buf;
}

static void transfer(uint8_t token, size_t len)
{
	wch_write(dev, WCH_INT_ST, token);
	sys_write16(len, config.base + WCH_RX_LEN);
	wch_write(dev, WCH_INT_FG, WCH_TRANSFER);
	wch_work(&priv.work);
}

static void expect_request(size_t index, struct net_buf *buf, int err)
{
	zassert_true(index < event_count);
	zassert_equal(events[index].type, UDC_EVT_EP_REQUEST);
	zassert_equal_ptr(events[index].buf, buf);
	zassert_equal(udc_get_buf_info(buf)->err, err);
}

ZTEST(wch_udc, test_caps_and_internal_dma)
{
	zassert_false(data.caps.hs);
	zassert_false(data.caps.rwup);
	zassert_false(data.caps.addr_before_status);
	zassert_is_null(udc_get_ep_cfg(dev, 0x01));
	zassert_is_null(udc_get_ep_cfg(dev, 0x82));
	zassert_false(endpoints[2].caps.iso);
	zassert_false(endpoints[2].caps.interrupt);
	for (unsigned int i = 0; i < 3; i++) {
		zassert_equal((uintptr_t)priv.packet[i] % 4, 0);
		zassert_equal(sys_read32(config.base + WCH_DMA(i)),
			      (uint32_t)(uintptr_t)priv.packet[i]);
	}
	zassert_equal(wch_read(dev, WCH_CTRL), BIT(5) | BIT(3) | BIT(0));
	zassert_equal(udc_host_wakeup(dev), -ENOTSUP);
}

ZTEST(wch_udc, test_in_packet_progress_queue_and_zlp)
{
	struct net_buf *first = enqueue(0x81, 128);
	udc_get_buf_info(first)->zlp = true;
	struct net_buf *next = enqueue(0x81, 1);

	zassert_equal(first->len, 128); /* DMA preparation must not consume bytes. */
	zassert_mem_equal(priv.packet[1], first->data, 64);
	transfer(WCH_TOKEN_IN | 1, 0);
	zassert_equal(first->len, 64);
	zassert_equal(event_count, 0);
	transfer(WCH_TOKEN_IN | 1, 0);
	zassert_equal(sys_read16(config.base + WCH_TX_LEN(1)), 0);
	zassert_equal(event_count, 0);
	transfer(WCH_TOKEN_IN | 1, 0);
	expect_request(0, first, 0);
	zassert_equal(sys_read16(config.base + WCH_TX_LEN(1)), 1);
	transfer(WCH_TOKEN_IN | 1, 0);
	expect_request(1, next, 0);
}

ZTEST(wch_udc, test_empty_in_single_zlp)
{
	struct net_buf *buf = enqueue(0x81, 0);
	udc_get_buf_info(buf)->zlp = true;
	transfer(WCH_TOKEN_IN | 1, 0);
	expect_request(0, buf, 0);
	zassert_false(udc_ep_is_busy(&endpoints[2]));
}

ZTEST(wch_udc, test_out_short_packet_and_duplicate_toggle)
{
	struct net_buf *buf = enqueue(0x02, 128);

	memset(priv.packet[2], 0xa5, 64);
	transfer(WCH_TOKEN_OUT | 2, 64); /* Retransmission with wrong DATA toggle. */
	zassert_equal(buf->len, 0);
	transfer(WCH_TOKEN_OUT | WCH_TOG_OK | 2, 64);
	zassert_equal(buf->len, 64);
	zassert_equal(event_count, 0);
	memset(priv.packet[2], 0x5a, 7);
	transfer(WCH_TOKEN_OUT | WCH_TOG_OK | 2, 7);
	expect_request(0, buf, 0);
	zassert_equal(buf->len, 71);
	zassert_equal(buf->data[0], 0xa5);
	zassert_equal(buf->data[70], 0x5a);
}

ZTEST(wch_udc, test_out_overflow_and_zero_length)
{
	struct net_buf *small = enqueue(0x02, 1);

	memset(priv.packet[2], 0xaa, 64);
	transfer(WCH_TOKEN_OUT | WCH_TOG_OK | 2, 64);
	expect_request(0, small, -EMSGSIZE);
	zassert_equal(small->len, 0);
	struct net_buf *zero = enqueue(0x02, 0);

	transfer(WCH_TOKEN_OUT | WCH_TOG_OK | 2, 0);
	expect_request(1, zero, 0);
}

ZTEST(wch_udc, test_setup_preempts_data_and_protocol_stall)
{
	struct net_buf *setup = udc_ctrl_setup_alloc(dev);

	zassert_not_null(setup);
	zassert_ok(udc_ep_enqueue(dev, setup));
	struct net_buf *old = enqueue(0x80, 64);

	zassert_ok(udc_ep_set_halt(dev, 0x80));
	const uint8_t request[8] = {0x80, 6, 0, 1, 0, 0, 18, 0};

	memcpy(priv.packet[0], request, 8);
	transfer(WCH_TOKEN_SETUP, 8);
	expect_request(0, old, -ECONNRESET);
	expect_request(1, setup, 0);
	zassert_mem_equal(setup->data, request, 8);
	zassert_false(endpoints[1].stat.halted);
	zassert_equal(wch_read(dev, WCH_TX_CTL(0)), WCH_TOGGLE | WCH_NAK);
	zassert_equal(wch_read(dev, WCH_ADDR), 0);
	struct net_buf *status = enqueue(0x80, 0);

	transfer(WCH_TOKEN_IN, 0);
	expect_request(2, status, 0);
	zassert_ok(udc_set_address(dev, 7));
	zassert_equal(wch_read(dev, WCH_ADDR), 7);
}

ZTEST(wch_udc, test_setup_cache_and_malformed_setup)
{
	memset(priv.packet[0], 0x55, 8);
	transfer(WCH_TOKEN_SETUP, 8);
	zassert_true(data.setup_pending);
	struct net_buf *setup = udc_ctrl_setup_alloc(dev);

	zassert_ok(udc_ep_enqueue(dev, setup));
	expect_request(0, setup, 0);
	zassert_equal(setup->len, 8);
	transfer(WCH_TOKEN_SETUP, 7);
	zassert_true(data.setup_pending);
	zassert_false(data.setup_valid);
}

ZTEST(wch_udc, test_control_out_data_and_in_status)
{
	memset(priv.packet[0], 0, 8);
	transfer(WCH_TOKEN_SETUP, 8);
	struct net_buf *setup = udc_ctrl_setup_alloc(dev);

	zassert_ok(udc_ep_enqueue(dev, setup));
	struct net_buf *out = enqueue(0, 65);

	memset(priv.packet[0], 0x11, 64);
	transfer(WCH_TOKEN_OUT | WCH_TOG_OK, 64);
	memset(priv.packet[0], 0x22, 1);
	transfer(WCH_TOKEN_OUT | WCH_TOG_OK, 1);
	expect_request(1, out, 0);
	zassert_equal(out->len, 65);
	struct net_buf *status = enqueue(0x80, 0);

	transfer(WCH_TOKEN_IN, 0);
	expect_request(2, status, 0);
}

ZTEST(wch_udc, test_dequeue_clears_latched_completion)
{
	struct net_buf *old = enqueue(0x81, 64);

	wch_write(dev, WCH_INT_ST, WCH_TOKEN_IN | 1);
	wch_write(dev, WCH_INT_FG, WCH_TRANSFER);
	zassert_ok(udc_ep_dequeue(dev, 0x81));
	expect_request(0, old, -ECONNABORTED);
	struct net_buf *next = enqueue(0x81, 7);

	wch_work(&priv.work);
	zassert_equal(next->len, 7);
	zassert_equal(event_count, 1);
	transfer(WCH_TOKEN_IN | 1, 0);
	expect_request(1, next, 0);
}

ZTEST(wch_udc, test_busy_sie_preserves_loan)
{
	struct net_buf *buf = enqueue(0x81, 64);

	wch_write(dev, WCH_MIS_ST, 0);
	zassert_equal(udc_ep_dequeue(dev, 0x81), -EBUSY);
	zassert_equal(event_count, 0);
	zassert_equal_ptr(udc_buf_peek(&endpoints[2]), buf);
	wch_write(dev, WCH_MIS_ST, BIT(5));
	zassert_ok(udc_ep_dequeue(dev, 0x81));
	expect_request(0, buf, -ECONNABORTED);
}

ZTEST(wch_udc, test_halt_clear_resets_toggle_and_restarts)
{
	zassert_ok(udc_ep_set_halt(dev, 0x81));
	struct net_buf *buf = enqueue(0x81, 7);

	zassert_false(udc_ep_is_busy(&endpoints[2]));
	zassert_equal(wch_read(dev, WCH_TX_CTL(1)) & 3, WCH_STALL);
	zassert_ok(udc_ep_clear_halt(dev, 0x81));
	zassert_equal(wch_read(dev, WCH_TX_CTL(1)), WCH_ACK);
	transfer(WCH_TOKEN_IN | 1, 0);
	expect_request(0, buf, 0);
	zassert_ok(udc_ep_set_halt(dev, 0x02));
	zassert_ok(udc_ep_clear_halt(dev, 0x02));
	zassert_equal(wch_read(dev, WCH_RX_CTL(2)), WCH_NAK);
}

ZTEST(wch_udc, test_reset_beats_transfer_and_clears_address)
{
	struct net_buf *buf = enqueue(0x81, 64);

	zassert_ok(udc_set_address(dev, 42));
	wch_write(dev, WCH_INT_ST, WCH_TOKEN_IN | 1);
	wch_write(dev, WCH_INT_FG, WCH_RESET | WCH_TRANSFER);
	wch_work(&priv.work);
	expect_request(0, buf, -ECONNABORTED);
	zassert_equal(events[1].type, UDC_EVT_RESET);
	zassert_equal(buf->len, 64);
	zassert_equal(wch_read(dev, WCH_ADDR), 0);
	zassert_false(endpoints[2].stat.enabled);
	zassert_false(data.setup_pending);
}

ZTEST(wch_udc, test_suspend_resume)
{
	wch_write(dev, WCH_MIS_ST, BIT(5) | BIT(2));
	wch_write(dev, WCH_INT_FG, WCH_SUSPEND);
	wch_work(&priv.work);
	zassert_true(udc_is_suspended(dev));
	zassert_equal(events[0].type, UDC_EVT_SUSPEND);
	wch_write(dev, WCH_MIS_ST, BIT(5));
	wch_write(dev, WCH_INT_FG, WCH_SUSPEND);
	wch_work(&priv.work);
	zassert_false(udc_is_suspended(dev));
	zassert_equal(events[1].type, UDC_EVT_RESUME);
}

ZTEST(wch_udc, test_fifo_overflow_detaches)
{
	struct net_buf *buf = enqueue(0x02, 64);

	wch_write(dev, WCH_INT_EN, 0);
	wch_write(dev, WCH_INT_ST, WCH_TOKEN_OUT | WCH_TOG_OK | 2);
	wch_write(dev, WCH_INT_FG, WCH_FIFO_OV | WCH_TRANSFER);
	wch_work(&priv.work);
	expect_request(0, buf, -ECONNABORTED);
	zassert_equal(events[1].type, UDC_EVT_ERROR);
	zassert_false(priv.running);
	zassert_equal(wch_read(dev, WCH_CTRL), 0);
	zassert_equal(wch_read(dev, WCH_INT_EN), 0);
	struct net_buf *next = udc_ep_buf_alloc(dev, 0x02, 64);

	zassert_not_null(next);
	zassert_equal(udc_ep_enqueue(dev, next), -EIO);
	net_buf_unref(next);
}

ZTEST(wch_udc, test_disable_worker_and_reinitialize)
{
	struct net_buf *buf = enqueue(0x81, 7);

	zassert_ok(udc_disable(dev));
	expect_request(0, buf, -ECONNABORTED);
	wch_work(&priv.work);
	zassert_equal(event_count, 1);
	zassert_equal(wch_read(dev, WCH_CTRL), 0);
	zassert_ok(udc_shutdown(dev));
	zassert_equal(unprepare_count, 1);
	prepare_result = -EIO;
	zassert_equal(udc_init(dev, callback, &data), -EIO);
	zassert_false(udc_is_initialized(dev));
	prepare_result = 0;
	zassert_ok(udc_init(dev, callback, &data));
	zassert_ok(udc_enable(dev));
}

ZTEST(wch_udc, test_real_workqueue_dispatch)
{
	struct net_buf *buf = enqueue(0x81, 1);
	struct k_work_sync sync;

	wch_write(dev, WCH_INT_ST, WCH_TOKEN_IN | 1);
	wch_write(dev, WCH_INT_FG, WCH_TRANSFER);
	wch_isr(dev);
	k_work_flush(&priv.work, &sync);
	expect_request(0, buf, 0);
	zassert_equal(wch_read(dev, WCH_INT_EN), WCH_IRQS);
}

ZTEST(wch_udc, test_halt_preserves_acked_progress)
{
	struct net_buf *buf = enqueue(0x81, 128);

	wch_write(dev, WCH_INT_ST, WCH_TOKEN_IN | 1);
	wch_write(dev, WCH_INT_FG, WCH_TRANSFER);
	zassert_ok(udc_ep_set_halt(dev, 0x81));
	zassert_equal(buf->len, 64);
	zassert_ok(udc_ep_clear_halt(dev, 0x81));
	zassert_mem_equal(priv.packet[1], buf->data, 64);
	transfer(WCH_TOKEN_IN | 1, 0);
	expect_request(0, buf, 0);
}

ZTEST(wch_udc, test_old_stall_does_not_stall_cached_setup)
{
	transfer(WCH_TOKEN_SETUP, 8);
	zassert_true(data.setup_pending);
	zassert_ok(udc_ep_set_halt(dev, 0x80));
	zassert_false(endpoints[1].stat.halted);
	zassert_equal(wch_read(dev, WCH_TX_CTL(0)) & 3, WCH_NAK);
}

ZTEST_SUITE(wch_udc, NULL, NULL, before, cleanup, NULL);
