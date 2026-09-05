/* SPDX-License-Identifier: Apache-2.0 */

#include <zephyr/usb/usbd.h>
#include <zephyr/drivers/usb/udc.h>
#include <zephyr/sys/byteorder.h>

/* Test-only, unallocated IDs. Override both for any deployed product. */
USBD_DEVICE_DEFINE(test_usb, DEVICE_DT_GET(DT_NODELABEL(zephyr_udc0)), CONFIG_WCH_TEST_USB_VID,
		   CONFIG_WCH_TEST_USB_PID);
USBD_DESC_LANG_DEFINE(lang);
USBD_DESC_MANUFACTURER_DEFINE(manufacturer, "Ragtime development");
USBD_DESC_PRODUCT_DEFINE(product, "WCH USBFS hardware test");
USBD_DESC_CONFIG_DEFINE(config_desc, "Bulk loopback");
USBD_CONFIGURATION_DEFINE(configuration, 0, 50, &config_desc);

static struct usb_if_descriptor iface = {
	.bLength = sizeof(iface),
	.bDescriptorType = USB_DESC_INTERFACE,
	.bNumEndpoints = 2,
	.bInterfaceClass = USB_BCC_VENDOR,
};
static struct usb_ep_descriptor ep_in = {
	.bLength = sizeof(ep_in),
	.bDescriptorType = USB_DESC_ENDPOINT,
	.bEndpointAddress = 0x81,
	.bmAttributes = USB_EP_TYPE_BULK,
	.wMaxPacketSize = sys_cpu_to_le16(64),
};
static struct usb_ep_descriptor ep_out = {
	.bLength = sizeof(ep_out),
	.bDescriptorType = USB_DESC_ENDPOINT,
	.bEndpointAddress = 0x02,
	.bmAttributes = USB_EP_TYPE_BULK,
	.wMaxPacketSize = sys_cpu_to_le16(64),
};
static struct usb_desc_header end;
static const struct usb_desc_header *descriptors[] = {
	(void *)&iface,
	(void *)&ep_in,
	(void *)&ep_out,
	&end,
};
static uint8_t control_data[256];
static size_t control_length;
static bool enabled;
/* Diagnostic only; host assertions determine PASS, never this marker alone. */
volatile int wch_usb_hil_status;

static int receive(struct usbd_class_data *c_data)
{
	struct net_buf *buf = usbd_ep_buf_alloc(c_data, ep_out.bEndpointAddress, 256);

	if (buf == NULL) {
		return -ENOMEM;
	}
	int ret = usbd_ep_enqueue(c_data, buf);

	if (ret != 0) {
		net_buf_unref(buf);
	}
	return ret;
}

static int request(struct usbd_class_data *c_data, struct net_buf *buf, int err)
{
	uint8_t ep = udc_get_buf_info(buf)->ep;
	int ret = 0;

	if (err == 0 && enabled && ep == ep_out.bEndpointAddress) {
		struct net_buf *reply = usbd_ep_buf_alloc(c_data, ep_in.bEndpointAddress, buf->len);

		if (reply == NULL) {
			ret = -ENOMEM;
		} else {
			net_buf_add_mem(reply, buf->data, buf->len);
			/* Exact-MPS replies explicitly terminate a larger host read. */
			udc_get_buf_info(reply)->zlp = buf->len != 0 && (buf->len % 64) == 0;
			ret = usbd_ep_enqueue(c_data, reply);
			if (ret != 0) {
				net_buf_unref(reply);
			}
		}
	}
	net_buf_unref(buf);
	/* One outstanding frame: rearm OUT after its echoed IN completes. */
	if (err == 0 && enabled && ep == ep_in.bEndpointAddress) {
		ret = receive(c_data);
	}
	if (ret != 0) {
		wch_usb_hil_status = ret;
	}
	return ret;
}

static int control_out(struct usbd_class_data *c_data, const struct usb_setup_packet *setup,
		       const struct net_buf *buf)
{
	ARG_UNUSED(c_data);
	if (setup->bmRequestType != 0x40 || setup->bRequest != 0x5b ||
	    setup->wLength > sizeof(control_data)) {
		return -ENOTSUP;
	}
	if (buf != NULL) {
		if (buf->len > sizeof(control_data)) {
			return -EMSGSIZE;
		}
		memcpy(control_data, buf->data, buf->len);
		control_length = buf->len;
	} else if (setup->wLength == 0) {
		control_length = 0;
	}
	return 0;
}

#ifdef WCH_TEST_CTRL_ALLOC
static struct net_buf *control_in(struct usbd_class_data *c_data,
				  const struct usb_setup_packet *setup)
{
	if (setup->bmRequestType != 0xc0 || setup->bRequest != 0x5c) {
		return NULL;
	}
	size_t len = MIN(control_length, setup->wLength);
	struct net_buf *buf = usbd_ep_ctrl_data_in_alloc(usbd_class_get_ctx(c_data), len);

	if (buf != NULL) {
		net_buf_add_mem(buf, control_data, len);
	}
	return buf;
}
#else
/* Official 4.4.0 supplies the control IN buffer; current Ragtime allocates it. */
static int control_in(struct usbd_class_data *c_data, const struct usb_setup_packet *setup,
		      struct net_buf *buf)
{
	ARG_UNUSED(c_data);
	if (setup->bmRequestType != 0xc0 || setup->bRequest != 0x5c) {
		return -ENOTSUP;
	}
	net_buf_add_mem(buf, control_data,
			MIN(MIN(control_length, setup->wLength), net_buf_tailroom(buf)));
	return 0;
}
#endif

static void *get_desc(struct usbd_class_data *c_data, enum usbd_speed speed)
{
	ARG_UNUSED(c_data);
	return speed == USBD_SPEED_FS ? descriptors : NULL;
}
static void enable(struct usbd_class_data *c_data)
{
	enabled = true;
	wch_usb_hil_status = receive(c_data);
}
static void disable(struct usbd_class_data *c_data)
{
	ARG_UNUSED(c_data);
	enabled = false;
}
static const struct usbd_cctx_vendor_req requests = USBD_VENDOR_REQ(0x5b, 0x5c);
static const struct usbd_class_api api = {
	.get_desc = get_desc,
	.enable = enable,
	.disable = disable,
	.request = request,
	.control_to_dev = control_out,
	.control_to_host = control_in,
};
USBD_DEFINE_CLASS(wch_bulk_test, &api, NULL, &requests);

int main(void)
{
	/* Allows reset/recovery and removal of the debugger before releasing SDI. */
	wch_usb_hil_status = 1;
	k_sleep(K_SECONDS(5));
	int ret = usbd_add_descriptor(&test_usb, &lang);

	if (ret == 0) {
		ret = usbd_add_descriptor(&test_usb, &manufacturer);
	}
	if (ret == 0) {
		ret = usbd_add_descriptor(&test_usb, &product);
	}
	if (ret == 0) {
		ret = usbd_add_configuration(&test_usb, USBD_SPEED_FS, &configuration);
	}
	if (ret == 0) {
		ret = usbd_register_class(&test_usb, "wch_bulk_test", USBD_SPEED_FS, 1);
	}
	if (ret == 0) {
		ret = usbd_init(&test_usb);
	}
	if (ret == 0) {
		ret = usbd_enable(&test_usb);
	}
	wch_usb_hil_status = ret == 0 ? 2 : ret;
	return ret;
}
