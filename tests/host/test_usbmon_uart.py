# SPDX-License-Identifier: Apache-2.0
"""Decoder checks only: no USB device or monitor is opened."""
import struct
import unittest

from usbmon_uart import decode


def header(event="C", ep=0x83, address=48, bus=3, kind=3, status=0,
           length=65, captured=65, data_flag=0):
    return struct.pack("<QBBBBHBBqiiII", 123, ord(event), kind, ep, address,
                       bus, 1, data_flag, 10, 42, status, length, captured) + bytes(24)


class DecodeTests(unittest.TestCase):
    def test_complete_recovery(self):
        data = bytes(range(65))
        event = decode(header(), data + bytes(100), 3, 48)
        self.assertEqual(event["payload_hex"], data.hex())
        self.assertEqual(event["timestamp_us"], 10000042)
        self.assertFalse(event["truncated"])

    def test_cancelled_partial_urb_is_preserved(self):
        event = decode(header(status=-2, length=64, captured=64), bytes(range(64)), 3, 48)
        self.assertEqual((event["event"], event["status"], event["length"]), ("C", -2, 64))
        self.assertEqual(len(bytes.fromhex(event["payload_hex"])), 64)

    def test_submission_has_no_stale_payload(self):
        event = decode(header(event="S", status=-115, length=128, captured=0,
                              data_flag=1), b"stale", 3, 48)
        self.assertEqual(event["payload_hex"], "")
        self.assertFalse(event["data_present"])

    def test_out_submission_payload(self):
        event = decode(header(event="S", ep=3, status=-115, length=1, captured=1), b"E", 3, 48)
        self.assertEqual(event["payload_hex"], "45")

    def test_privacy_filter(self):
        for kwargs in ({"address": 47}, {"bus": 4}, {"ep": 0x81}, {"ep": 0}, {"kind": 1}):
            with self.subTest(kwargs=kwargs):
                self.assertIsNone(decode(header(**kwargs), b"private", 3, 48))

    def test_truncated_capture_and_short_header(self):
        event = decode(header(), b"x", 3, 48)
        self.assertTrue(event["truncated"])
        with self.assertRaises(ValueError):
            decode(bytes(48), b"", 3, 48)


if __name__ == "__main__":
    unittest.main()
