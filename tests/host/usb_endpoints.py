#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Bounded, explicitly targeted USBFS hardware checks. Requires PyUSB/libusb."""
import argparse
import errno
import json

LENGTHS = (0, 1, 63, 64, 65, 127, 128, 255, 256)
PRODUCT = "WCH USBFS hardware test"


def payload(length, cycle):
    return bytes(((index * 37) ^ cycle ^ 0xA5) & 0xFF for index in range(length))


def require(condition, message):
    # Unlike assert, hardware checks must remain enabled under python -O.
    if not condition:
        raise RuntimeError(message)


def exercise(dev, usb_error, cycles=10, timeout=2000):
    """Device is already identified, configured and exclusively claimed."""
    cases = 0
    byte_count = 0
    for cycle in range(cycles):
        for length in LENGTHS:
            data = payload(length, cycle)
            sent = dev.ctrl_transfer(0x40, 0x5B, 0, 0, data, timeout=timeout)
            require(sent == length, f"short EP0 OUT: {sent}/{length}")
            received = bytes(dev.ctrl_transfer(0xC0, 0x5C, 0, 0, length + 1, timeout=timeout))
            require(received == data, f"EP0 mismatch: cycle={cycle} length={length}")

            sent = dev.write(0x02, data, timeout=timeout)
            require(sent == length, f"short bulk OUT: {sent}/{length}")
            # OUT request capacity is 256. Shorter exact-MPS frames need a ZLP;
            # a full 256-byte frame completes without an extra OUT request.
            if length and length < 256 and length % 64 == 0:
                require(dev.write(0x02, b"", timeout=timeout) == 0, "OUT ZLP failed")
            received = bytes(dev.read(0x81, 257, timeout=timeout))
            require(received == data, f"bulk mismatch: cycle={cycle} length={length}")
            cases += 2
            byte_count += length * 2

        # Unsupported control request must STALL, not time out or disconnect.
        try:
            dev.ctrl_transfer(0xC0, 0x7F, 0, 0, 1, timeout=timeout)
        except usb_error as error:
            require(error.errno == errno.EPIPE, f"expected STALL, got {error}")
        else:
            raise RuntimeError("unsupported control request did not STALL")
        status = bytes(dev.ctrl_transfer(0x80, 0, 0, 0, 2, timeout=timeout))
        require(len(status) == 2, "EP0 did not recover after protocol STALL")

        for ep in (0x81, 0x02):
            dev.ctrl_transfer(0x02, 3, 0, ep, b"", timeout=timeout)  # SET_FEATURE HALT
            status = bytes(dev.ctrl_transfer(0x82, 0, 0, ep, 2, timeout=timeout))
            require(status == b"\x01\x00", f"EP {ep:#x} halt not visible")
            # Use the backend operation, not a raw CLEAR_FEATURE: the host
            # controller's data toggle must be reset along with the device's.
            dev.clear_halt(ep)
            status = bytes(dev.ctrl_transfer(0x82, 0, 0, ep, 2, timeout=timeout))
            require(status == b"\x00\x00", f"EP {ep:#x} halt did not clear")
        # Verify DATA0 recovery even on the last cycle.
        probe = payload(7, cycle + 1)
        require(dev.write(0x02, probe, timeout=timeout) == len(probe), "post-halt OUT failed")
        require(bytes(dev.read(0x81, 257, timeout=timeout)) == probe, "post-halt echo mismatch")
        cases += 4
    return {"result": "PASS", "cycles": cycles, "cases": cases, "payload_bytes": byte_count}


def positive(value):
    number = int(value, 0)
    if number <= 0:
        raise argparse.ArgumentTypeError("must be positive")
    return number


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bus", type=positive, required=True)
    parser.add_argument("--address", type=positive, required=True)
    parser.add_argument("--vid", type=lambda s: int(s, 0), default=0x1209)
    parser.add_argument("--pid", type=lambda s: int(s, 0), default=0xFFFF)
    parser.add_argument("--cycles", type=positive, default=10)
    parser.add_argument("--timeout-ms", type=positive, default=2000)
    parser.add_argument("--reset-after", action="store_true",
                        help="reset this DUT after checks; rerun to validate re-enumeration")
    args = parser.parse_args()
    import usb.core
    import usb.util

    dev = usb.core.find(idVendor=args.vid, idProduct=args.pid,
                        bus=args.bus, address=args.address)
    require(dev is not None, "selected DUT not found")
    dev.default_timeout = args.timeout_ms
    require(usb.util.get_string(dev, dev.iProduct) == PRODUCT, "wrong product: refusing writes")
    # Read descriptors before the first configuration/endpoint mutation.
    require(dev.bMaxPacketSize0 == 64, "expected EP0 MPS 64")
    interface = dev[0][(0, 0)]
    require(interface.bInterfaceClass == 0xFF, "expected vendor interface")
    require({ep.bEndpointAddress for ep in interface} == {0x81, 0x02}, "wrong endpoints")
    require(all(ep.wMaxPacketSize == 64 and (ep.bmAttributes & 3) == 2
                for ep in interface), "expected FS bulk endpoints")
    claimed = False
    try:
        dev.set_configuration()
        # Never automatically detach an unrelated kernel driver.
        usb.util.claim_interface(dev, 0)
        claimed = True
        report = exercise(dev, usb.core.USBError, args.cycles, args.timeout_ms)
        report.update(bus=args.bus, address=args.address, vid=args.vid, pid=args.pid)
        print(json.dumps(report))
    finally:
        if claimed:
            usb.util.release_interface(dev, 0)
        usb.util.dispose_resources(dev)
    if args.reset_after:
        dev.reset()
        print("Reset requested, not verified. Rerun with the new bus/address after enumeration.")
        usb.util.dispose_resources(dev)


if __name__ == "__main__":
    main()
