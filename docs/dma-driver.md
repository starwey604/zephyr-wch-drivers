# Out-of-tree DMA prerequisite

This increment replaces only the WCH DMA driver. It does not fork Zephyr,
patch a west-managed checkout, implement UART async, or enable USBFS.
The original import and downstream changes are separate commits;
see [provenance](upstream.md).

## Enable explicitly

```ini
CONFIG_DMA=y
CONFIG_WCH_DRIVERS=y
CONFIG_DMA_WCH=n
CONFIG_WCH_DMA=y
```

The existing `wch,wch-dma` binding and standard Zephyr DMA API are retained.
CMake rejects simultaneous upstream/external DMA drivers. The module remains
CH32V203-only and opt-in; including it does not change STM32 drivers.
UART preparation tests now use the replacement; board defaults are unchanged.

## Ownership contract

Pass a pointer to a **zero-based `uint32_t` index** as the filter parameter:

```c
uint32_t requested = DT_DMAS_CELL_BY_NAME(DT_NODELABEL(usart1), tx, channel);
int channel = dma_request_channel(controller, &requested);
if (channel < 0) {
    return channel;
}
/* Configure/start; disable peripheral requests before stopping DMA. */
dma_release_channel(controller, channel);
```

USART1 TX/RX uses indices 3/4; USART2 uses 6/5. `NULL` accepts any free channel.
Invalid/occupied fixed requests return `-EINVAL` through Zephyr's allocator.
The filter pointer is consumed synchronously, not retained. The standard
context bitmap owns allocation; UART code must not access it directly.
`dma_config()` does not prove ownership: all clients must respect the same
single-owner convention. Release disables/clears the channel and forgets its
configuration/callback; the next owner must configure it again.

## Transfer and event contract

- Single block; memory-to-memory, memory-to-peripheral, peripheral-to-memory.
  Cyclic is supported for peripheral transfers, not memory copies.
- Equal source/destination widths of 1, 2 or 4 bytes; addresses and lengths
  must align to width. CNTR accepts 1..65535 **elements**. Config/reload and
  `pending_length` use **bytes**. UART byte buffers still max at 65535.
- No scatter/gather, chaining, packing, decrement, software handshake, reload
  flags or slot mux. Burst lengths may be zero (unspecified) or one element.
  Unsupported modes and addresses exceeding 32 bits are explicitly rejected.
- Normal TC disables DMA; cyclic TC keeps running. Busy includes suspended
  state and a terminal IRQ awaiting service. Config/reload while busy fails.
  Start/resume while running is idempotent. Use reload/config for a new buffer.
- `half_complete_callback_en` produces `DMA_STATUS_HALF_COMPLETE`; TC produces
  `DMA_STATUS_COMPLETE`. `complete_callback_en` does not request half interrupts.
  Coalesced HT+TC reports TC only, not a stale half event after callback reentry.
- TE dominates simultaneous TC/HT and disables DMA. Cleanup occurs even when
  error callbacks are suppressed or no callback exists.
- Stop clears pending peripheral flags without notification. Config/reload/
  start also clear stale flags. Release additionally forgets configuration.
- Cyclic read/write positions are byte offsets for incrementing endpoints.
  `free` and `total_copied` are unsupported and zero. Offsets do not count wraps;
  software cannot reconstruct arbitrarily many missed hardware events.

CH32V203 is single-core. State/MMIO transitions and DMA callbacks run under
IRQ exclusion, preventing stop/release racing with a saved callback. Callbacks
may reenter DMA APIs but must not block or perform lengthy work. The ISR makes
no state changes after invoking user code. A future UART layer must account
for this callback context and avoid lock recursion.

## Verification boundaries

`tests/unit/dma_native` compiles the **production driver** into native_sim,
using actual Zephyr DMA allocation/APIs and a minimal fake HAL/register bank.
It tests allocation, validation, normal/cyclic IRQs, both register banks,
error precedence, stop/restart, reentry, byte progress and clock failures.
The fake bank explicitly models flag clearing. It does not move memory,
simulate bus arbitration/nested PFIC timing, or prove electrical behavior.

`tests/drivers/dma` adds a bounded hardware memory-copy test at 8/16/32-bit
widths, checking completion, remaining count and bytes. No UART wiring is
needed; it remains **build-only and has not run on hardware**. UART/USB cannot
yet report ztest results; use a debugger until independent reporting is added.

Qualification scope is CH32V203 DMA1 (seven channels). The inherited extended
bank is software-tested, not a claim of support for other series/DMA2.
Still required: real copies/IRQ delivery, suspend/resume position retention,
UART request gating, cyclic wrap/overrun stress, dual-port load and recovery.

The [TX increment](uart-tx.md) now reserves USART1/2 channels and implements TX,
abort/deadline handling, with DONE at **USART TC**, not DMA TC. RX follows separately.

References: [Zephyr DMA expectations](https://docs.zephyrproject.org/latest/hardware/peripherals/dma.html),
[v4.4.0 API](https://github.com/zephyrproject-rtos/zephyr/blob/v4.4.0/include/zephyr/drivers/dma.h).
