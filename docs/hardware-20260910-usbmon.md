# 2026-09-10: WCH-Link USB request completion stalls

**The reproduced 64-byte echo deficit reaches a host USB receive buffer, but
does not complete its 128-byte request normally.** This narrows the
[direction-isolation finding](hardware-20260910-uart-direction.md). It is not
evidence of CH32 losing those UART bytes. Earlier non-64-byte losses and
trace-induced ORE remain open; high-rate UART is still unqualified.

## Unchanged DUT and capture setup

No flashing or firmware changes. The direction/echo fixture remains 921600
8N1, trace/DMA/async/USBFS disabled. Full 18232-byte readback matched SHA-256
`0601266f48c045da551c5b43a1ab6cf9e668d0d6c41c1f0801ecbc9ad6abe790`.
Build, wiring, clocks, tools and ELF are in the previous report; fixture/runner
base is `f90f51d27920882f1ed0968ccce908c17635f334`.

The owner loaded usbmon and granted user read access to bus 3. WCH-LinkE 2.19
was `1a86:8010`, address 48; kernel `7.2.2-1-cachyos`. The new
`tests/host/usbmon_uart.py` uses binary `MON_IOCX_GETX` on read-only
`/dev/usbmon3`, retaining only this device's bulk OUT `0x03`/IN `0x83`.
Other devices, control and debug endpoints are discarded before recording.
No driver detach/rebind, probe update, USB configuration, power or protection
changes. No debug reads occur during serial traffic.

## Thirteen short trials with simultaneous capture

All use `uart_direction.py --case echo2 --echo-chunk 64`, diagnostic address
`0x20000a28` and the explicitly selected WCH-Link serial port.

| Case | Serial pass / trials | USB observation |
| --- | ---: | --- |
| No gap, probe after timeout | 0/5 | Added byte completes IN with missing 64 + 1 bytes |
| No gap, no probe; close after timeout | 0/3 | Cancelled IN contains exactly the missing 64 bytes |
| 2-ms gap after each successful chunk | 5/5 | All 80 payload chunks complete as 1 + 63 bytes |

Every boot matched. In all eight failed trials MCU received-prefix equality
matched, RX and DATAR-fed TX counts matched sent bytes (including probes),
and callback-entry error flags were zero. This does not expand the fixture's
error-observation coverage. Forensic recovery never converts a failure to PASS.

Local evidence under `build/test-wch-uart-direction-echo/`:

- `usbmon-probe.jsonl` and `usbmon-probe-hil.jsonl`: 264 selected USB events.
- `usbmon-close.jsonl` and `usbmon-close-hil.jsonl`: 146 selected USB events.
- `usbmon-paced.jsonl` and `usbmon-paced-hil.jsonl`: 672 selected USB events.

All captures finished with zero queued/dropped events and no caller-buffer
truncation. Raw logs contain opaque kernel URB identifiers and remain local.
No camera/Bluetooth traffic was retained.

## Observed request sequence

IN submissions are **128 bytes on this actual kernel**, not just inferred from
upstream source. First probe trial, relative to its first bulk event:

| Time, ms | URB event, not individual packet timing |
| ---: | --- |
| 0.000..0.035 | Host queues sixteen 128-byte IN requests |
| 497.822 | First 64-byte payload OUT submitted |
| 498.010 / 498.957 | IN completes with 1 / 63 bytes; first echo complete |
| 499.029 / 499.120 | Second 64-byte OUT submitted / completed successfully |
| Until 1506.403 | No further IN completion; serial read times out |
| 1506.403 | One probe byte OUT submitted |
| 1506.534 | Pending IN completes successfully with 65 bytes |

Across five trials, the 65-byte completion follows probe OUT submission by
129..162 microseconds. Captured bytes exactly equal the missing prefix plus
the probe; the probe is not a retransmission command.

The no-probe control sends no extra UART data. On serial close, one pending
IN completes with `status=-2` (`-ENOENT`) and `actual_length=64`. Its captured
bytes exactly fill the deficit in all three trials (128, 128 and 192 payload
bytes originally sent). Cancellation occurs about 1.04 seconds after the last
64-byte OUT submission. Thus the host request buffer contains the bytes **at
cancellation**; this does not timestamp their earlier USB packet arrival.

## Interpretation and limits

Linux documents cancellation status and short-packet termination in its
[USB host API](https://docs.kernel.org/driver-api/usb/usb.html).
[Upstream cdc-acm](https://raw.githubusercontent.com/torvalds/linux/master/drivers/usb/class/cdc-acm.c)
normally requests twice the endpoint packet size and treats cancelled reads
separately from successful delivery. This fits the observed submissions and
why closing is not a serial recovery mechanism. Submit status `-115` and
close-time cancellation `-2` are not spontaneous USB transport errors.

The leading explanation is a full-packet termination/idle-flush compatibility
problem on WCH-Link CDC IN: a 64-byte full packet does not itself end a
128-byte request; a subsequent short packet can end it. Probe and paced 1+63
results fit this. Timely device-side short/ZLP termination is the first thing
to investigate, **not a speculative CH32 UART driver patch**. Request completion
behavior is observed; precise responsibility between bridge firmware and host
controller is not established.

[usbmon](https://docs.kernel.org/usb/usbmon.html) observes driver/HCD requests,
not individual tokens, NAKs or ZLPs. It cannot directly prove WCH-Link omitted
a ZLP. There is no USB/UART waveform capture or known-good bridge A/B test.
These findings cover this specific deficit, not every earlier loss or baud.
The board's Type-C remains CH340, not CH32 native USBFS; no UDC qualification
follows from this capture.

## Handoff

Prefer a known-good dedicated USB-UART bridge for the USART2 comparison,
retaining WCH-Link for debug. Another discriminating test would be explicitly
authorized temporary CDC-interface detachment and 64-byte raw USB reads; that
changes the driver binding and was **not done**. Do not add arbitrary bytes
to a product protocol or treat 2-ms pacing as a fix. Probe updates and host
quirks need separate review/testing.

Six decoder unit tests passed: recovery, cancelled partial data, absent data,
OUT payload, device/endpoint filter and truncation/short header. Python syntax
checks passed. See [host instructions](../tests/host/README.md). After final
RAM capture the board was reset, retaining the same 921600 diagnostic image.
