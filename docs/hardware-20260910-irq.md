# 2026-09-10: USART2 and interrupt bring-up

This follow-up adds a dual-port binary IRQ fixture, not changes to the UART
driver. DMA/USB remain disabled and **unqualified**. USART2 and dual-port wire
tests are **blocked pending link/wiring diagnosis**, not passed.

## Setup and provenance

- Same CH32V203F8U QFN20 board and startup-clock fix as the
  [first session](hardware-20260910.md); Ragtime baseline `a23d949`.
- Zephyr `577e42ad187825cc30d4b51423c32872eb4cf054` (4.4.99), HAL
  `1713a445d44278e902a00e0fee3a111d7bde0d60`, SDK 1.0.1.
- Driver baseline `ca480f52ce9e5b5cf8deff72cad38a14db5a6d7c` plus this
  commit's `tests/drivers/uart_irq` and `tests/host/uart_irq.py`.
- WCH-LinkE 2.19, pinned wlink 0.1.2 source `249f2c1`; RISC-V debug retained.
  Exact target and clear read/write protection checked before flashing.
- USART1 uses onboard CH340E; USART2 reportedly connected to WCH-Link CDC.
  Both USB devices enumerated. Actual USART2 wire path remains unconfirmed.

## Observed results

| Test | Result | Evidence |
| --- | --- | --- |
| 115200/921600/3000000 fixture builds | PASS | Each ROM 17,928 B, RAM 7,488 B |
| USART1 IRQ RX + TX, 115200 | PASS, bounded | 513 baseline + 65,536 patterned bytes each direction; exact match |
| USART1 IRQ RX + TX, 921600 | PASS, bounded | Same 66,049 bytes each direction; exact match |
| USART2, 115200 | BLOCKED | No boot bytes at host; no MCU RX bytes after host probe |
| Concurrent two-port traffic | NOT RUN | Runner stops at missing USART2 banner |
| Dual 3 Mbaud on wire | NOT RUN | Current bridges not documented for dual 3 Mbaud |
| DMA, USB, cold reset, analyzer timing | NOT RUN | Not qualified by these IRQ tests |

Both successful USART1 runs reported 132,160 IRQ callbacks, RX 66,049,
TX 66,111 (includes 62-byte banner), zero UART errors, zero queue overflow
and zero failure latch. Timed 65,536-byte block phases took 5.726 s at 115200
and 0.817 s at 921600. These are host request/echo measurements, **not** a
continuous line-rate, CPU-load or oscillator-accuracy qualification.
Boot RCC CFGR0 was `003c000a`; both BRRs were `04e2` at 115200 and `009c`
at 921600. Debugger access occurred only after traffic.

## USART2 stop point

At 115200 the diagnostic showed 62 TX IRQ callbacks and 62 bytes fed to the
UART, but zero RX bytes. Opening CDC with DTR/RTS high also received no banner
and no echo; this did not resolve the issue. A transmitted software count
does **not** prove the signal reached the external pad/adapter.

Post-test GPIOA CFGLR `44448b44` selects PA2 AF push-pull and PA3 pull-up;
AFIO PCFR1 was zero (USART2 not remapped). Both UARTs initialized and BRRs
matched. Next verify **PA2 -> Link RX, PA3 <- Link TX, common GND**, including
header identity/contact, then repeat at 115200. If correct, isolate the bridge
with a user-installed TX/RX loopback or measure the MCU pads. Do not blindly
swap driven outputs, change firmware protection or disable debug.

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
the dual-IRQ **115200** image for the next wiring check.
Its complete 17,928-byte flash readback matched the binary; SHA-256:
`e201f99cf845e35a3f5d6417f35da9199e7cd8300b44e2dd29bc75d2f8bf42b7`.
