# Host-side fixtures

`usb_endpoints.py` performs targeted EP0/bulk/STALL checks on a physical DUT;
see [the USB runbook](../../docs/usb-udc.md). PyUSB/libusb are needed only for
hardware execution. No device is accessed by these software tests:

```sh
python -m unittest discover -s tests/host -p test_usb_endpoints.py
python -O -m unittest discover -s tests/host -p test_usb_endpoints.py
```

The runner requires an explicit DUT bus/address and validates its identity and
endpoints before configuration writes. It never flashes devices or automatically
detaches kernel drivers. Test VID/PID are laboratory placeholders, not a
production allocation. Keep captures/results outside source control unless
intentionally reviewed as test evidence.

## Remaining fixtures

- `uart_direction.py` performs bounded IRQ direction/echo HIL with post-traffic
  RAM capture and optional stalled-return probes; see
  [its fixture](../drivers/uart_direction/README.md). It resets the target, but
  does not flash or restore firmware. Failed probes remain failed trials.
- `uart_polling.py` now verifies the first-power UART1/CH340 firmware with
  50 ms inter-byte pacing; see [its fixture](../drivers/uart_polling/README.md).
  It uses pyserial and never flashes or resets a device.
- UART high-rate host traffic generator: configurable ports/baud, framed sequence number,
  length/checksum, simultaneous bidirectional traffic and bounded failures.
- USB sustained traffic, reconnect/reset and suspend/resume qualification.
  The current runner requests an optional reset but requires a separate rerun
  to verify re-enumeration.

The paced UART1 checks have run on one board; USB hardware tests have not.
Fake-API tests do not qualify transfers.

## Targeted WCH-Link USB monitor

`usbmon_uart.py` is a read-only, 1..60-second x86-64 Linux binary usbmon
capture. It validates the explicit bus/address as `1a86:8010`, then retains
only CDC bulk endpoints `0x03`/`0x83`. No other device traffic, control traffic
or debug endpoints are written. The bus monitor itself sees the whole bus;
keep camera/Bluetooth or other private traffic idle where possible. Local
JSON includes opaque kernel URB identifiers: do not publish it unreviewed.

After the owner loads `usbmon` and grants read access to `/dev/usbmon<N>`, run
from Ragtime in one terminal (substitute the currently verified bus/address):

```sh
.venv/bin/python ../zephyr-wch-drivers/tests/host/usbmon_uart.py \
  --bus 3 --address <WCH-Link-address> --seconds 30 > build/wch-usbmon.jsonl
```

Wait for `usbmon ready`, then in another terminal run the
[direction runner](../drivers/uart_direction/README.md), `--case echo2`,
`--echo-chunk 64`, `--trials 5`, optionally `--probe-after-failure`.
Without the probe, the existing runner closes the serial port after timeout;
capture preserves cancelled URBs' status, actual length and bytes. Closing is
not a recovery/pass, and the capture script itself never opens/closes a serial
port. Use `--gap-ms 2` for the pacing control. Allow capture to finish normally
and require zero dropped/queued events before interpreting absence of events.
JSON `truncated` flags caller-buffer truncation; a missing data payload is not
evidence of a zero-length USB transfer. The script never flashes, rebinds kernel
drivers, changes USB configuration, or alters monitor permissions.

Read [the captured hardware evidence](../../docs/hardware-20260910-usbmon.md)
before attributing a stall to UART hardware. usbmon reports URB-level events,
not individual USB packets or UART waveforms. Decoder software checks:

```sh
python -m unittest discover -s tests/host -p test_usbmon_uart.py
```
