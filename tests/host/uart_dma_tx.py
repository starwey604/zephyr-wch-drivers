#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Bounded dual-port capture for uart_dma TX-only hardware fixture; no flashing."""
import argparse
import json
from pathlib import Path
import struct
import subprocess
import tempfile
import time

import serial


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--uart1", required=True)
    parser.add_argument("--uart2", required=True)
    parser.add_argument("--baud", type=int, choices=(115200, 921600), default=115200)
    parser.add_argument("--trials", type=int, default=5)
    parser.add_argument("--diag-address", type=lambda x: int(x, 0), required=True)
    parser.add_argument("--wlink", required=True)
    args = parser.parse_args()
    if not 1 <= args.trials <= 20:
        parser.error("trials must be 1..20")
    if not 0x20000000 <= args.diag_address <= 0x20005000 - 64 or args.diag_address % 4:
        parser.error("64-byte diagnostic must fit aligned in SRAM")
    if Path(args.uart1).resolve() == Path(args.uart2).resolve():
        parser.error("distinct serial ports required")

    def command(*parts):
        subprocess.run([args.wlink, "--chip", "CH32V20X", "--speed", "low", *parts],
                       timeout=20, check=True, capture_output=True)

    outcomes = []
    try:
        for trial in range(args.trials):
            ports, traffic = [], []
            try:
                for name in (args.uart1, args.uart2):
                    port = serial.Serial(port=None, baudrate=args.baud, timeout=.02,
                                         write_timeout=2, exclusive=True)
                    port.dtr = port.rts = False
                    port.port = name
                    port.open()
                    ports.append(port)
                time.sleep(.1)
                for port in ports:
                    port.reset_input_buffer()
                command("reset", "quit")
                # Both ports start on reset. OS input buffering retains the
                # second port while reading the first; no host sends or echoes.
                for number, port in enumerate(ports, 1):
                    pattern = bytes(i ^ (0x5a if number == 1 else 0xa5) for i in range(128))
                    expected = pattern[:1] + pattern
                    got = bytearray()
                    deadline = time.monotonic() + 2
                    while len(got) < len(expected) and time.monotonic() < deadline:
                        got.extend(port.read(len(expected) - len(got)))
                    traffic.append({"uart": number, "bytes": len(got),
                                    "wire_match": got == expected, "received_hex": got.hex()})
                time.sleep(.1)
                for t, port in zip(traffic, ports):
                    t["unexpected_hex"] = port.read(1).hex()
            finally:
                for port in ports:
                    port.close()
            # Target is now idle; never inspect debug state during traffic.
            with tempfile.TemporaryDirectory(prefix="wch-dma-tx-") as directory:
                path = Path(directory) / "diag.bin"
                command("dump", hex(args.diag_address), "64", "--out", str(path))
                diag = struct.unpack("<16I", path.read_bytes())
            expected_diag = (0x57545848, 2) + (2, 0, 129, 128, 0, 2, 0) * 2
            good = diag == expected_diag and all(t["wire_match"] and not t["unexpected_hex"] for t in traffic)
            outcomes.append(good)
            print(json.dumps({"trial": trial, "baud": args.baud,
                              "result": "PASS" if good else "FAIL", "diag": diag,
                              "traffic": traffic}), flush=True)
        print(json.dumps({"summary": {"PASS": sum(outcomes), "FAIL": len(outcomes) - sum(outcomes)}}), flush=True)
    finally:
        command("reset", "quit")
    return 0 if all(outcomes) else 1


if __name__ == "__main__":
    raise SystemExit(main())
