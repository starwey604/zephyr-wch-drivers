# Host-side fixtures (planned)

No host executable is supplied yet: the target has no USB UDC/bulk class and
the UART driver has no asynchronous DMA API. Add scripts alongside the first
working target implementation, so host and device agree on their protocol.

- UART: configurable ports/baud; framed sequence number, length and checksum;
  simultaneous bidirectional traffic, bounded timeouts and explicit failures.
- USB: require explicit VID/PID and interface/endpoint selection; vendor-specific
  bulk echo with payload lengths 0, 1, 63, 64, 65 and multi-packet transfers.
- Exercise reconnect/reset, stalls/recovery and repeated transfers separately.
- Never auto-flash devices or auto-detach unrelated host kernel drivers.

Do not assign a production VID/PID in test code. Use an appropriately authorized
development identity when the USB class is added. Keep captures/results outside
source control unless intentionally reviewed as test evidence.
