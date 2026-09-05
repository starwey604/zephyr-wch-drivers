# UART DMA RX increment

Experimental CH32V203 USART1/2 byte-wide, normal-mode DMA reception. This is
new downstream code, not a community async-driver import. No Zephyr/HAL files
are patched. **Hardware qualification is pending; stop here before product use.**

## Configuration and deliberately limited scope

Add `CONFIG_WCH_UART_ASYNC_RX=y` to the [TX configuration](uart-tx.md).
It depends on `WCH_UART_ASYNC_TX`; both directions share the callback and port
lock, but have independent transfer state and fixed, lifetime-owned channels.
Polling/IRQ and TX-only configurations remain available unchanged.

- RX supports 8N1, buffers of 1..65535 DMA-accessible bytes, and
  `SYS_FOREVER_US` **only**. Every finite timeout, including zero, returns
  `-ENOTSUP` without borrowing the buffer; values below -1 return `-EINVAL`.
- No idle-time partial reports yet. A full buffer, explicit disable or error
  produces `UART_RX_RDY`. Applications needing finite inactivity timeout
  (including current FeTec usage) must not adopt this RX implementation yet.
- Normal-mode DMA is stopped/reconfigured between buffers. Rearming precedes
  callbacks, but a software gap remains. Two queued buffers do **not** establish
  gapless or dual-3-Mbaud reception. No cyclic ring or overflow recovery is claimed.

## Buffer ownership and events

Register a callback before enabling RX (`-EACCES` otherwise). Successful enable
borrows the buffer and issues `UART_RX_BUF_REQUEST`, potentially before the API
returns. Config/start failures return an error without borrowing or releasing it.
Provide one distinct, nonoverlapping next buffer in response to the request;
it remains borrowed until `UART_RX_BUF_RELEASED`. A duplicate/premature response
returns `-EBUSY`, overlap with the current buffer `-EINVAL`, and a response after
stop `-EACCES`. Do not reuse buffers from RDY; wait for RELEASED.

A full buffer produces RDY (offset zero, full length), RELEASED, then a request
for the newly started buffer; without a next buffer, DISABLED follows instead.
Explicit disable gates DMAR, stops DMA, samples the stable committed byte count,
reports any data, releases current/unused-next buffers and emits DISABLED last.
No zero-length RDY is generated. Idle disable returns `-EFAULT`.

UART parity/framing/noise/overrun errors produce STOPPED before pending RDY,
all releases and DISABLED. Errors beat coincident DMA completion. DMA failures
and invalid/unavailable progress are conservatively mapped to
`UART_ERROR_OVERRUN` (Zephyr has no generic DMA-error reason); unknown counts
are zero, never fabricated. Only bytes committed by DMA count as received.

## Reentrancy, interrupts and receive flags

State/MMIO transitions hold the UART lock; a bounded, serialized event drain
invokes callbacks outside it. A request opens only when delivered, permitting
one response and bounding outstanding loans/events. Stale requests are suppressed
when their buffer generation has ended. DISABLED makes RX idle immediately before
its callback, so restarting there cannot overtake old buffer releases. Callbacks
may supply buffers, disable reception or restart on DISABLED. Do not block them.
DMA-originated callbacks still inherit the DMA driver's outer IRQ exclusion.

Callback replacement while RX is active/stopping returns `-EBUSY`.
`uart_poll_in()` also returns `-EBUSY` until DISABLED; it cannot consume DMA data.
One application owner must coordinate each port and callback-context lifetime.
The DMA provider clears old channel flags on stop/config/start and delivers
callbacks synchronously under IRQ exclusion; it must not defer old callbacks
across reuse. Native late-IRQ tests do not prove silicon flag behavior.

Receiver flags are cleared only at session boundaries, with DMA stopped, DMAR
gated and RE temporarily disabled, by reading STATR then DATAR. This discards
stale/out-of-session bytes; the sender must remain quiet until enable completes.
No DATAR read occurs across an in-session buffer handoff. We intentionally do
not enable IDLE interrupts: live flag clearing could consume a byte intended for
DMA. Validate that race, RE behavior and stop-count stability on hardware before
designing the finite-timeout path.

## Verification and next gate

Native tests compile the complete production UART with real Zephyr APIs and a
fault-injectable DMA/MMIO fixture. The RX-enabled scenario runs the 20 TX tests
plus 15 RX tests: ownership, boundary lengths, rollback, partial stop, two-buffer
handoff, errors, callback reentry/interleaved completions and dual-port isolation.
They do not move bytes or emulate UART electrical/register-read side effects.

`wch.uart_dma.rx_loopback` adds an actual paced local-loopback test application:
full 1-/32-byte reception, partial 7-byte stop and 32+7-byte buffer handoff on
both ports, with byte comparisons and bounded waits. It remains **build-only**.
The continuous-RX/IDLE case explicitly skips; USB still has no UDC.

Continue with the [hardware gates](hardware-bringup.md), jointly with the board
owner when hardware arrives. Do not advance into IDLE timeout, sustained traffic
optimization, FeTec integration or USB until the prerequisite measurements pass.

References: [Zephyr v4.4.0 UART contract](https://github.com/zephyrproject-rtos/zephyr/blob/v4.4.0/include/zephyr/drivers/uart.h),
[WCH flag-clear documentation](https://github.com/openwch/ch32v20x/blob/main/EVT/EXAM/SRC/Peripheral/src/ch32v20x_usart.c).
The vendor source was consulted, not copied.
