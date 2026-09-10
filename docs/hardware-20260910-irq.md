# 2026-09-10: USART2 and interrupt bring-up

This follow-up adds a dual-port binary IRQ fixture, not changes to the UART
driver. DMA/USB remain disabled and **unqualified**. After correcting USART2
TX/RX wiring, dual-port IRQ echo passed at 115200. USART2 burst echo at 921600
lost return bytes; that rate and dual 3 Mbaud remain unqualified.

## Setup and provenance

- Same CH32V203F8U QFN20 board and startup-clock fix as the
  [first session](hardware-20260910.md); Ragtime baseline `a23d949`.
- Zephyr `577e42ad187825cc30d4b51423c32872eb4cf054` (4.4.99), HAL
  `1713a445d44278e902a00e0fee3a111d7bde0d60`, SDK 1.0.1.
- Driver baseline `ca480f52ce9e5b5cf8deff72cad38a14db5a6d7c` plus the fixture
  in `b5e536b6476b24fcaeee9458889e4c77ca822268`. The rewiring retest used the
  same binaries and host runner, without driver changes.
- WCH-LinkE 2.19, pinned wlink 0.1.2 source `249f2c1`; RISC-V debug retained.
  Exact target and clear read/write protection checked before flashing.
- USART1 uses onboard CH340E; USART2 uses WCH-Link CDC. J1's PD00/PD01
  annotations are misleading: schematic nets connect J1 pin 3 (PD00/TX2) to
  PA2 and J1 pin 2 (PD01/RX2) to PA3. The user initially connected RX-to-RX
  and TX-to-TX, then corrected to **PD00 -> Link RX, PD01 <- Link TX**.

## Observed results

| Test | Result | Evidence |
| --- | --- | --- |
| 115200/921600/3000000 fixture builds | PASS | Each ROM 17,928 B, RAM 7,488 B |
| USART1 IRQ RX + TX, 115200 | PASS, bounded | 513 baseline + 65,536 patterned bytes each direction; exact match |
| USART1 IRQ RX + TX, 921600 | PASS, bounded | Same 66,049 bytes each direction; exact match |
| USART2 and concurrent two-port traffic, 115200 | PASS, bounded after rewiring | Initial 66,049 and final 262,657 bytes per port per direction; zero device errors/overflow |
| USART2 burst echo, 921600 | FAIL | 256-byte request returned 249 bytes; later burst lost a full 64-byte prefix |
| Concurrent two-port traffic, 921600 | NOT RUN | USART2 sequential prerequisite failed before concurrent phase |
| Dual 3 Mbaud on wire | NOT RUN | Current bridges not documented for dual 3 Mbaud |
| DMA, USB, cold reset, analyzer timing | NOT RUN | Not qualified by these IRQ tests |

Both successful USART1 runs reported 132,160 IRQ callbacks, RX 66,049,
TX 66,111 (includes 62-byte banner), zero UART errors, zero queue overflow
and zero failure latch. Timed 65,536-byte block phases took 5.726 s at 115200
and 0.817 s at 921600. These are host request/echo measurements, **not** a
continuous line-rate, CPU-load or oscillator-accuracy qualification.
Boot RCC CFGR0 was `003c000a`; both BRRs were `04e2` at 115200 and `009c`
at 921600. Debugger access occurred only after traffic.

## Initial USART2 stop point (resolved by rewiring)

At 115200 the diagnostic showed 62 TX IRQ callbacks and 62 bytes fed to the
UART, but zero RX bytes. Opening CDC with DTR/RTS high also received no banner
and no echo; this did not resolve the issue. A transmitted software count
does **not** prove the signal reached the external pad/adapter.

Post-test GPIOA CFGLR `44448b44` selects PA2 AF push-pull and PA3 pull-up;
AFIO PCFR1 was zero (USART2 not remapped). Both UARTs initialized and BRRs
matched. Correcting J1 TX/RX connections resolved the silent link. No pinmux
or driver changes were needed. Keep common ground and debug wiring unchanged.

## Rewiring retest and higher-rate stop point

At 115200, both boot banners and both 513-byte sequential prerequisites passed,
then each port completed 128 concurrent 512-byte request/echo blocks. Timed
phases took 5.722 s (USART1) and 5.762 s (USART2). Each port's post-run snapshot
showed 132,160 callbacks, RX 66,049, TX 66,111 and zero error/overflow/failure.

After the higher-rate checks, the restored 115200 image passed another run
with 512 concurrent blocks per port: 262,657 bytes per port per direction
including the 513-byte baseline. Timed block phases took 22.883 s and 23.037 s.
Each port reported 525,376 callbacks, RX 262,657, TX 262,719, and zero
error/overflow/failure. Full flash readback matched before this final run;
the target was reset after post-run diagnostic capture.

At 921600, both boot banners and USART1's sequential cases passed. USART2's
first 256-byte pattern returned only 249 bytes, first difference at offset 64.
MCU USART2 RX was 257 and TX 319 (one preceding byte + pattern + banner),
with zero errors/overflow/failure. The requested 2048-round concurrent phase
**never started**; this is not a 1-MiB pass.

A separate USART2-only run completed 32 request/echo chunks of 32 bytes
(1024 bytes), followed by two matching 256-byte bursts. The third burst
returned exactly bytes `40..ff` (192 bytes), missing its first 64 bytes.
MCU RX was 1792 and TX 1854, again zero errors/overflow/failure. This narrows
the symptom to the return path after the software's TX accounting, but does
**not** prove on-wire TX correctness or establish a WCH-Link firmware defect.
An additional dual-run attempt failed its boot-banner check with a partial
USART1 line, before payload tests; do not count it as traffic coverage.

Stop high-rate qualification here. Next compare USART2 TX with an independent
logic analyzer/receiver and separately test the WCH-Link UART bridge. Preserve
the failing burst result even if paced traffic or later reruns pass. Do not
patch the UART driver merely to hide the lost bytes.

## 3-Mbaud prerequisite

[WCH-Link manual v2.4, table 3](https://www.wch.cn/uploads/file/20250124/1737704462135866.pdf)
lists supported WCH-LinkE UART rates through 921600.
[WCH's interface selection table](https://www.wch-ic.com/products/productsCenter/mcuInterface?categoryId=1&tName=Extension%2FIsolation)
lists CH340E at 2 Mbps. A successful Linux baud-setting ioctl would not prove
either bridge runs at 3 Mbaud. The 3-Mbaud image was compiled, **not flashed**.
Resume qualification with independently validated 3-Mbaud endpoints and timing
capture; do not attach a second transmitter to the onboard CH340-driven RX.

## Reproduction and retained artifacts

See [fixture commands](../tests/drivers/uart_irq/README.md). Local artifacts
are under Ragtime `build/test-wch-irq-{115200,921600,3mbaud}`: matching ELF,
binary, config, generated DTS, host logs and post-run diagnostic dumps. These
local artifacts are not public repository fixtures. The board is returned to
the dual-IRQ **115200** image as the known working baseline.
Its complete 17,928-byte flash readback matched the binary; SHA-256:
`e201f99cf845e35a3f5d6417f35da9199e7cd8300b44e2dd29bc75d2f8bf42b7`.
