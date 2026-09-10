#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Feed both UART DMA RX ports from existing bridges; inspect RAM after traffic."""
import argparse
import concurrent.futures
import json
from pathlib import Path
import struct
import subprocess
import tempfile
import threading
import time

import serial


def payload(port, round_number, length):
    return bytes((i * 73 + (i >> 8) * 19 + port * 41 + round_number * 7) & 255
                 for i in range(length))


def expect(port, text):
    got = bytearray()
    deadline = time.monotonic() + 2
    while len(got) < len(text) and time.monotonic() < deadline:
        got.extend(port.read(len(text) - len(got)))
    if bytes(got) != text:
        raise RuntimeError(f"handshake mismatch: expected={text.hex()} received={got.hex()}")


def send_pair(ports, round_number, start, end, corrupt=False):
    barrier = threading.Barrier(2)

    def worker(number, port):
        data = payload(number, round_number, end)[start:end]
        if corrupt and number == 1 and round_number == 1 and start == 0:
            data = bytes([data[0] ^ 1]) + data[1:]
        barrier.wait(timeout=2)
        if port.write(data) != len(data):
            raise RuntimeError("short host write")

    with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
        futures = [pool.submit(worker, n, p) for n, p in enumerate(ports, 1)]
        for future in futures:
            future.result()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--uart1", required=True)
    parser.add_argument("--uart2", required=True)
    parser.add_argument("--baud", type=int, choices=(115200, 921600), default=115200)
    parser.add_argument("--trials", type=int, default=5)
    parser.add_argument("--diag-address", type=lambda x: int(x, 0), required=True)
    parser.add_argument("--wlink", required=True)
    parser.add_argument("--corrupt-byte", action="store_true",
                        help="negative check: corrupt UART1 round 1 byte 0; expect FAIL/exit 1")
    args = parser.parse_args()
    if not 1 <= args.trials <= 20:
        parser.error("trials must be 1..20")
    if not 0x20000000 <= args.diag_address <= 0x20005000 - 352 or args.diag_address % 4:
        parser.error("352-byte diagnostic must fit aligned in SRAM")
    if Path(args.uart1).resolve() == Path(args.uart2).resolve():
        parser.error("distinct serial ports required")

    def command(*parts):
        subprocess.run([args.wlink, "--chip", "CH32V20X", "--speed", "low", *parts],
                       timeout=20, check=True, capture_output=True)

    outcomes = []
    try:
        for trial in range(args.trials):
            ports = []
            record = {"trial": trial, "baud": args.baud, "sent_rounds": 0,
                      "corrupt_byte": args.corrupt_byte}
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
                for round_number, length in enumerate((1, 32, 7, 39, 1024)):
                    for n, port in enumerate(ports, 1):
                        expect(port, f"WCH RX READY UART{n} round={round_number}\r\n".encode())
                    send_pair(ports, round_number, 0, 32 if round_number == 3 else length,
                              args.corrupt_byte)
                    if round_number == 3:
                        for n, port in enumerate(ports, 1):
                            expect(port, f"WCH RX NEXT UART{n} round=3\r\n".encode())
                        send_pair(ports, round_number, 32, 39)
                    record["sent_rounds"] += 1
                time.sleep(.3)
                record["unexpected_hex"] = [p.read(1).hex() for p in ports]
            except (RuntimeError, serial.SerialException) as error:
                record["host_error"] = str(error)
                # Let the fixture's bounded waits expire before debug access.
                time.sleep(3)
            finally:
                for port in ports:
                    port.close()
            with tempfile.TemporaryDirectory(prefix="wch-dma-rx-") as directory:
                path = Path(directory) / "diag.bin"
                command("dump", hex(args.diag_address), "352", "--out", str(path))
                diag = struct.unpack("<88I", path.read_bytes())
            brr = 1250 if args.baud == 115200 else 156
            good = diag[:8] == (0x57525848, 2, 0x003c000a, brr, brr, 4, 5, 0)
            for r, length in enumerate((1, 32, 7, 39, 1024)):
                events = 2 if r == 3 else 1
                for p in range(2):
                    offset = 8 + r * 16 + p * 8
                    good &= diag[offset:offset + 8] == (events, length, events, events, 1, 0, 0, 1)
            good = good and "host_error" not in record and not any(record["unexpected_hex"])
            outcomes.append(bool(good))
            record.update(result="PASS" if good else "FAIL", diag=diag)
            print(json.dumps(record), flush=True)
        print(json.dumps({"summary": {"PASS": sum(outcomes), "FAIL": len(outcomes) - sum(outcomes)}}), flush=True)
    finally:
        command("reset", "quit")
    return 0 if all(outcomes) else 1


if __name__ == "__main__":
    raise SystemExit(main())
