# 2026-09-10: IRQ timing/error flight recorder

Added diagnostic logging, not a fix for the earlier burst loss. No waveform
capture, DMA, USBFS or 3-Mbaud tests were performed. The board owner requested
that the latest image remain installed; the final image is **dual IRQ 921600
with sampled RAM tracing**, not the previous 115200 image.

## Reproducible final configuration

Same board, corrected J1 wiring, WCH-LinkE 2.19, Zephyr/HAL/SDK and unmodified
echo application as the [previous campaign](hardware-20260910-921600.md).
Use this commit's driver and host tools, with `trace.conf`, the 921600 overlay,
sample interval 127 and slow threshold 8 us. See the
[build/capture instructions and limitations](uart-irq-trace.md).

- Final build: `build/test-wch-irq-trace-final`, ROM 20,724 B, RAM 9,760 B.
- Flash image SHA-256:
  `bd8f47c10879639cdcef143fbfb1380870c49c5a5b5f5d1c855d8f5173599cd1`.
- Exact target/protection checked before flashing; full 20,724-byte readback
  matched. No protection, mode or power changes.
- Matching ELF symbols: `wch_irq_diag=0x20001320`,
  `wch_uart_irq_trace=0x20000820`, trace size 2280 bytes.
- Local complete JSON: `build/test-wch-irq-trace-final/trace-matrix.jsonl`.

## Final sampled-trace trials

Each case had three reset-isolated trials, up to 128 request/echo blocks per
port. Stop each worker at its first mismatch; read all diagnostics only after
both workers finish. All boot banners matched, zero boot failures.

| Case | Pass / trials | Observation |
| --- | ---: | --- |
| USART1, 512-byte blocks | 3 / 3 | 65,536 payload bytes each direction per trial |
| USART2, 512-byte blocks | 0 / 3 | Return loss; no latched hardware errors |
| Dual, 512-byte blocks | 0 / 3 | USART2 hardware ORE in all three trials |
| Dual, 32-byte blocks | 0 / 3 | USART2 hardware ORE in all three trials |

USART1 finished its payload in the dual trials after USART2 had failed; that
does not mean both ports sustained the complete test concurrently.

One concrete error snapshot (dual 32-byte, trial 1, zero-based):

```text
USART2 first_error_site   = 4       (fifo_read)
       first_error_status = 0xA8    (TXE | RXNE | ORE)
       first_error_rx     = 17
       first_error_tx     = 78      (includes 62 boot bytes)
       api_error_or       = 0x08
       fifo_error_or      = 0x08
```

The first ORE was seen by the existing status read in `fifo_read`, not by the
preceding `err_check`; RAM logging retains it after DATAR is read. Later
`err_check` also saw ORE and the existing fixture stopped that port. This
demonstrates why an entry-only check is not a complete observation window;
it does not establish that the old firmware's every loss had this cause.

The largest sampled driver-callback duration was nominally 4.042 us, and
entry-to-RX-read was 1.556 us. No sampled callback reached the 8-us threshold.
**These are sampled windows, not IRQ pending latency or full ISR execution.**
USART2 often overran before any receive callback was sampled; zero RX timing
in those records means missing timing coverage, not instantaneous service.

## Observer effect and intermediate experiments

Two intermediate logging builds (full callback timing, then every 128th
callback) each ran the same four cases three times: USART1-only passed 3/3;
USART2-only and both dual cases failed 3/3. Full timing saw first-error site 4
in a dual burst. The even sampling interval repeatedly measured TX only under
alternating RX/TX, so the final interval is odd (127). Intermediate ELF/binaries
and JSON remain under `build/test-wch-irq-trace-921600` and
`build/test-wch-irq-trace-sampled`; their provisional layouts differ from the
final decoder ABI. These experiments are not a clean single-variable timing
benchmark and do not replace the final results above.

The earlier uninstrumented dual 32-byte tests passed 5/5, whereas all tracing
variants overran. Therefore tracing materially perturbs service timing even
with sampling. Captured ORE is real in the instrumented run, but cannot by
itself explain the original packet-aligned losses. Keep MCU-service overrun
and bridge/return loss as separate hypotheses. Further causal timing work
needs lower-overhead instrumentation or external observation; no speculative
IRQ/driver behavior change is included in this increment.

## Software checks and handoff

- Trace-disabled 921600 binary is byte-identical to the prior non-trace image
  (`1b1aab18...`); default users incur no trace code/storage.
- DMA TX/RX fixture rebuilt successfully, tracing off (compile-only).
- Host CTest passed ring overwrite/freeze, error-site latches, timer wrap,
  port isolation and 127-IRQ sampling checks. Decoder checked ring order,
  nominal units, sample counts and malformed/short dumps; Python syntax passed.
- IRQ trace scenario remains Twister `build_only: true`; hardware failures
  remain failures and are retained in JSON.
- Final runner reset cleared RAM after saving results, leaving the latest
  **921600 diagnostic image** installed. No image restoration is performed.

## Follow-up hardware session: 21 additional trials

At the owner's next request, reran the installed image without rebuilding or
flashing. Full flash readback again matched the SHA-256 above. Source/runner
baseline was `cffe4fc9a4eb14eba868497b1e3e14d3cb0f8672`; board, wiring and
sample interval stayed unchanged. Each case used three reset-isolated trials,
128 blocks maximum and a one-second receive deadline. All 21 boot checks
passed. Local raw log:
`build/test-wch-irq-trace-final/hil-followup-matrix.jsonl`.

| Case at 921600 | Pass / trials | Observed failure |
| --- | ---: | --- |
| USART1, 512-byte blocks | 3 / 3 | None in these bounded runs |
| USART2, 32-byte blocks | 3 / 3 | None in these bounded runs |
| USART2, 64-byte blocks | 0 / 3 | Each second block returned no bytes; MCU counted all 128 RX bytes |
| USART2, 128-byte blocks | 1 / 3 | Two second-block failures lost 5 and 6 bytes, starting at offset 64 |
| USART2, 512-byte blocks | 0 / 3 | Failed blocks lost 2, 1 and 64 bytes |
| Dual, 32-byte blocks | 0 / 3 | USART2 ORE during first block |
| Dual, 512-byte blocks | 0 / 3 | USART2 ORE during first block |

Total: **7 PASS, 14 FAIL, zero boot failures**. Successful single-port runs
transferred 65,536 bytes (USART1/512), 4096 (USART2/32), or 16,384
(USART2/128) each direction. Do not count aborted blocks as completed tests.

All USART2-only failures had matching app/driver byte counters, no latched
API/FIFO hardware errors and no software queue overflow. Software TX counts
still cannot establish that every byte reached the physical wire or bridge.

In all six dual trials, USART2 latched raw ORE `0x08`; the existing fixture
then disabled that port's interrupts. Remaining unprocessed bytes after this
fail-stop are not a measurement of the initial physical loss quantity.
USART1 completed its own blocks after USART2 stopped, so those completions
do not qualify a complete concurrent run.

The first ORE in dual/32 trial 2 was again observed in `fifo_read()`:
`first_error_status=0xE8`, `first_error_rx=12`, `first_error_tx=73`
(TX includes the banner). Later API and FIFO latches both contained ORE.
All five other dual cases first observed it at the entry error check.
This repeats the entry-only observation gap in the instrumented fixture.

Maximum sampled callback/service windows remained nominally 4.042/1.556 us;
no sampled callback reached 8 us. This does not bound interrupt pending time
or exclude missed unsampled delays. The known observer effect still prevents
attributing the original non-trace failures solely to the captured ORE.

No driver fix or peripheral configuration change was made in this session.
Keep the two symptoms separate: complete software counts with missing host
return bytes, versus MCU receive overrun under concurrent instrumented load.
Next isolation work should separate RX/TX load and reduce diagnostic overhead
before drawing causal conclusions. The final reset left the same **921600
diagnostic image** installed; no restoration, DMA or 3-Mbaud tests occurred.
