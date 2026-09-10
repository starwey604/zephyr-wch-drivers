#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Repeated 921600 IRQ echo diagnosis; no flashing, power or protection changes."""
import argparse
import concurrent.futures
import json
from pathlib import Path
import random
import struct
import subprocess
import tempfile
import threading
import time

import serial


def read_exact(port, count, seconds=1):
    data = bytearray()
    deadline = time.monotonic() + seconds
    while len(data) < count and time.monotonic() < deadline:
        data.extend(port.read(count - len(data)))
    return bytes(data)


def traffic(port, number, size, blocks, trial, barrier):
    rng = random.Random(0x57434800 + number + trial * 256)
    result = {"uart": number, "sent": 0, "received": 0, "matched_blocks": 0}
    barrier.wait(timeout=5)
    started = time.monotonic()
    for sequence in range(blocks):
        payload = bytes([number]) + sequence.to_bytes(4, "little") + rng.randbytes(size - 5)
        written = port.write(payload)
        result["sent"] += written
        if written != size:
            raise RuntimeError("short host write")
        received = read_exact(port, size)
        result["received"] += len(received)
        if received != payload:
            result.update(result="FAIL", failed_block=sequence,
                          expected_hex=payload.hex(), received_hex=received.hex(),
                          first_difference=next((i for i, (a, b) in
                                                 enumerate(zip(payload, received)) if a != b),
                                                min(size, len(received))))
            break
        result["matched_blocks"] += 1
    else:
        trailing = port.read(1)
        result["result"] = "FAIL" if trailing else "PASS"
        if trailing:
            result["trailing_hex"] = trailing.hex()
    result["seconds"] = round(time.monotonic() - started, 3)
    return result


def wlink(args, *command):
    subprocess.run([args.reset_tool, "--chip", "CH32V20X", "--speed", "low", *command],
                   check=True, timeout=20, capture_output=True)


def snapshot(args):
    # Read only after every serial worker has finished; attach can affect clocks.
    with tempfile.TemporaryDirectory(prefix="wch-irq-diag-") as directory:
        path = Path(directory) / "diag.bin"
        wlink(args, "dump", hex(args.diag_address), "68", "--out", str(path))
        words = struct.unpack("<17I", path.read_bytes())
    if words[0] != 0x57434849 or words[1] != 2:
        raise RuntimeError("wrong fixture/diagnostic address or stage")
    keys = ("callbacks", "rx", "tx", "errors", "overflow", "failed")
    return {"cfgr0": hex(words[2]), "brr": list(words[3:5]),
            "ports": [dict(zip(keys, words[5 + i * 6:11 + i * 6])) for i in range(2)]}


def run_trial(args, trial):
    numbers = [1, 2] if args.mode == "dual" else [int(args.mode)]
    ports = []
    record = {"trial": trial, "mode": args.mode, "baud": 921600,
              "size": args.size, "blocks": args.blocks}
    try:
        for number in numbers:
            port = serial.Serial(port=None, baudrate=921600, timeout=.02,
                                 write_timeout=2, exclusive=True)
            port.dtr = False
            port.rts = False
            port.port = getattr(args, f"uart{number}")
            port.open()
            ports.append(port)
        # Drain old traffic before reset, never discard bytes after reset.
        time.sleep(.1)
        for port in ports:
            port.reset_input_buffer()
        wlink(args, "reset", "quit")
        record["banners"] = []
        for number, port in zip(numbers, ports):
            expected = (f"WCH IRQ HIL v1 UART{number} cfgr0=003c000a "
                        "brr=0000009c USB/DMA off\r\n").encode()
            banner = read_exact(port, len(expected))
            record["banners"].append(banner.decode("ascii", errors="backslashreplace"))
            if banner != expected:
                record["result"] = "BOOT_FAIL"
                return record
        barrier = threading.Barrier(len(ports))
        with concurrent.futures.ThreadPoolExecutor(max_workers=len(ports)) as pool:
            futures = [pool.submit(traffic, port, number, args.size, args.blocks, trial, barrier)
                       for number, port in zip(numbers, ports)]
            record["traffic"] = [future.result() for future in futures]
        record["result"] = ("PASS" if all(r["result"] == "PASS" for r in record["traffic"])
                            else "FAIL")
    finally:
        for port in ports:
            port.close()
        record["diag"] = snapshot(args)
    for result in record["traffic"]:
        diag = record["diag"]["ports"][result["uart"] - 1]
        result["device_counts_match"] = (diag["rx"] == result["sent"] and
                                          diag["tx"] == result["sent"] + 62)
        if not result["device_counts_match"] or any(diag[k] for k in
                                                     ("errors", "overflow", "failed")):
            record["result"] = "FAIL"
    return record


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--uart1")
    parser.add_argument("--uart2")
    parser.add_argument("--mode", choices=["1", "2", "dual"], required=True)
    parser.add_argument("--size", type=int, choices=[32, 64, 128, 256, 512], default=512)
    parser.add_argument("--blocks", type=int, default=128)
    parser.add_argument("--trials", type=int, default=5)
    parser.add_argument("--reset-tool", required=True)
    parser.add_argument("--diag-address", type=lambda x: int(x, 0), required=True,
                        help="wch_irq_diag address from the matching ELF; SRAM only")
    args = parser.parse_args()
    if not 1 <= args.trials <= 20 or not 1 <= args.blocks <= 2048:
        parser.error("trials 1..20 and blocks 1..2048 required")
    if not 0x20000000 <= args.diag_address <= 0x20005000 - 68 or args.diag_address % 4:
        parser.error("diagnostic must be aligned within CH32V203F8U SRAM")
    selected = [1, 2] if args.mode == "dual" else [int(args.mode)]
    if any(not getattr(args, f"uart{i}") for i in selected):
        parser.error("provide explicit device paths for selected ports")
    if args.mode == "dual" and Path(args.uart1).resolve() == Path(args.uart2).resolve():
        parser.error("distinct serial devices required")
    records = []
    for trial in range(args.trials):
        record = run_trial(args, trial)
        records.append(record)
        print(json.dumps(record), flush=True)
    print(json.dumps({"summary": {key: sum(r["result"] == key for r in records)
                                 for key in ("PASS", "FAIL", "BOOT_FAIL")}}), flush=True)
    # Diagnostic attachment may have changed clocks; leave a clean boot.
    wlink(args, "reset", "quit")
    return 0 if all(r["result"] == "PASS" for r in records) else 1


if __name__ == "__main__":
    raise SystemExit(main())
