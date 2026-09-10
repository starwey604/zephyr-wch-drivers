#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Decode a stopped/post-traffic wch_uart_irq_trace RAM dump, version 1."""
import argparse
import json
from pathlib import Path
import struct

FIELDS = """regs baud irq_count rx_bytes tx_bytes api_error_or fifo_error_or
api_error_reads fifo_error_reads max_isr_ticks slow_isrs max_rx_service_ticks
rx_service_over_char entry_tick last_status events_written frozen first_error_site
first_error_status first_error_tick first_error_rx first_error_tx char_ticks timing_samples
sample_countdown""".split()
EVENT_FIELDS = "tick kind status irq rx tx duration detail".split()


def layout(data):
    if len(data) < 32:
        raise ValueError("short trace header")
    magic, version, count, words, capacity, period, hz, threshold = struct.unpack_from("<8I", data)
    if magic != 0x57495452 or version != 1 or not 1 <= count <= 8:
        raise ValueError("wrong trace ABI/magic/port count")
    if capacity != 32 or words != len(FIELDS) + capacity * len(EVENT_FIELDS) or not hz or not period:
        raise ValueError("invalid trace layout")
    return {"version": version, "port_count": count, "port_words": words,
            "event_count": capacity, "period_ticks": period, "nominal_hz": hz,
            "slow_ticks": threshold, "size": 32 + count * words * 4}


def decode(data):
    header = layout(data)
    if len(data) != header["size"]:
        raise ValueError("trace dump length mismatch")
    ports = []
    for i in range(header["port_count"]):
        offset = 32 + i * header["port_words"] * 4
        values = struct.unpack_from(f"<{header['port_words']}I", data, offset)
        port = dict(zip(FIELDS, values[:len(FIELDS)]))
        for field in ("max_isr_ticks", "max_rx_service_ticks"):
            port[field.replace("_ticks", "_us")] = round(port[field] * 1e6 / header["nominal_hz"], 3)
        port["events"] = []
        written = port["events_written"]
        for sequence in range(max(0, written - header["event_count"]), written):
            start = len(FIELDS) + sequence % header["event_count"] * len(EVENT_FIELDS)
            port["events"].append(dict(zip(EVENT_FIELDS, values[start:start + len(EVENT_FIELDS)])))
        ports.append(port)
    return {"header": header, "ports": ports}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("dump", type=Path)
    args = parser.parse_args()
    print(json.dumps(decode(args.dump.read_bytes()), indent=2))
