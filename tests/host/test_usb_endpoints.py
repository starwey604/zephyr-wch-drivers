# SPDX-License-Identifier: Apache-2.0
import errno
import unittest
from usb_endpoints import exercise, payload, positive


class USBError(Exception):
    errno = errno.EPIPE


class FakeDevice:
    def __init__(self):
        self.control = b""
        self.echo = b""
        self.halted = set()
        self.cleared = []
        self.writes = []
        self.corrupt = False

    def ctrl_transfer(self, kind, request, value, index, data, timeout):
        if request == 0x7F:
            raise USBError()
        if request == 0x5B:
            self.control = data
            return len(data)
        if request == 0x5C:
            return self.control
        if request == 3:
            self.halted.add(index)
        if request == 1:
            raise AssertionError("use clear_halt to synchronize host data toggle")
        if request == 0:
            return bytes((int(index in self.halted), 0))
        return 0

    def clear_halt(self, ep):
        self.cleared.append(ep)
        self.halted.discard(ep)

    def write(self, ep, data, timeout):
        self.writes.append(data)
        self.echo += data
        return len(data)

    def read(self, ep, length, timeout):
        data, self.echo = self.echo, b""
        return data + b"bad" if self.corrupt else data


class HostProtocol(unittest.TestCase):
    def test_boundaries_and_zlp(self):
        dev = FakeDevice()
        self.assertEqual(exercise(dev, USBError, cycles=1)["cases"], 22)
        self.assertEqual(dev.writes.count(b""), 3)  # empty frame + 64/128 terminators
        self.assertEqual(len(dev.writes[-1]), 7)
        self.assertEqual(dev.cleared, [0x81, 0x02])

    def test_mismatch_fails(self):
        dev = FakeDevice()
        dev.corrupt = True
        with self.assertRaises(RuntimeError):
            exercise(dev, USBError, cycles=1)

    def test_payload_changes(self):
        self.assertEqual(len(payload(256, 0)), 256)
        self.assertNotEqual(payload(256, 0), payload(256, 1))

    def test_bounded_arguments(self):
        self.assertEqual(positive("2"), 2)
        with self.assertRaises(Exception):
            positive("0")


if __name__ == "__main__":
    unittest.main()
