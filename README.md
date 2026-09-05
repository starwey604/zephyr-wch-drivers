# Zephyr WCH Drivers

Out-of-tree Zephyr drivers for WCH MCUs, developed independently for Ragtime
products. Initial scope: CH32V203, with CH32V203F8U6 as the first hardware
validation target. This is a downstream project, not an official WCH driver pack.

## Status

This first revision provides the Zephyr module scaffolding and west integration
only. **It does not implement UART DMA or USB UDC yet.**

| Component | Status |
| --- | --- |
| CMake / Kconfig / DTS module registration | Available |
| UART asynchronous TX/RX using DMA | Planned |
| USBFS UDC with endpoint DMA and bulk IN/OUT | Planned |
| Hardware validation | Pending |

Existing upstream polling/interrupt UART and DMA drivers are not replaced by
this scaffold. Enabling `CONFIG_WCH_DRIVERS=y` currently adds no driver code.

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
  -d build/gello-module --pristine always -- -DCONFIG_WCH_DRIVERS=y
```

The Gello board is supplied by `Ragtime_Firmwares`, not this repository. Its
UART console is disabled by default. Drivers remain opt-in so STM32 products
can include this module without enabling WCH support.

## Development boundaries

- Keep hardware drivers in `drivers/`, DT bindings and SoC additions in `dts/`.
- Keep board wiring, application policy, and protocols in the product repository.
- Expose standard Zephyr UART and USB device APIs to applications.
- When implementing the UART replacement, explicitly disable upstream
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

## Community work and provenance

Potential implementation references include
[WCH platform planning](https://github.com/zephyrproject-rtos/zephyr/discussions/110177),
[USBFS PR #94286](https://github.com/zephyrproject-rtos/zephyr/pull/94286), and
[BOJIT's USB branch](https://github.com/BOJIT/zephyr/tree/driver/ch32_usb).
No driver source has been imported from these projects in this initial revision.

Before importing code, record the exact repository, commit and original paths;
retain author/license notices and document local modifications. Keep changes
separable so fixes can later be contributed upstream.

## License

Apache-2.0; see [LICENSE](LICENSE). Imported files must retain their original
attribution and applicable license notices.
