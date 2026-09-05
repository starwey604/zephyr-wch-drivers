#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Expect specific diagnostics, not just any failed build. Does not flash hardware."""

import argparse
from pathlib import Path
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--workspace", type=Path, required=True)
    parser.add_argument("--sdk", type=Path)
    args = parser.parse_args()
    workspace = args.workspace.resolve()
    module = Path(__file__).resolve().parent.parent
    source = module / "tests/drivers/uart_dma"
    cases = [
        ("duplicate-driver", ["-DCONFIG_UART_WCH_USART=y"], "Disable CONFIG_UART_WCH_USART"),
        ("duplicate-dma-driver", ["-DCONFIG_DMA_WCH=y"], "Disable CONFIG_DMA_WCH"),
        ("async-with-irq", ["-DCONFIG_UART_ASYNC_API=y", "-DCONFIG_WCH_UART_ASYNC_TX=y",
                            "-DCONFIG_UART_INTERRUPT_DRIVEN=y"], "WCH async TX requires"),
        ("async-wide-data", ["-DCONFIG_UART_ASYNC_API=y", "-DCONFIG_WCH_UART_ASYNC_TX=y",
                             "-DCONFIG_UART_INTERRUPT_DRIVEN=n", "-DCONFIG_UART_WIDE_DATA=y"],
         "WCH async TX requires"),
        ("init-order", ["-DCONFIG_DMA_INIT_PRIORITY=60"], "DMA must initialize before UART"),
        ("bad-channel", [], "DMA channel index is out of range"),
        ("wrong-request", [], "Incorrect CH32V203 UART DMA request mapping"),
        ("missing-rx", [], "WCH UART DMA requires tx and rx dma-names"),
        ("disabled-dma", [], "DMA controller is disabled"),
    ]
    for name, extra, diagnostic in cases:
        command = [
            str(workspace / ".venv/bin/west"), "build", "-s",
            str(workspace / "zephyr/samples/hello_world"),
            "-b", "ragtime_gello", "-d", str(workspace / "build/test-wch-negative" / name),
            "--pristine", "always", "--",
            f"-DEXTRA_ZEPHYR_MODULES={module}",
            f"-DEXTRA_CONF_FILE={module / 'tests/configs/ragtime-cpp20.conf'};"
            f"{module / 'tests/configs/uart-dma-prepare.conf'}",
        ]
        if args.sdk:
            command.append(f"-DZEPHYR_SDK_INSTALL_DIR={args.sdk.resolve()}")
        if not extra:
            command.append(f"-DEXTRA_DTC_OVERLAY_FILE={source / 'negative' / (name + '.overlay')}")
        command.extend(extra)
        result = subprocess.run(command, cwd=workspace, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, text=True, timeout=180)
        if result.returncode == 0 or diagnostic not in result.stdout:
            print(f"FAIL {name}: expected failed build containing {diagnostic!r}")
            print(result.stdout)
            return 1
        print(f"PASS {name}: expected diagnostic observed", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
