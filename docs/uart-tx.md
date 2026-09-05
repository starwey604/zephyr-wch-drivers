# UART DMA TX increment

Experimental CH32V203 USART1/2 async **TX only**, using Zephyr's public UART
and DMA APIs. The community import remains the polling/IRQ baseline; this TX
implementation is new downstream code. No Zephyr/HAL checkout is patched.

## Configuration

```ini
CONFIG_SERIAL=y
CONFIG_WCH_DRIVERS=y
CONFIG_UART_WCH_USART=n
CONFIG_WCH_UART=y
CONFIG_DMA=y
CONFIG_DMA_WCH=n
CONFIG_WCH_DMA=y
CONFIG_UART_ASYNC_API=y
CONFIG_WCH_UART_ASYNC_TX=y
CONFIG_UART_INTERRUPT_DRIVEN=n
CONFIG_UART_WIDE_DATA=n
CONFIG_CONSOLE=n
CONFIG_UART_CONSOLE=n
```

TX selects DMA resource validation. Both TX and RX fixed channels are reserved
at device initialization (USART1: 3/4; USART2: 6/5). RX is reserved for the next
increment but does not run. If RX allocation fails, TX is released and device
initialization fails. No channel is acquired until framing, clocks and pins
have passed validation. Channels are owned for the device's lifetime.

The driver now exposes build-time async capability so `UART_ASYNC_API` can be
configured without a Kconfig dependency cycle. The TX implementation still
requires the explicit `WCH_UART_ASYNC_TX` option. A successful callback setup
in that mode means **TX-only support**, not complete async RX support.
CMake rejects mixing IRQ/wide-data APIs with this initial TX mode. Existing
polling/IRQ-only variants remain available without enabling the option.

## Buffer and completion contract

- 8N1, no CTS/RTS, static DT baud rate, byte-wide non-cyclic DMA; length 1..65535.
  Null/empty buffers fail with `-EINVAL`, oversized buffers with `-EMSGSIZE`.
  Buffers must be DMA-accessible and valid until their terminal callback.
- Register a callback first; otherwise TX returns `-EACCES`. One application
  owner should coordinate operations for each port. Do not replace/free callback
  context until all terminal notifications have been received.
- TX owns the buffer only after successful setup. Config/start/deadline setup
  failures return an error with no terminal event, leaving DMA requests gated.
- `DMA_ACTIVE` becomes `WAIT_TC` at DMA completion. `UART_TX_DONE` is emitted
  only after USART TC, including the case where TC preceded a delayed DMA IRQ.
  No end-of-transfer TC flag is cleared while waiting for final completion.
- Exactly one DONE or ABORTED is produced for an accepted transfer. Callbacks
  may start the next TX after DONE. Changing callbacks while a transfer is
  active returns `-EBUSY`; abort while idle returns `-EFAULT`.

## Abort, timeout and trailing bytes

Abort first gates UART DMA requests, stops DMA, and reads the remaining count.
`UART_TX_ABORTED.data.tx.len` is the number of bytes **handed to USART**, not
proof that every stop bit reached the wire. This follows the DMA-progress
accounting used by Zephyr's STM32 driver; zero remaining DMA bytes alone must
not be reported as UART_TX_DONE. USART may still shift its queued tail.
Until that tail reaches TC, a new async TX returns `-EBUSY`, including from an
abort callback. The old buffer can be reused once ABORTED is delivered because
DMA has stopped reading it. The receiver must resynchronize interrupted frames.

If a provider unexpectedly fails status reporting, progress is conservatively
zero (or the known full count after DMA completion); the port waits for TC
before reuse. It does not fabricate exact wire progress. The required WCH DMA
provider normally guarantees valid status/stop for the reserved channel.

This driver additionally honors a **whole-transfer software deadline without
flow control**. Zephyr only requires TX timeout with flow control, so this is
an explicit downstream extension. Timeout is in microseconds; `SYS_FOREVER_US`
disables it and zero uses the next tick. Resolution is rounded up to a kernel
tick and delivery depends on system-workqueue scheduling, not a hardware timer
guarantee. Expiry emits ABORTED even while waiting for USART TC.

Every worker checks the current absolute deadline under the UART lock. Terminal
events disarm that deadline rather than cancelling a possibly executing worker,
so a callback can immediately schedule another TX. A stale wakeup either does
nothing or reschedules to the new deadline; it cannot expire the new TX early.

## Callback and polling context

State/MMIO transitions use a per-port spinlock. UART callbacks run after that
lock is released, with no driver state writes after notification. DMA-originated
notifications still inherit the WCH DMA driver's outer IRQ exclusion; UART TC
callbacks run in an ISR, timeout callbacks on the system workqueue, and explicit
abort callbacks in the caller's context. Keep callbacks short and nonblocking.

Polling RX remains usable because async RX is absent. Polling TX waits while
an async buffer owns the port; async TX waits for an earlier polling byte's TC.
Do not call polling TX from an ISR/critical section or the system workqueue
while async TX is active: blocking there can prevent completion/timeout work.
Use async TX consistently for a product port. No IRQ/async callback switching,
power management, runtime reconfiguration or wide-data support is claimed.

## Tests and next boundary

`tests/unit/uart_tx_native` compiles the production UART source with real Zephyr
API wrappers, spinlocks, workqueue and DMA channel allocation. Only HAL/MMIO,
pinctrl and the fault-injectable DMA provider are modeled. It covers setup
rollback, ownership, two ports, TC ordering, abort/error progress, callback
reentry, actual timeout execution, stale work and explicit unsupported RX.
These are software tests, not a virtual UART or hardware qualification.

The `wch.uart_dma.tx` hardware scenario sends deterministic 1- and 128-byte
buffers on both ports, with bounded waits and event/buffer/count assertions.
It remains **build-only**. Capture TX externally to validate data/baud and
last-stop-bit timing; the event test alone does not verify transmitted bytes.
No hardware has been flashed. RX loopback and USB remain explicit skips.

Next increment: non-cyclic async RX, buffer handoff/release, idle/timeout
accounting and receive errors. Hardware TX/tail/timeout qualification is still
required before relying on this driver in a product.

References: [v4.4.0 UART API](https://github.com/zephyrproject-rtos/zephyr/blob/v4.4.0/include/zephyr/drivers/uart.h),
[pinned STM32 abort accounting](https://github.com/zephyrproject-rtos/zephyr/blob/577e42ad187825cc30d4b51423c32872eb4cf054/drivers/serial/uart_stm32.c),
[WCH USART register operations](https://github.com/openwch/ch32v20x/blob/main/EVT/EXAM/SRC/Peripheral/src/ch32v20x_usart.c).
The WCH vendor implementation was consulted, not imported.
