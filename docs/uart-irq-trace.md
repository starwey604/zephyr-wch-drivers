# UART IRQ RAM diagnostics

`CONFIG_WCH_UART_IRQ_TRACE=y` enables an opt-in CH32V203 IRQ-only flight
recorder. It is disabled by default and incompatible with async UART/SMP.
No printf, Zephyr logger backend, UART output, allocation, locks, or additional
STATR/DATAR reads are added. Diagnostic overhead still changes execution time;
this is not a throughput qualification configuration.

## Recorded information

The debugger-visible `wch_uart_irq_trace` contains a versioned header and
per-device records (identified by USART register address, not DT ordering):

- IRQ and FIFO RX/TX byte counts, including the fixture's boot banner.
- Separate latched raw hardware error masks for `err_check()` and
  `fifo_read()`. The latter retains the existing STATR read **before** DATAR,
  but writes the RAM log **after** DATAR, without adding another status read.
- Sampled driver-callback duration and entry-to-RX-read service duration,
  plus an 8-us default slow threshold and a nominal one-character threshold.
- A 32-entry event ring per device with raw timer, event kind, status,
  IRQ/byte counts and duration. First observed error freezes the ring and
  records its site/status/counts; counters continue. Error counts mean status
  observations, not unique lost bytes. Normal UART error handling is unchanged.

Event kinds: 1 sampled callback exit, 2 slow sampled exit, 3 error in
`err_check`, 4 error in `fifo_read`. Hardware ORE is raw mask `0x08`;
the fixture's Zephyr `UART_ERROR_OVERRUN` mask is `0x01`.

Default `CONFIG_WCH_UART_IRQ_TRACE_SAMPLE_INTERVAL=127` times one in 127
callbacks, beginning with the first. Odd spacing avoids always sampling TX
in alternating RX/TX IRQs. All callbacks still count bytes and capture errors.
An interval of 1 enables intrusive full timing; normal events are then logged
every 128 callbacks. `CONFIG_WCH_UART_IRQ_TRACE_SLOW_US` defaults to 8.

## Timing limitations

Timestamps are raw SysTick CNT cycles, modulo CMP+1 (nominal 1,440,001 cycles
on this 144-MHz/100-Hz-tick fixture), not wall-clock timestamps. Short-duration
subtraction handles one wrap; it cannot detect pauses spanning whole periods.
Do not break/halt during traffic. Converted microseconds assume the nominal
clock, not an externally measured frequency.

The measured window excludes architecture interrupt entry/exit and final
trace epilogue. It does **not** measure RX arrival-to-handler latency, time
waiting behind another IRQ, or all interrupt-disabled windows. Maxima cover
sampled callbacks only. A zero duration with no relevant samples is not zero
latency. Unsampled error event duration is `UINT32_MAX` (unknown).

Calls to FIFO APIs/`err_check()` must remain owned by the fixture's UART ISR
while tracing. Read/decode only after all traffic stops. Debug attachment can
perturb clocks; reset before any new wire test. Never equate zero sampled
errors or short callback durations with proof of a clean physical link.

## Build and capture

Use the [IRQ fixture build](../tests/drivers/uart_irq/README.md), adding the
921600 overlay and this fragment in a separate build directory:

```sh
-DEXTRA_DTC_OVERLAY_FILE=<absolute fixture path>/boards/921600.overlay
-DEXTRA_CONF_FILE=<absolute fixture path>/trace.conf
```

After explicitly flashing the intended board, resolve `wch_irq_diag` and
`wch_uart_irq_trace` with `nm -S` on the matching ELF. Then run, from Ragtime:

```sh
.venv/bin/python ../zephyr-wch-drivers/tests/host/uart_irq_repeat.py \
  --mode dual --size 512 --trials 3 \
  --uart1 /dev/serial/by-id/<CH340> --uart2 /dev/serial/by-id/<WCH-Link-CDC> \
  --diag-address <ELF-address> --trace-address <ELF-address> \
  --reset-tool "$PWD/build/wch-tools/bin/wlink"
```

The runner reads the RAM log after both workers finish, emits decoded events
in JSON and marks observed trace errors/count mismatches as failures. It
resets after final capture, but **does not restore/reflash any image**. Per the
board owner's request, leave the latest diagnostic image installed unless
explicitly asked to change it. Reset clears the RAM log; retain the host JSON.

For an already saved raw RAM dump, decode offline with
`python tests/host/uart_irq_trace.py <dump.bin>`. Version 1 layout is eight
header words, then per port 25 statistic words and 32 eight-word events.
The header supplies port count/record size; the decoder checks them.

## Verification

`tests/unit/uart_irq_trace` tests wrap arithmetic, ring overwrite, first-error
freeze, separate error sites, port isolation and odd-interval timing samples.
Run CMake/build/CTest on the host; these do not model UART/PFIC hardware.
The trace-disabled 921600 build remains byte-identical to the previous binary;
the DMA TX/RX fixture also rebuilds without tracing. See the
[hardware report](hardware-20260910-irq-trace.md) for observer effects and actual run results.
