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

- UART host traffic generator: configurable ports/baud, framed sequence number,
  length/checksum, simultaneous bidirectional traffic and bounded failures.
- USB sustained traffic, reconnect/reset and suspend/resume qualification.
  The current runner requests an optional reset but requires a separate rerun
  to verify re-enumeration.

Hardware tests have not been executed; fake-API tests do not qualify transfers.
