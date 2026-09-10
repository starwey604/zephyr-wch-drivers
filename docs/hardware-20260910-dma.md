# 2026-09-10: first DMA memory-copy and dual UART hardware passes

**Bounded DMA functionality now passes on the physical CH32V203F8U6.**
Memory copy, dual UART TX and host-fed RX passed without production-driver
changes. This is not continuous-load, active-abort/deadline, 3-Mbaud or USBFS
qualification. The [WCH-Link CDC completion issue](hardware-20260910-usbmon.md)
remains separate; no CDC driver or probe firmware was changed.

## Setup and provenance

Same generic QFN20 board and corrected wiring: CH340E on USART1 PA9/PA10,
WCH-LinkE 2.19 on USART2 PA2/PA3, common ground, debug SDI retained. No wiring
changes, local-loopback jumpers, power/protection changes or USBFS enablement.
Read-only target/protection preflight confirmed CH32V20X `0x203d0500` and
unprotected flash; the flash tool also reported protection clear each time.
All five images were flashed and fully read back with byte equality.

- Driver base: `d54dfe5702d57c4c42803bd5c03fc846daab5ad9`; only tests/docs
  changed in this increment, including RAM diagnostics for existing fixtures.
- Zephyr: `577e42ad187825cc30d4b51423c32872eb4cf054` (workspace 4.4.99).
- HAL: `1713a445d44278e902a00e0fee3a111d7bde0d60`; Zephyr SDK 1.0.1.
- wlink source: `249f2c100005827dce8c7d82ff46917e52cddad9`, CH32V20X/low speed.
- GPIO mapping and clock setup unchanged; RX diagnostic CFGR0 `003c000a`,
  BRR `04e2` at 115200 and `009c` at 921600. No physical bit-period measurement.

## Executed results

| Test | Reset-isolated runs | Assertions / observations |
| --- | ---: | --- |
| DMA1 CH1 memory copy | 5/5 PASS | 256 bytes at each of 8/16/32-bit widths; exact memory equality, completion callbacks, stopped/zero remaining |
| Dual UART DMA TX, 115200 | 5/5 PASS | 1-byte then 128-byte TX per port; exact host bytes, two TX_DONE events, expected buffer/length, TC observed in both callbacks |
| Dual UART DMA TX, 921600 | 5/5 PASS | Same checks, no TX_ABORTED |
| Dual host-fed DMA RX, 115200 | 5/5 PASS | Five rounds per port: full 1, full 32, partial 7, paced 32+7 handoff, full 1024 |
| Dual host-fed DMA RX, 921600 | 5/5 PASS + 1/1 confirmation | Same data, length, buffer identity/release, request/disabled and error checks |
| Deliberately corrupted RX byte, 921600 | 1/1 expected rejection | UART1 round 1 receives 32 bytes but `match=0`; fixture stops with `-EIO` |

Thus 26 positive runs passed. The negative run remains **FAIL / exit 1** in its
raw JSON; it is not included as a passing transfer. It flipped only UART1 round
1 byte 0. DMA received the full 32 bytes, without an RX_STOPPED error, but the
content check rejected them (stage 255, round 1, one prior round passed,
error -5). Later rounds were not executed. UART2's round-1 match flag was not
evaluated after the first-port failure; its zero is not evidence of corruption.
The following clean 921600 run passed all five rounds on both ports.

RX uses actual external UART senders, not a software register model or payload
echo. Each port receives 1103 payload bytes per positive run. The MCU compares
every byte before reusing buffers and the host reads all per-round results
after traffic. Across eleven positive RX runs this is 24266 compared bytes.
The ten TX runs compare 2580 host-received payload bytes. DMA1 CH4/CH7 TX and
CH5/CH6 RX are the board's fixed requests, using the production external DMA
and UART drivers. Callback delivery exercises real DMA/PFIC interrupts.

## Images and retained evidence

All paths below are under Ragtime `build/`; `hil.jsonl` in each directory
contains full results. RX 921600 also has `negative.jsonl` and `confirm.jsonl`.
Each image has `readback.bin` beside its `zephyr/zephyr.bin` (the memory-copy
directory additionally retains each 32-byte RAM snapshot).

| Directory | ROM / RAM, bytes | Diagnostic symbol / size |
| --- | ---: | --- |
| `test-wch-dma-hil` | 35740 / 9152 | `wch_dma_hil_diag=0x20000450` / 32 |
| `test-wch-tx-hil` | 45096 / 10640 | `wch_tx_hil_diag=0x200006b4` / 64 |
| `test-wch-tx-hil-921600` | 45096 / 10640 | same TX layout |
| `test-wch-rx-host` | 25712 / 10336 | `wch_rx_host_diag=0x20000fb8` / 352 |
| `test-wch-rx-host-921600` | 25712 / 10336 | same RX layout; final installed image |

SHA-256 in that order:

```text
40437fad0b3f3d8fd93878851d945afcead12b0ae32846afbad65a486dbf0b34
a03ce1fcb9c2dbdb8b884a9728a7dfbc1db7461d127816127776d488a31d2c64
5d355e94c73b8f5444041def342d008646eb840c6d6aba5819e690cf4747f1aa
335818e3a7f2fec0843c901d1672222719fe4b2c7fb4fefb94657e2fdc359996
7a7517d453f62e77134721e5ae6b20ff784b4efbe8287e3bd13073d6b11ba985
```

## Reproduction

Use the [host-fed RX build/run instructions](../tests/drivers/uart_rx_host/README.md).
Builds isolate `hal_wch` and this external module with `ZEPHYR_MODULES`, keeping
unrelated Ragtime application changes out. For the memory-copy and TX fixtures,
use the same SDK/module/board arguments, with these differences:

- Memory copy: source `tests/drivers/dma`, append the low-baud overlay
  `tests/drivers/uart_dma/boards/rx-low-baud.overlay` via `EXTRA_DTC_OVERLAY_FILE`.
  After reset allow two seconds, then dump `wch_dma_hil_diag`. Its eight words
  must equal `(0x57444d41, 2, 3, 3, 0, 0, 0, 0)`: magic, stage, widths passed,
  callbacks, channel, API result, callback status, stop result. Stage 2 follows
  all data/progress assertions, not merely transfer initiation.
- TX: source `tests/drivers/uart_dma`, same low-baud overlay, plus
  `-DCONFIG_UART_INTERRUPT_DRIVEN=n -DCONFIG_UART_ASYNC_API=y
  -DCONFIG_WCH_UART_ASYNC_TX=y`. Do **not** enable RX for this capture runner,
  since the older combined fixture expects physical loopbacks. Run
  `tests/host/uart_dma_tx.py` with explicit ports, baud, matching diagnostic
  address and wlink path. It checks wire bytes plus the 64-byte TX record.
- 921600: separate build directory and the shared
  `tests/drivers/uart_direction/boards/ragtime_gello.overlay` instead of the
  low-baud overlay. This session never selected the board's 3-Mbaud default.

All waits are bounded. No SDI reads occur during measured traffic. Host Python
syntax checks and all five builds passed; the negative hardware check verifies
that content mismatch is rejected. Twister remains `build_only: true`.

## Remaining gates and final board state

RX handoff deliberately waits for a `NEXT` message before sending seven tail
bytes. Partial RX uses a 500-ms host-send window before explicit disable, not
a new driver timeout implementation. The 1024-byte round is a bounded burst,
not a sustained stream. TX's callback TC snapshots are not waveform proof of
exact last-stop-bit timing. Active abort, software deadline expiry, error
injection/recovery, simultaneous DMA TX+RX, gapless rearming, long stress and
3 Mbaud remain untested. No IDLE/inactivity timeout or cyclic RX was added.
These passes do not waive [the timing gates](hardware-bringup.md).

The final 921600 host-fed RX image remains installed, with async TX/RX support,
SDI retained and USBFS disabled. No old image was restored. The runner resets
after saving diagnostics; without a new host run, this fixture naturally
times out waiting for input. Saved JSON, not a later no-host timeout marker,
is the evidence for completed trials.
