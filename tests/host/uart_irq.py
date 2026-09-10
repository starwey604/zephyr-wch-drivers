#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Bounded dual-port binary IRQ echo checks; explicit ports, no flash/mode change."""
import argparse
import concurrent.futures
import json
import os
import random
import subprocess
import threading
import time

import serial


def read_exact(port, count, seconds=3):
    deadline = time.monotonic() + seconds
    data = bytearray()
    while len(data) < count and time.monotonic() < deadline:
        data.extend(port.read(count - len(data)))
    return bytes(data)


def exchange(port, payload):
    if port.write(payload) != len(payload):
        raise RuntimeError("short host write")
    received = read_exact(port, len(payload))
    if received != payload:
        mismatch = next((i for i, (a, b) in enumerate(zip(payload, received))
                         if a != b), min(len(payload), len(received)))
        raise RuntimeError(f"echo mismatch: sent={len(payload)} received={len(received)} "
                           f"first_difference={mismatch}")


def run_port(port, number, rounds, barrier):
    rng = random.Random(0x57434800 + number)
    barrier.wait(timeout=5)
    started = time.monotonic()
    total = 0
    for sequence in range(rounds):
        payload = number.to_bytes(1, "little") + sequence.to_bytes(4, "little")
        payload += rng.randbytes(507)
        exchange(port, payload)
        total += len(payload)
    return {"uart": number, "bytes_each_direction": total,
            "seconds": round(time.monotonic() - started, 3)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--uart1")
    parser.add_argument("--uart2")
    parser.add_argument("--only", type=int, choices=[1, 2],
                        help="Run one port only; default requires both ports")
    parser.add_argument("--baud", type=int, default=115200,
                        choices=[115200, 921600, 3000000])
    parser.add_argument("--rounds", type=int, default=128)
    parser.add_argument("--reset-tool", required=True,
                        help="Explicit wlink binary: reset quit only, after opening ports")
    args = parser.parse_args()
    if not 1 <= args.rounds <= 4096:
        parser.error("rounds must be 1..4096")
    selected = [args.only] if args.only else [1, 2]
    paths = {1: args.uart1, 2: args.uart2}
    if any(not paths[number] for number in selected):
        parser.error("provide a device path for each selected UART")
    if not args.only and os.path.realpath(args.uart1) == os.path.realpath(args.uart2):
        parser.error("two distinct serial devices are required")
    ports = []
    try:
        for number in selected:
            path = paths[number]
            port = serial.Serial(port=None, baudrate=args.baud, timeout=0.05,
                                 write_timeout=3, exclusive=True)
            port.dtr = False
            port.rts = False
            port.port = path
            port.open()
            ports.append(port)
            port.reset_input_buffer()
        # Debug attach can change clocks: reset before measuring, never during traffic.
        subprocess.run([args.reset_tool, "--chip", "CH32V20X", "--speed", "low",
                        "reset", "quit"], check=True, timeout=20, capture_output=True)
        for number, port in zip(selected, ports):
            banner = bytearray()
            deadline = time.monotonic() + 3
            while not banner.endswith(b"\n") and time.monotonic() < deadline:
                banner.extend(port.read(1))
            expected = f"WCH IRQ HIL v1 UART{number} ".encode()
            if not banner.startswith(expected) or not banner.endswith(b"USB/DMA off\r\n"):
                raise RuntimeError(f"UART{number} missing/wrong boot banner: {bytes(banner)!r}")
            print(json.dumps({"uart": number, "banner": banner.decode().strip()}), flush=True)
        # Distinct sequential cases detect crossed ports and cover all byte values.
        for number, port in zip(selected, ports):
            for payload in (bytes([number]), bytes(range(256)), bytes(reversed(range(256)))):
                exchange(port, payload)
            print(json.dumps({"uart": number, "single_port": "PASS", "bytes": 513}), flush=True)
        barrier = threading.Barrier(len(ports))
        with concurrent.futures.ThreadPoolExecutor(max_workers=len(ports)) as pool:
            futures = [pool.submit(run_port, port, i, args.rounds, barrier)
                       for i, port in zip(selected, ports)]
            results = [future.result(timeout=args.rounds * 3 + 10) for future in futures]
        for number, port in zip(selected, ports):
            if port.read(1):
                raise RuntimeError(f"UART{number}: unexpected trailing data")
        print(json.dumps({"result": "PASS", "baud_setting": args.baud,
                          "mode": "single" if args.only else "dual", "traffic": results,
                          "note": "512-byte request/echo blocks; NOT sustained line-rate qualification"}),
              flush=True)
    finally:
        for port in ports:
            port.close()


if __name__ == "__main__":
    main()
