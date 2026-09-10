# UART direction isolation fixture

CH32V203/Gello, both UARTs at 921600 8N1. USBFS, DMA and driver timing trace
are off. Wiring remains USART1/CH340E and USART2/WCH-Link, using the corrected
J1 mapping in the [IRQ fixture](../uart_irq/README.md). This is a bounded
diagnostic app, not a product protocol or a sustained-throughput benchmark.

After reset each UART sends a polling bootstrap banner, then accepts one
command byte. **All measured payload transfers use interrupt APIs.**

| Command | Action | Software mode |
| --- | --- | ---: |
| `R` | Receive 1024 bytes into RAM; do not echo | 1 |
| `T` | Transmit an independent known 1024-byte pattern | 2 |
| `B` | Receive the whole 1024-byte frame, then transmit the received bytes | 3 |
| `D` | Transmit the known pattern while receiving independently | 4 |
| `E` | Immediately echo the received prefix, up to 1024 bytes | 5 |

The command is excluded from payload counters. Each port disables its active
interrupts on a detected UART error; reset is required for the next trial.
Completion means payload received/fed into DATAR, not verified USART TC.
The host checks actual returned bytes and/or reads the exact MCU RX buffer
after traffic; software TX counts alone are never a wire-level pass.

## Build and run

From Ragtime, without importing unrelated product modules:

```sh
.venv/bin/west build -s ../zephyr-wch-drivers/tests/drivers/uart_direction \
  -b ragtime_gello -d build/test-wch-uart-direction --pristine always -- \
  -DBOARD_ROOT=$PWD/firmware \
  "-DZEPHYR_MODULES=$PWD/modules/hal/wch;$PWD/../zephyr-wch-drivers" \
  -DZEPHYR_SDK_INSTALL_DIR=/path/to/zephyr-sdk
```

Explicitly flash only after target/protection checks, then resolve
`wch_direction_diag` from the matching ELF. For example:

```sh
.venv/bin/python ../zephyr-wch-drivers/tests/host/uart_direction.py \
  --case rx-both --trials 5 \
  --uart1 /dev/serial/by-id/<CH340> --uart2 /dev/serial/by-id/<WCH-Link-CDC> \
  --diag-address <ELF-address> --wlink "$PWD/build/wch-tools/bin/wlink"
```

Cases include `rx1`, `rx2`, `rx-both`, `tx1`, `tx2`, `tx-both`, `rx1-tx2`,
`tx1-rx2`, and `batch1/2/-both`, `duplex1/2/-both`, `echo1/2/-both`.
In echo mode, `--echo-chunk` selects 32/64/128/256/512/1024 bytes;
`--gap-ms` adds a 0..20-ms pause after each successful chunk. Optional
`--probe-after-failure` sends one additional expected payload byte after a
partial echo failure, if the frame is not yet fully sent, and records any
delayed response. It **never** converts that failed test to a pass. Probe bytes
are recorded separately from the original payload count, but are included in
MCU RX/TX counters. `probe_recovers_prefix` checks that the original response
plus delayed response exactly equals all sent bytes including the probe.
`mcu_received_prefix_match` compares only the bytes counted as received; an
incomplete frame can fail full-buffer equality without received-byte corruption.

Each trial checks exact boot identity and clock/BRR, starts selected workers
together, and records full receive-buffer equality plus counts/error/completion
state. A direction-only RX trial allows 50 ms settling after host write before
reading an unexpected-return byte and capturing RAM. TX/echo waits have a
one-second deadline. This is not a measurement of the USB-to-UART queue delay.

No debug reads occur during traffic. The runner resets before each trial and
after final diagnostic capture; it never flashes, restores an image, changes
protection, power or probe mode. The last installed image stays on the board.

## RAM ABI and limitations

`wch_direction_diag` is 2136 bytes: six little-endian 32-bit header words
(magic `0x57444952`, stage 2, CFGR0, two BRRs, length 1024), then two port
records. Each has eight words (callback count, mode, RX, TX, error mask, done,
unexpected-command/input count, reserved) and a 1024-byte receive buffer.
The payload is byte `((i * 73) + ((i >> 8) * 19) + uart_number * 41) & 255`.
Matched counters without buffer/host comparison are not sufficient.

All error checks are in the existing callback path; zero latched errors does
not exclude physical errors between checks or errors in the bridge receiver.
Trials are short and reset-isolated. Payload traffic that succeeds here does
not waive the earlier repeated small-block echo failures. Twister remains
`build_only: true`.
