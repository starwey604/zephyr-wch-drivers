/* SPDX-License-Identifier: Apache-2.0 */

#include <zephyr/ztest.h>

ZTEST(wch_usb_endpoints, test_control_enumeration_pending)
{
	TC_PRINT("SKIP: WCH USBFS UDC and EP0 enumeration are not implemented yet\n");
	ztest_test_skip();
}

ZTEST(wch_usb_endpoints, test_bulk_in_out_pending)
{
	TC_PRINT("SKIP: bulk IN/OUT requires a real UDC, USB class and host fixture\n");
	ztest_test_skip();
}

ZTEST_SUITE(wch_usb_endpoints, NULL, NULL, NULL, NULL, NULL);
