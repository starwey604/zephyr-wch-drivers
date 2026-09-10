#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Paced ASCII checks for uart_polling; never flashes or resets a target."""
import argparse
import json
import os
import time

import serial
from serial.tools import list_ports


def expect(port, expected, timeout=2):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        line = port.readline()
        if line == expected:
            return
        if line and not line.startswith(b"WCH ALIVE "):
            raise RuntimeError(f"expected {expected!r}, received {line!r}")
    raise RuntimeError(f"timeout waiting for {expected!r}")


def paced_write(port, data):
    # At the default 100 Hz Zephyr tick, k_sleep(1 ms) can span two ticks.
    # No FIFO/DMA/IRQ is enabled: do not send a burst into the one-byte RDR.
    for byte in data:
        port.write(bytes([byte]))
        port.flush()
        time.sleep(0.05)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True, help="explicit CH340 device or by-id path")
    args = parser.parse_args()
    candidates = list(list_ports.comports(include_links=True))
    if not any(os.path.realpath(p.device) == os.path.realpath(args.port)
               and (p.vid, p.pid) == (0x1A86, 0x7523)
               for p in candidates):
        raise RuntimeError("selected port is not an enumerated CH340")
    port = serial.Serial(port=None, baudrate=115200, timeout=0.1,
                         write_timeout=1, exclusive=True)
    port.dtr = False
    port.rts = False
    port.port = args.port
    with port:
        # Start only on a live fixture; discard one possibly partial opening line.
        port.readline()
        deadline = time.monotonic() + 3
        while time.monotonic() < deadline:
            if port.readline().startswith(b"WCH ALIVE uptime_ms="):
                break
        else:
            raise RuntimeError("fixture heartbeat not found; refusing test writes")
        cases = 0
        for cycle in range(2):
            for length in (0, 1, 15, 63):
                data = bytes(65 + (i + cycle) % 26 for i in range(length))
                paced_write(port, data + b"\n")
                expect(port, b"ECHO " + data + b"\r\n")
                cases += 1
        paced_write(port, b"X" * 64 + b"\n")
        expect(port, b"ERR line too long\r\n")
        cases += 1
        paced_write(port, b"RECOVER\r\n")
        expect(port, b"ECHO RECOVER\r\n")
        cases += 1
    print(json.dumps({"result": "PASS", "cases": cases, "baud": 115200,
                      "inter_byte_ms": 50, "port": args.port}))


if __name__ == "__main__":
    main()
