#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Isolate RX/TX directions on the 921600 UART direction fixture. No flashing."""
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

CASES = {"rx1": {1: "rx"}, "rx2": {2: "rx"}, "rx-both": {1: "rx", 2: "rx"},
         "tx1": {1: "tx"}, "tx2": {2: "tx"}, "tx-both": {1: "tx", 2: "tx"},
         "rx1-tx2": {1: "rx", 2: "tx"}, "tx1-rx2": {1: "tx", 2: "rx"},
         "batch1": {1: "batch"}, "batch2": {2: "batch"},
         "batch-both": {1: "batch", 2: "batch"},
         "duplex1": {1: "duplex"}, "duplex2": {2: "duplex"},
         "duplex-both": {1: "duplex", 2: "duplex"},
         "echo1": {1: "echo"}, "echo2": {2: "echo"}, "echo-both": {1: "echo", 2: "echo"}}


def payload(number):
    return bytes((i * 73 + (i >> 8) * 19 + number * 41) & 255 for i in range(1024))


def read_exact(port, count):
    result = bytearray()
    deadline = time.monotonic() + 1
    while len(result) < count and time.monotonic() < deadline:
        result.extend(port.read(count - len(result)))
    return bytes(result)


def command(args, *parts):
    subprocess.run([args.wlink, "--chip", "CH32V20X", "--speed", "low", *parts],
                   check=True, timeout=20, capture_output=True)


def transfer(port, number, direction, barrier, args):
    barrier.wait(timeout=5)
    if port.write({"rx": b"R", "tx": b"T", "batch": b"B", "duplex": b"D", "echo": b"E"}[direction]) != 1:
        raise RuntimeError("short command write")
    record = {"uart": number, "direction": direction}
    expected = payload(number)
    if direction == "echo":
        total = bytearray()
        record["host_sent"] = 0
        for start in range(0, 1024, args.echo_chunk):
            part = expected[start:start + args.echo_chunk]
            record["host_sent"] += port.write(part)
            got = read_exact(port, len(part))
            total.extend(got)
            if got != part:
                break
            if args.gap_ms:
                time.sleep(args.gap_ms / 1000)
        record["host_received"] = len(total)
        record["wire_match"] = bytes(total) == expected
        if not record["wire_match"]:
            record["received_hex"] = total.hex()
            if args.probe_after_failure and record["host_sent"] < 1024:
                # Preserve failure; a new byte tests for stalled, rather than lost, return data.
                next_byte = expected[record["host_sent"]:record["host_sent"] + 1]
                record["probe_sent"] = port.write(next_byte)
                reply = read_exact(port, max(1, record["host_sent"] - len(total) + 1))
                record["probe_reply_hex"] = reply.hex()
                record["probe_recovers_prefix"] = (
                    record["probe_sent"] == 1 and
                    bytes(total) + reply == expected[:record["host_sent"] + 1])
        record["unexpected_return_hex"] = port.read(1).hex()
        return record
    if direction in ("rx", "batch", "duplex"):
        record["host_sent"] = port.write(expected)
    if direction == "rx":
        # No on-wire echo: inspect the actual MCU receive buffer after traffic.
        time.sleep(.05)
        record["unexpected_return_hex"] = port.read(1).hex()
    else:
        got = read_exact(port, len(expected))
        record["host_received"] = len(got)
        record["wire_match"] = got == expected
        record["unexpected_return_hex"] = port.read(1).hex()
        if got != expected:
            record["received_hex"] = got.hex()
            record["first_difference"] = next((i for i, (a, b) in enumerate(zip(expected, got))
                                               if a != b), min(len(expected), len(got)))
    return record


def trial(args, index):
    actions = CASES[args.case]
    ports = {}
    record = {"case": args.case, "trial": index, "baud": 921600, "result": "BOOT_FAIL",
              "echo_chunk": args.echo_chunk, "gap_ms": args.gap_ms}
    try:
        for number in actions:
            port = serial.Serial(port=None, baudrate=921600, timeout=.02,
                                 write_timeout=2, exclusive=True)
            port.dtr = False
            port.rts = False
            port.port = getattr(args, f"uart{number}")
            port.open()
            ports[number] = port
        time.sleep(.1)
        for port in ports.values():
            port.reset_input_buffer()
        command(args, "reset", "quit")
        for number, port in ports.items():
            expected = (f"WCH DIR HIL v1 UART{number} n=1024 "
                        "cfgr0=003c000a brr=0000009c\r\n").encode()
            got = read_exact(port, len(expected))
            if got != expected:
                record["bad_banner_hex"] = got.hex()
                return record
        barrier = threading.Barrier(len(ports))
        with concurrent.futures.ThreadPoolExecutor(max_workers=len(ports)) as pool:
            futures = [pool.submit(transfer, port, n, actions[n], barrier, args)
                       for n, port in ports.items()]
            record["traffic"] = [f.result() for f in futures]
    finally:
        for port in ports.values():
            port.close()
    with tempfile.TemporaryDirectory(prefix="wch-direction-") as directory:
        path = Path(directory) / "diag.bin"
        command(args, "dump", hex(args.diag_address), "2136", "--out", str(path))
        data = path.read_bytes()
    header = struct.unpack_from("<6I", data)
    if header != (0x57444952, 2, 0x003c000a, 156, 156, 1024):
        raise RuntimeError("wrong direction fixture or diagnostic address")
    for t in record["traffic"]:
        offset = 24 + (t["uart"] - 1) * 1056
        keys = ("callbacks", "mode", "rx", "tx", "errors", "done", "unexpected", "reserved")
        d = dict(zip(keys, struct.unpack_from("<8I", data, offset)))
        t["diag"] = d
        passed = True
        if t["direction"] in ("rx", "batch", "duplex", "echo"):
            got = data[offset + 32:offset + 1056]
            match = got == payload(t["uart"])
            t["mcu_buffer_match"] = match
            # An early host timeout leaves the rest of the RX array unwritten.
            # Distinguish an incomplete frame from corruption of received bytes.
            t["mcu_received_prefix_match"] = (
                d["rx"] <= 1024 and got[:d["rx"]] == payload(t["uart"])[:d["rx"]])
            if not match:
                t["mcu_buffer_hex"] = got.hex()
            passed = match and d["rx"] == 1024 and t["host_sent"] == 1024
        if t["direction"] != "rx":
            passed = passed and t["wire_match"] and d["tx"] == 1024
        if t["direction"] == "rx":
            passed = passed and d["tx"] == 0
        if t["direction"] == "tx":
            passed = passed and d["rx"] == 0
        passed = passed and d["mode"] == {"rx": 1, "tx": 2, "batch": 3, "duplex": 4, "echo": 5}[t["direction"]]
        t["result"] = ("PASS" if passed and d["done"] and not d["errors"] and
                       not d["unexpected"] and not t["unexpected_return_hex"] else "FAIL")
    record["result"] = "PASS" if all(t["result"] == "PASS" for t in record["traffic"]) else "FAIL"
    return record


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--case", choices=CASES, required=True)
    parser.add_argument("--uart1")
    parser.add_argument("--uart2")
    parser.add_argument("--trials", type=int, default=5)
    parser.add_argument("--echo-chunk", type=int, choices=[32, 64, 128, 256, 512, 1024], default=1024)
    parser.add_argument("--gap-ms", type=int, choices=range(21), default=0)
    parser.add_argument("--probe-after-failure", action="store_true")
    parser.add_argument("--wlink", required=True)
    parser.add_argument("--diag-address", type=lambda x: int(x, 0), required=True)
    args = parser.parse_args()
    if not 1 <= args.trials <= 20:
        parser.error("trials must be 1..20")
    if not 0x20000000 <= args.diag_address <= 0x20005000 - 2136 or args.diag_address % 4:
        parser.error("diagnostic must fit aligned within SRAM")
    if any(not getattr(args, f"uart{n}") for n in CASES[args.case]):
        parser.error("provide selected port paths")
    if len(CASES[args.case]) == 2 and Path(args.uart1).resolve() == Path(args.uart2).resolve():
        parser.error("distinct serial devices required")
    results = []
    for i in range(args.trials):
        r = trial(args, i)
        results.append(r["result"])
        print(json.dumps(r), flush=True)
    print(json.dumps({"summary": {k: results.count(k) for k in ("PASS", "FAIL", "BOOT_FAIL")}}), flush=True)
    command(args, "reset", "quit")
    return 0 if all(r == "PASS" for r in results) else 1


if __name__ == "__main__":
    raise SystemExit(main())
