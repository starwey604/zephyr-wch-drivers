#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Read-only, bounded Linux usbmon capture of one WCH-Link CDC bulk pair."""
import argparse
import ctypes
import fcntl
import json
import os
from pathlib import Path
import platform
import select
import struct
import sys
import time


class GetArg(ctypes.Structure):
    _fields_ = [("header", ctypes.c_void_p), ("data", ctypes.c_void_p),
                ("alloc", ctypes.c_size_t)]


def decode(header, payload, bus, address):
    if len(header) != 64:
        raise ValueError("usbmon GETX requires a 64-byte header")
    fields = struct.unpack_from("<QBBBBHBBqiiII", header)
    urb, event, kind, ep, dev, usb_bus, setup_flag, data_flag, sec, usec, status, length, captured = fields
    # Discard every other device/endpoint before storing or printing anything.
    if usb_bus != bus or dev != address or kind != 3 or ep not in (0x03, 0x83):
        return None
    return {"urb": hex(urb), "event": chr(event), "endpoint": hex(ep),
            "timestamp_us": sec * 1000000 + usec, "status": status,
            "length": length, "captured": captured,
            "payload_hex": payload[:min(captured, len(payload))].hex() if data_flag == 0 else "",
            "data_present": data_flag == 0, "truncated": captured > len(payload)}


def check_target(bus, address):
    for device in Path("/sys/bus/usb/devices").iterdir():
        try:
            if int((device / "busnum").read_text()) != bus or int((device / "devnum").read_text()) != address:
                continue
            if ((device / "idVendor").read_text().strip(),
                    (device / "idProduct").read_text().strip()) != ("1a86", "8010"):
                raise ValueError("selected device is not a WCH-Link in RISC-V mode")
            return
        except FileNotFoundError:
            continue
    raise ValueError("selected WCH-Link bus/address not found")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bus", type=int, required=True)
    parser.add_argument("--address", type=int, required=True)
    parser.add_argument("--seconds", type=int, default=30)
    args = parser.parse_args()
    if not 1 <= args.bus <= 999 or not 1 <= args.address <= 127 or not 1 <= args.seconds <= 60:
        parser.error("bus 1..999, address 1..127 and seconds 1..60 required; no all-bus capture")
    if (sys.platform != "linux" or platform.machine() != "x86_64" or
            sys.byteorder != "little" or ctypes.sizeof(GetArg) != 24):
        parser.error("this diagnostic ioctl encoding supports x86-64 Linux only")
    check_target(args.bus, args.address)
    header = ctypes.create_string_buffer(64)
    data = ctypes.create_string_buffer(4096)
    request = GetArg(ctypes.addressof(header), ctypes.addressof(data), len(data))
    getx = (1 << 30) | (ctypes.sizeof(GetArg) << 16) | (0x92 << 8) | 10
    stats_ioctl = (2 << 30) | (8 << 16) | (0x92 << 8) | 3
    count = 0
    fd = os.open(f"/dev/usbmon{args.bus}", os.O_RDONLY | os.O_NONBLOCK)
    try:
        print(json.dumps({"capture": "ready", "bus": args.bus, "address": args.address,
                          "seconds": args.seconds, "endpoints": ["0x03", "0x83"]}), flush=True)
        print("usbmon ready", file=sys.stderr, flush=True)
        deadline = time.monotonic() + args.seconds
        while time.monotonic() < deadline:
            if not select.select([fd], [], [], min(.1, max(0, deadline - time.monotonic())))[0]:
                continue
            try:
                fcntl.ioctl(fd, getx, bytes(request))
            except BlockingIOError:
                continue
            event = decode(header.raw, data.raw, args.bus, args.address)
            if event is not None:
                print(json.dumps(event), flush=True)
                count += 1
        stats = bytearray(8)
        fcntl.ioctl(fd, stats_ioctl, stats, True)
        queued, dropped = struct.unpack("<II", stats)
        print(json.dumps({"capture": "finished", "events": count,
                          "queued": queued, "dropped": dropped}), flush=True)
        return 0 if dropped == 0 and queued == 0 else 1
    finally:
        os.close(fd)


if __name__ == "__main__":
    raise SystemExit(main())
