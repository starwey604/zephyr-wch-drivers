# Dual USART interrupt hardware fixture

This opt-in application echoes binary bytes using `uart_fifo_read()` and
`uart_fifo_fill()` in the interrupt callback on **both** ports. Even the boot
banners use TX interrupts. DMA, async UART and USB are disabled; SDI remains
available. It is not a production protocol or a sustained-throughput benchmark.

Use separate 3.3 V TTL bridges, common ground, and crossed TX/RX:

- USART1: PA9 TX -> bridge RX; PA10 RX <- bridge TX (onboard CH340E).
- USART2: PA2 TX -> WCH-Link RX; PA3 RX <- WCH-Link TX.
- Leave WCH-Link power outputs disconnected when the board is USB-powered.
- Do not connect a second adapter TX to PA10 while CH340 TX still drives it.

From the Ragtime workspace, build without importing unrelated product modules:

```sh
.venv/bin/west build -s ../zephyr-wch-drivers/tests/drivers/uart_irq \
  -b ragtime_gello -d build/test-wch-irq-115200 --pristine always -- \
  -DBOARD_ROOT=$PWD/firmware \
  "-DZEPHYR_MODULES=$PWD/modules/hal/wch;$PWD/../zephyr-wch-drivers" \
  -DZEPHYR_SDK_INSTALL_DIR=/path/to/zephyr-sdk
```

Default baud is 115200, 8N1. For a separate higher-baud build, append
`-DEXTRA_DTC_OVERLAY_FILE=<absolute fixture path>/boards/921600.overlay`
or `boards/3mbaud.overlay`. Verify generated DTS/config before explicitly
flashing. **3 Mbaud requires independently validated bridges and bit timing**:
the current WCH-Link/CH340E setup is not qualified for this rate.

After target/protection checks and an explicitly authorized flash, use pyserial:

```sh
.venv/bin/python ../zephyr-wch-drivers/tests/host/uart_irq.py \
  --uart1 /dev/serial/by-id/<CH340> --uart2 /dev/serial/by-id/<WCH-Link-CDC> \
  --baud 115200 --reset-tool "$PWD/build/wch-tools/bin/wlink"
```

The host opens only explicit devices, sets DTR/RTS low, resets the target using
the supplied wlink binary, checks port-specific boot banners, checks three
single-port patterns (513 bytes each), then runs 128 concurrent 512-byte
request/echo blocks per port with different deterministic sequences. Full
byte comparison and a trailing-data check must pass. `--only 1` or `--only 2`
isolates a port and requires only its device argument; `--rounds` is bounded.
No flash, protection, probe-mode or power commands are issued by the script.

The ISR owns each 1024-byte queue after initialization. Any UART error or
queue overflow latches failure and disables that port's IRQs; reset is required.
After traffic (never during timing measurements), inspect `wch_irq_diag` using
the matching ELF. It is 17 little-endian 32-bit words: magic `0x57434849`, stage
(2 ready), boot CFGR0, two BRRs, then six words per port: callback count, RX
bytes, TX bytes, error mask, overflow count, failure latch. TX includes the
62-byte banner. Require exact byte counts and zero errors/overflow/failure.
Debugger attachment can perturb clocks; reset before any subsequent wire test.

Twister remains `build_only: true`. See the
[hardware results and remaining blockers](../../../docs/hardware-20260910-irq.md).
