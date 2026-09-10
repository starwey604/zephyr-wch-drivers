# Host-fed dual UART DMA RX fixture

Use the existing CH340E/USART1 and WCH-Link/USART2 wiring. **No local-loopback
jumpers**: adapter TX drives MCU RX, adapter RX observes MCU control messages,
with common ground and 3.3-V UART levels. USBFS remains disabled and SDI stays
available. Never join two TX outputs.

This fixture tests actual DMA payload reception, not a modeled register block.
Short polling `READY`/`NEXT` messages coordinate the host; no payload echo or
DMA TX is used. Five rounds run on both ports after each reset:

| Round | Host bytes per port | Receiver behavior |
| ---: | ---: | --- |
| 0 | 1 | Full 1-byte buffer, automatic release/disable |
| 1 | 32 | Full 32-byte buffer, automatic release/disable |
| 2 | 7 | 32-byte buffer, explicit disable after 500-ms host-send window |
| 3 | 32, then 7 | Queued second 32-byte buffer; `NEXT` handshake before tail; explicit disable |
| 4 | 1024 | Full 1024-byte buffer, automatic release/disable |

Each round checks data on the MCU, RX_RDY lengths/offsets/buffer identities,
request/release counts, duplicate releases, RX_DISABLED and error events.
The host snapshots all round results after traffic. This is **paced handoff**,
not lossless streaming. The 500-ms window is test scheduling, not driver IDLE
or finite inactivity-timeout support; RX uses `SYS_FOREVER_US` throughout.
Host scheduling delays can fail a trial and must not be called DMA faults
without examining the saved results. On a failure the MCU stops both receivers.

## Build

From Ragtime, isolated from unrelated product modules:

```sh
.venv/bin/west build -s ../zephyr-wch-drivers/tests/drivers/uart_rx_host \
  -b ragtime_gello -d build/test-wch-rx-host --pristine always -- \
  -DBOARD_ROOT=$PWD/firmware \
  "-DZEPHYR_MODULES=$PWD/modules/hal/wch;$PWD/../zephyr-wch-drivers" \
  -DZEPHYR_SDK_INSTALL_DIR=/path/to/zephyr-sdk
```

Default is 115200. For 921600 use a separate build directory and append
`-DEXTRA_DTC_OVERLAY_FILE=$PWD/../zephyr-wch-drivers/tests/drivers/uart_direction/boards/ragtime_gello.overlay`.
Keep `build_only: true`: compiling does not run the hardware test.

Flash only after the [hardware checks](../../../docs/hardware-bringup.md),
verify readback, and resolve `wch_rx_host_diag` from the matching ELF. Run:

```sh
.venv/bin/python ../zephyr-wch-drivers/tests/host/uart_dma_rx.py \
  --uart1 /dev/serial/by-id/<CH340> --uart2 /dev/serial/by-id/<WCH-Link-CDC> \
  --baud 115200 --trials 5 --diag-address <ELF-address> \
  --wlink "$PWD/build/wch-tools/bin/wlink"
```

The runner opens explicit exclusive ports, resets, waits for each handshake,
starts both senders together, captures RAM only after traffic and resets again.
It never flashes or restores firmware. A standalone reset without the host
will deliberately time out; rerun with the host, not by interpreting that
later timeout as the outcome of an earlier saved trial.

`--corrupt-byte --trials 1` deliberately flips UART1's first byte in round 1.
Expected result is **FAIL / exit 1**, MCU stage `0xff`, error `-EIO`, round 1
UART1 `match=0`. This is a negative fixture check, not a successful transfer.

## Diagnostic ABI

`wch_rx_host_diag` is 352 bytes, 88 little-endian 32-bit words: magic
`0x57525848`, stage (1 running, 2 passed, 255 failed), CFGR0, two BRRs, current
round, passed-round count, signed error; then five pairs of port records.
Each port has ready count, bytes, release count, request count, disabled count,
stop reason, bad-event count and data-match flag. Expected CFGR0 is `003c000a`,
BRR is `04e2` at 115200 or `009c` at 921600. Result snapshots are valid only
with their matching ELF and protocol. See the [hardware report](../../../docs/hardware-20260910-dma.md).
