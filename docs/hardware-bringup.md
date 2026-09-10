# CH32V203 hardware gates

**Status: first board available; gate 1 partially executed.** The
[2026-09-10 report](hardware-20260910.md) records successful flashing, a board
startup-clock fix and paced UART1/CH340 echo. DMA/USB and the remaining gates
are not qualified. The [IRQ follow-up](hardware-20260910-irq.md) adds bounded
USART1 checks at 115200/921600; USART2 link and dual 3 Mbaud remain blocked.
Continue together with the board owner; native tests and
successful linking cannot waive any gate below.

This is the UART gate. USB was subsequently advanced independently at the
product owner's request; see [USB's separate hardware gate](usb-udc.md).
That does not qualify either peripheral or waive the UART timing checks.

## Prepare before powering the board

Confirm the actual board revision, schematic and MCU marking/package. Gello's
mapping is CH32V203F8U6 **QFN20**, not F8P6/TSSOP20. Use 3.3 V UART logic and
common ground, never RS-232 voltage. Check supply wiring/current limit and
WCH-LinkE RISC-V mode. Keep NRST accessible. Do not enable USB: its package
pads are shared with SDI, which we need for recovery and result inspection.

UART1: PA9 TX -> PA10 RX (package 15 -> 18); UART2: PA2 TX -> PA3 RX
(package 3 -> 4). Attach separate local-loopback jumpers only for the RX test.
Never short two TX outputs or leave an external adapter TX driving a loopback
RX. A high-impedance logic analyzer can observe both TXs and a shared ground.

## Ordered gates and stop criteria

1. **Power, debug, clock and polling.** Confirm chip identity, stable reset/debug
   and PA0 key input. Start at 115200 baud. Capture a known TX pattern externally
   and measure bit period before moving toward 3 Mbaud. Local loopback alone
   cannot detect a common TX/RX clock error. Stop on wrong baud/pins or unstable
   debug; investigate board/HSI/clock setup before DMA.
2. **DMA and TX.** Execute `tests/drivers/dma` memory-copy checks at 8/16/32-bit
   widths; then `wch.uart_dma.tx`. Confirm real PFIC IRQs and DMA1 CH4/CH7 TX
   routing. Compare transmitted bytes and USART TC with the last stop bit.
   Instrument abort/timeout and queued tail separately; the existing TX hardware
   test covers normal completion, not abort timing. Stop on count/IRQ/tail errors.
3. **RX baseline (implemented fixture).** Run the low-baud test below with both
   loopbacks. Check full/partial packets, release counts, reenable and paced
   handoff. Inspect DMA1 CH5/CH6, stable remaining counts after stop, and error
   flags. Breakpoints halt execution and distort timing: use them for stopped
   result inspection, not throughput conclusions. Stop on any mismatch/overrun.
4. **Required measurement before further RX implementation.** Use an independent
   sender/analyzer to vary byte arrival around DMA completion and RX disable;
   inject UART overrun/framing/noise where feasible. Measure rearm gaps and test
   read-STATR/read-DATAR plus RE transitions under controlled arrivals. Native
   MMIO cannot establish whether that sequence steals bytes. Only after these
   results should we implement IDLE/inactivity timeout and choose between
   normal-mode handoff and an internal cyclic staging buffer.
5. **Later qualification, not this increment.** After fixing observed races,
   run independent dual-port traffic with sequence numbers/CRC, large and tiny
   buffers, idle/burst boundaries, interrupt load and 3-Mbaud stress. Define and
   record duration/bytes/error criteria before running; require zero unexplained
   missing/duplicate/reordered bytes. FeTec/product adoption and USB are separate
   follow-up gates. No throughput claim follows from paced local loopback.

## Reproducible low-baud RX build

From the Ragtime dev workspace, using the manifest-pinned module:

```sh
.venv/bin/west build -s modules/drivers/wch/tests/drivers/uart_dma \
  -b ragtime_gello/ch32v203 -d build/test-wch-rx-hil --pristine always -- \
  -DEXTRA_CONF_FILE=$PWD/modules/drivers/wch/tests/configs/ragtime-cpp20.conf \
  -DEXTRA_DTC_OVERLAY_FILE=$PWD/modules/drivers/wch/tests/drivers/uart_dma/boards/rx-low-baud.overlay \
  -DCONFIG_UART_INTERRUPT_DRIVEN=n -DCONFIG_UART_ASYNC_API=y \
  -DCONFIG_WCH_UART_ASYNC_TX=y -DCONFIG_WCH_UART_ASYNC_RX=y
```

Add `-DZEPHYR_SDK_INSTALL_DIR=/home/ww/zephyr-sdk-1.0.1` if required. Confirm both
`current-speed` values are 115200 in generated `zephyr/zephyr.dts`. Flash only
after gates 1/2 and wiring review, using `.venv/bin/west flash
-d build/test-wch-rx-hil --runner wlink`. WCH-capable OpenOCD is needed for its
alternative debug runner; confirm probe/tool availability when the board arrives.

Both UART consoles remain disabled; USB is not a reporting channel. Use the
debugger with the matching ELF: break on `ztest_test_fail`, inspect ztest's
`test_status`, and inspect `wch_rx_hil_stage` (0 not started, 1 running, 2 RX
fixture passed). Stage 2 says nothing about other suites or stress qualification.
Set up failure capture before running, then avoid breakpoints in timing paths.
Before automated HIL, add an independent reporting transport/fixture; retain
`build_only: true` meanwhile. All fixture waits are bounded.

## Evidence to record per gate

Copy this checklist into a dated hardware report (do not record pending as pass):

- Board revision, MCU marking, supply, probe/analyzer model, wiring photo.
- Firmware/Zephyr/HAL/module full SHAs, build command, ELF and `.config`.
- Baud/traffic source, payload, transfer count, pacing and interrupt load.
- Gate/case: PASS / FAIL / NOT RUN; expected versus observed byte/event counts.
- Analyzer trace and stopped register snapshots; missing/duplicate/overrun count.
- Reproduction steps for failures, recovery performed, and next agreed action.

Keep secrets/unique device identifiers out of the public repository. Extend
dated reports with actual results; never turn partial gate coverage into a
blanket hardware qualification claim.
