# First-power UART polling fixture

For the CH32V203F8U6 development board with onboard CH340 connected to
USART1 PA9/PA10. No loopback jumpers or WCH-Link UART wires are needed.
WCH-Link SDI remains available; USBFS and DMA are not enabled. USART2 is off.

At 115200 8N1 the firmware sends a startup banner and one idle heartbeat per
second. A bounded ASCII line (up to 63 characters, LF or CRLF terminated)
receives `ECHO <line>`. Overlong lines return an error. Only send printable ASCII.
Pace host bytes at 50 ms: the default 100 Hz kernel tick and sleeping poll loop
cannot receive an unpaced burst without overrun. `tests/host/uart_polling.py`
checks 0/1/15/63-character lines, overflow rejection and CRLF recovery:

```sh
.venv/bin/python ../zephyr-wch-drivers/tests/host/uart_polling.py \
  --port /dev/serial/by-id/usb-1a86_USB_Serial-if00-port0
```

This is a paced functional test, not a continuous RX or baud-accuracy benchmark.
The heartbeat's CPU frequency is the configured value, not a measurement.
`wch_poll_diag` records progress and boot RCC/EXTEN/BRR snapshots in SRAM;
the startup line also reports the latter without needing a debugger attachment.

Build from Ragtime using the standalone driver checkout (adjust SDK path):

```sh
.venv/bin/west build -s ../zephyr-wch-drivers/tests/drivers/uart_polling \
  -b ragtime_gello -d build/test-wch-polling-hil --pristine always -- \
  -DBOARD_ROOT=$PWD/firmware \
  "-DZEPHYR_MODULES=$PWD/modules/hal/wch;$PWD/../zephyr-wch-drivers" \
  -DZEPHYR_SDK_INSTALL_DIR=/home/ww/zephyr-sdk-1.0.1
```

The explicit module list isolates first-power testing from product libraries.
Before flashing, identify the exact probe/MCU and check protection read-only;
do not automatically unprotect a chip. Flash replaces existing application code.
Keep hardware results separate from compile-only Twister results.
