# 2026-09-10: direction isolation and recoverable 64-byte echo stalls

**Some apparent USART2 return loss is delayed delivery, not destroyed bytes.**
After a one-second echo timeout, sending one further byte recovered all missing
64 bytes plus that byte, in order, in 12/12 reproduced failures. This does not
explain every earlier short loss or the instrumented UART overrun observations.
No production driver code changed in this session. High-rate UART remains
unqualified; DMA, native USBFS and 3 Mbaud were not tested.

## Setup and reproducibility

Same CH32V203F8U6 board, corrected J1 wiring, onboard CH340E on USART1 and
WCH-LinkE firmware 2.19 CDC on USART2 as the
[earlier IRQ campaign](hardware-20260910-irq.md). Both ports are 921600 8N1;
clock register `CFGR0=003c000a`, BRR `009c`. Driver timing trace, DMA, async API
and USBFS are disabled. Boot banners use polling; all tested payload uses IRQs.

- Zephyr: `577e42ad187825cc30d4b51423c32872eb4cf054` (4.4.99 workspace).
- HAL: `1713a445d44278e902a00e0fee3a111d7bde0d60`; SDK 1.0.1.
- Production driver base: `1aa8e1ca404af2c63a52a562af15c547c9b613c1`.
- Host kernel: `7.2.2-1-cachyos`; wlink source:
  `249f2c100005827dce8c7d82ff46917e52cddad9`.
- New source/runner: this commit's
  [direction fixture](../tests/drivers/uart_direction/README.md) and
  `tests/host/uart_direction.py`. Build using its isolated-module command.

Three successive diagnostic app builds added commands; the production driver
and build configuration stayed unchanged. These are not byte-identical images
or a controlled timing benchmark. All have RAM usage 9536 B and diagnostic
symbol `wch_direction_diag=0x20000a28`, size 2136 B.

| Build directory under `build/` | Commands available | ROM bytes |
| --- | --- | ---: |
| `test-wch-uart-direction` | R/T | 17992 |
| `test-wch-uart-direction-duplex` | R/T/B/D | 18160 |
| `test-wch-uart-direction-echo` | R/T/B/D/E | 18232 |

Binary SHA-256, respectively:

```text
4c9560627af0e4c81af733d445ba3f3e9a9b6c74f99a446cd04db2fa24f2819a
b2ea408462297d4ffc731a57707aff8d5b8ab870f18a86ea98d5849c9e30bd8a
0601266f48c045da551c5b43a1ab6cf9e668d0d6c41c1f0801ecbc9ad6abe790
```

Local JSON evidence is `matrix.jsonl` in each build directory, plus
`probe-confirm.jsonl` in the last. Target/protection checked before flashing;
final full 18232-byte flash readback matched. No power, protection or probe-mode
changes. Debug RAM reads happen only after traffic. Resets separate trials and
follow the final capture; **the last 921600 direction/echo image stays installed**.

## Results: 103 reset-isolated trials

Each successful selected direction transfers and compares exactly 1024 payload
bytes. Pure RX checks the actual MCU RAM buffer; TX checks actual host bytes.
Counters alone are not a pass. Command bytes are excluded.

| Test | Pass / trials |
| --- | ---: |
| RX1, RX2, dual RX, TX1, TX2, dual TX, RX1+TX2, TX1+RX2 | 5/5 each; 40/40 |
| Batch echo on UART1, UART2, both | 5/5 each; 15/15 |
| Independent full duplex on UART1, UART2, both | 5/5 each; 15/15 |
| UART2 immediate echo, 1024-byte chunks, no added gap | 5/5 |
| UART2 immediate echo, 32-byte chunks, no added gap | 5/5 |
| UART2 immediate echo, 64-byte chunks, no added gap | 1/5 |
| UART2 immediate echo, 64-byte chunks, 2-ms gap | 5/5 |
| UART1 immediate echo, 64-byte chunks, no added gap | 5/5 |
| Dual immediate echo, 64-byte chunks, no added gap | 0/5 |
| Additional UART2 64-byte probe confirmation | 0/3 |

All twelve failed trials remain **FAIL**, even though the optional probe
recovered their missing data. All boot identities matched. In dual failures
UART1 completed its own payload; that is not a dual-port pass.

Concrete confirmation: host sent 128 payload bytes but received 64. After the
timeout it sent payload byte 128; the response contained exactly bytes 64..128
(65 bytes). MCU RX/TX counters were both 129, received-prefix equality true,
and latched UART errors zero. Other confirmations stalled after 192 or 256
sent bytes with the same 64-byte deficit and complete recovery. The earlier
nine matrix failures also satisfy exact prefix-plus-probe equality.

The fixture holds data until received and feeds it into DATAR; its TX counter
is not USART TC. Nevertheless the complete recovered host bytes establish that
this subset was not permanent byte loss. Zero callback-entry error flags does
not exclude all hardware errors; the
[trace report](hardware-20260910-irq-trace.md) explains that observation limit.

## Interpretation and next discriminating check

Read-only USB descriptors show WCH-Link CDC bulk IN `0x83`/OUT `0x03` have
64-byte maximum packets (CH340 bulk endpoints have 32-byte maximum packets).
[Upstream Linux cdc-acm](https://raw.githubusercontent.com/torvalds/linux/master/drivers/usb/class/cdc-acm.c)
normally allocates an RX request of twice the endpoint packet size, except for
its single-RX-URB quirk. That is consistent with 128-byte reads here; it is
**not a capture or verification of this custom kernel's actual requests**.

A USB full-packet/short-packet or idle-flush interaction in the bridge/host
return path is therefore a leading hypothesis. The 2-ms pacing result is a
diagnostic clue, not a production fix. Neither a Linux bug nor a WCH firmware
bug has been established. Recoverable bytes argue against destructive signal
corruption for these cases, not against every possible board/wiring problem.

Next capture the targeted WCH-Link USB traffic and compare request submission,
completion lengths and timing across a stall/probe. Current user access to
usbmon is unavailable; no capture, driver unbind, kernel patch or probe update
was performed. USB capture setup needs coordination with the owner. Keep the
earlier non-64-byte losses and trace-induced ORE as separate open issues.

Software checks: all three board builds linked; final host script passed Python
syntax compilation. A fake-port check accepted complete delayed recovery and
rejected a corrupted recovered byte, retaining the original failed transfer
in both cases. The final three hardware confirmations also exercised the
explicit recovery/prefix fields. Twister remains `build_only: true`.
