# Zephyr WCH Drivers

Out-of-tree Zephyr drivers for WCH MCUs, developed independently for Ragtime
products. Initial scope: CH32V203, with CH32V203F8U6 as the first hardware
validation target. This is a downstream project, not an official WCH driver pack.

## Status

This revision provides Zephyr module integration, an opt-in community UART
baseline, experimental UART DMA TX/RX, USBFS UDC and executable test fixtures.
**RX inactivity timeout remains unsupported; UART and USB are awaiting hardware validation.**

| Component | Status |
| --- | --- |
| CMake / Kconfig / DTS module registration | Available |
| External polling/interrupt UART baseline | Dual IRQ echo passed at 115200; repeated 921600 tests lose bytes on both links |
| UART DMA resource/contract preparation | Validation and host arithmetic tests; no transfers |
| External general DMA driver | Fixed allocation, cyclic/error/count fixes; native tests, hardware pending |
| UART asynchronous TX using DMA | Opt-in; TC completion, abort/deadline, native tests |
| UART asynchronous RX using DMA | Opt-in normal-mode buffers/disable/errors; finite timeout unsupported |
| USBFS UDC with endpoint DMA and bulk IN/OUT | Experimental EP0 + EP1 IN/EP2 OUT; hardware gate |
| ztest / Twister test structure | Native state tests; build-only DMA/UART/USB; targeted host USB fixture |
| Hardware validation | First-power/flash/UART1 polling + dual IRQ 115200 subset passed; high-rate/DMA/USB pending |

Existing upstream UART/DMA drivers remain the default. `CONFIG_WCH_DRIVERS=y`
alone adds no driver code. To use the external UART baseline, explicitly set
`CONFIG_UART_WCH_USART=n` and `CONFIG_WCH_UART=y`. The two UART drivers must
never own the same DT devices simultaneously. Async TX requires its separate
opt-in option; build-time async capability does not imply full async RX support.

`CONFIG_WCH_UART_DMA_PREPARE=y` additionally validates USART1/2 DMA resources
when `CONFIG_DMA=y`; it does not enable asynchronous transfers. Current framing
support is explicitly 8N1 without hardware flow control. See the
[round-1 design and audit](docs/uart-dma-design.md) for ownership, locking,
version compatibility and known upstream DMA limitations.

The [DMA prerequisite increment](docs/dma-driver.md) addresses those limitations
inside this module: enable `CONFIG_WCH_DMA=y` with `CONFIG_DMA_WCH=n` and
`CONFIG_DMA=y`. Duplicate DMA drivers are rejected at configuration time.
This is a general DMA replacement, **not** a UART async implementation.

The [TX increment](docs/uart-tx.md) adds `CONFIG_WCH_UART_ASYNC_TX=y` together
with `CONFIG_UART_ASYNC_API=y` on top of that replacement. Disable
`UART_INTERRUPT_DRIVEN` and `UART_WIDE_DATA` in this mode. DONE waits for
USART TC; ABORTED reports DMA-fed bytes, and a draining UART rejects new TX
until TC. Read the deadline, buffer-lifetime and callback-context boundaries
before using this experimental API.

The [RX increment](docs/uart-rx.md) adds `CONFIG_WCH_UART_ASYNC_RX=y` for
`SYS_FOREVER_US` reception, next-buffer handoff and stop/error events. It does
not promise gapless reception. Development now pauses at the
[hardware gates](docs/hardware-bringup.md) before IDLE timeout, high-rate
optimization or FeTec integration. At the product owner's request, USB was
advanced independently to its own hardware gate in the next increment.

The [USBFS increment](docs/usb-udc.md) adds opt-in `CONFIG_WCH_UDC=y`, a
controller binding, internal endpoint-DMA staging, EP0 and a bulk pair.
It includes a vendor echo firmware and an explicitly targeted PyUSB script.
Gello USB initialization releases shared SDI pins only with an explicit opt-in;
read the wiring/recovery instructions before flashing. No USB device has been
enumerated or qualified yet. Board defaults remain unchanged.

## Repository layout

First hardware evidence: [2026-09-10 CH32V203F8U session](docs/hardware-20260910.md).
It includes the Gello startup-clock fix needed after debugger reset, a small
polling fixture and paced CH340 host checks. It does not qualify DMA or USB.
The [IRQ follow-up](docs/hardware-20260910-irq.md) records dual binary echo at
115200, corrected J1 wiring, USART2 burst loss at 921600, and the 3-Mbaud gate.
The [35-trial follow-up](docs/hardware-20260910-921600.md) also found occasional
USART1 loss; neither 921600 link is qualified despite earlier individual passes.
Opt-in [IRQ RAM diagnostics](docs/uart-irq-trace.md) now retain error sites and
sampled callback timing without printing on either tested UART.
The [direction-isolation follow-up](docs/hardware-20260910-uart-direction.md)
finds recoverable 64-byte USART2 echo stalls; 70 direction/batch/duplex checks
passed, but earlier loss/overrun issues and high-rate qualification remain open.

- `drivers/serial/`: opt-in UART C source, CMake and Kconfig.
- `drivers/dma/`: opt-in general DMA replacement using the upstream binding/API.
- `drivers/usb/`: experimental USBFS UDC and CH32V203 clock/pad integration.
- `dts/`: downstream USBFS binding; existing UART bindings are reused.
- `tests/`: standalone ztest applications, USB firmware, Twister scenarios and
  a host runner; see [test instructions](tests/README.md).
- `docs/upstream.md`: pinned import provenance and local-change notes.
- `zephyr/module.yml`: west/Zephyr module discovery.

## Integration

Add a pinned project to the application's west manifest:

```yaml
- name: zephyr-wch-drivers
  url: https://github.com/starwey604/zephyr-wch-drivers
  revision: <full-commit-sha>
  path: modules/drivers/wch
```

The consuming manifest must also include `hal_wch` from Zephyr's manifest
(including it in an import allowlist if one is used). This repository has no
nested manifest and does not choose or replace the application's Zephyr version.

```sh
.venv/bin/west update zephyr-wch-drivers hal_wch
```

Zephyr discovers `zephyr/module.yml` automatically through west. If a build
explicitly sets `ZEPHYR_MODULES`, include this repository in that list as well.
The CMake/Kconfig module name is `wch_drivers`.

In the Ragtime firmware workspace, test opt-in registration with:

```sh
.venv/bin/west build -s zephyr/samples/hello_world -b ragtime_gello \
  -d build/gello-module --pristine always -- \
  -DCONFIG_WCH_DRIVERS=y -DCONFIG_CPP=y -DCONFIG_STD_CPP20=y
```

`CONFIG_CPP=y` and `CONFIG_STD_CPP20=y` are needed by Ragtime's pinned OnePID
module, which adds a C++20 interface target even to this C sample; they are
not requirements of the WCH module itself. If SDK discovery fails, also pass
`-DZEPHYR_SDK_INSTALL_DIR=/path/to/zephyr-sdk-1.0.1`.

The Gello board is supplied by `Ragtime_Firmwares`, not this repository. Its
UART console is disabled by default. Drivers remain opt-in so STM32 products
can include this module without enabling WCH support.

## Development boundaries

- Keep hardware drivers in `drivers/`, DT bindings and SoC additions in `dts/`.
- Keep board wiring, application policy, and protocols in the product repository.
- Expose standard Zephyr UART and USB device APIs to applications.
- When enabling the UART replacement, explicitly disable upstream
  `CONFIG_UART_WCH_USART` and reject configurations enabling both drivers.
- Reuse `hal_wch` register definitions. Keep DMA fixes in the opt-in module
  replacement; do not patch the application's Zephyr checkout during builds.
- Pin Zephyr, HAL and this module in the consuming manifest. Test supported
  combinations before updating; out-of-tree UDC code can depend on changing
  Zephyr internal interfaces.
- Add runtime tests for UART buffering/timeouts/abort and USB control/bulk,
  reset/reconnect and idle behavior as implementations become available.

The initial integration baseline is Zephyr
`577e42ad187825cc30d4b51423c32872eb4cf054` and its `hal_wch` revision
`1713a445d44278e902a00e0fee3a111d7bde0d60`, with Zephyr SDK 1.0.1.

Compile-only integration checks passed in the complete west workspace:
`ragtime_gello` with `samples/hello_world` and `CONFIG_WCH_DRIVERS=y`, and
`ragtime_florid` with Ragtime's `apps/led` and the WCH option disabled. Both
used the C++20 settings above. This verifies module integration, not hardware.

The UART import is additionally compiled in polling and interrupt variants by
Twister, together with DMA hardware tests and the USB echo firmware (build-only).
The DMA, UART TX/RX and USB UDC implementations have executable native_sim tests.
See [test instructions](tests/README.md) for commands and hardware-test gaps.

## Community work and provenance

Potential implementation references include
[WCH platform planning](https://github.com/zephyrproject-rtos/zephyr/discussions/110177),
[USBFS PR #94286](https://github.com/zephyrproject-rtos/zephyr/pull/94286), and
[BOJIT's USB branch](https://github.com/BOJIT/zephyr/tree/driver/ch32_usb).
The UART baseline was imported from the pinned BOJIT branch snapshot documented
in [upstream provenance](docs/upstream.md). The USB register operations are now
adapted with retained attribution and rewritten 4.4 control/transfer handling.

Before importing code, record the exact repository, commit and original paths;
retain author/license notices and document local modifications. Keep changes
separable so fixes can later be contributed upstream.

## License

Apache-2.0; see [LICENSE](LICENSE). Imported files must retain their original
attribution and applicable license notices.
