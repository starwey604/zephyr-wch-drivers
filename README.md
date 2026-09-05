# Zephyr WCH Drivers

Out-of-tree Zephyr drivers for WCH MCUs, developed independently for Ragtime
products. Initial scope: CH32V203, with CH32V203F8U6 as the first hardware
validation target. This is a downstream project, not an official WCH driver pack.

## Status

This revision provides Zephyr module integration, an opt-in community UART
baseline and test scaffolding. **It does not implement UART DMA or USB UDC yet.**

| Component | Status |
| --- | --- |
| CMake / Kconfig / DTS module registration | Available |
| External polling/interrupt UART baseline | Imported; compile-tested only |
| UART DMA resource/contract preparation | Validation and host arithmetic tests; no transfers |
| UART asynchronous TX/RX using DMA | Planned |
| USBFS UDC with endpoint DMA and bulk IN/OUT | Planned |
| ztest / Twister test structure | UART smoke checks and explicit DMA/USB skips |
| Hardware validation | Pending |

Existing upstream UART/DMA drivers remain the default. `CONFIG_WCH_DRIVERS=y`
alone adds no driver code. To use the external UART baseline, explicitly set
`CONFIG_UART_WCH_USART=n` and `CONFIG_WCH_UART=y`. The two UART drivers must
never own the same DT devices simultaneously. No async capability is advertised.

`CONFIG_WCH_UART_DMA_PREPARE=y` additionally validates USART1/2 DMA resources
when `CONFIG_DMA=y`; it does not enable asynchronous transfers. Current framing
support is explicitly 8N1 without hardware flow control. See the
[round-1 design and audit](docs/uart-dma-design.md) for ownership, locking,
version compatibility and known upstream DMA limitations.

## Repository layout

- `drivers/serial/`: opt-in UART C source, CMake and Kconfig.
- `dts/`: future bindings and SoC additions; existing UART bindings are reused.
- `tests/`: standalone ztest applications, Twister scenarios and host-fixture
  plans; see [test instructions](tests/README.md).
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
- Reuse `hal_wch` register definitions and the upstream general DMA driver.
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
Twister, together with the USB placeholder (three configurations, none run).
See [test instructions](tests/README.md) for commands and hardware-test gaps.

## Community work and provenance

Potential implementation references include
[WCH platform planning](https://github.com/zephyrproject-rtos/zephyr/discussions/110177),
[USBFS PR #94286](https://github.com/zephyrproject-rtos/zephyr/pull/94286), and
[BOJIT's USB branch](https://github.com/BOJIT/zephyr/tree/driver/ch32_usb).
The UART baseline was imported from the pinned BOJIT branch snapshot documented
in [upstream provenance](docs/upstream.md). No USB source has been imported.

Before importing code, record the exact repository, commit and original paths;
retain author/license notices and document local modifications. Keep changes
separable so fixes can later be contributed upstream.

## License

Apache-2.0; see [LICENSE](LICENSE). Imported files must retain their original
attribution and applicable license notices.
