# Upstream provenance

## UART baseline

- Repository: https://github.com/BOJIT/zephyr
- Branch inspected: `driver/ch32_usb`
- Exact commit: `dc53b3104fbbe6db5d35d3276d281ffdc3da6483`
- Original path: `drivers/serial/uart_wch_usart.c`
- [Pinned source](https://github.com/BOJIT/zephyr/blob/dc53b3104fbbe6db5d35d3276d281ffdc3da6483/drivers/serial/uart_wch_usart.c)
- Local path: `drivers/serial/uart_wch_usart.c`
- License: Apache-2.0; original Google LLC copyright retained in the file.

The inspected branch contains a polling/interrupt UART driver, not a UART
async/DMA implementation. The branch name describes its USB work; it does not
imply UART DMA support. This import is a starting point, not a completed port.

Initial local changes: provenance comment and separate module CMake/Kconfig
registration using `CONFIG_WCH_UART`. Round 1 adds clock/readiness/BRR checks,
explicit 8N1-only framing validation, configuration-before-enable ordering,
and opt-in DMA DT/readiness checks. See [the audit](uart-dma-design.md).
Do not enable upstream `CONFIG_UART_WCH_USART` at the same time: both use
`wch,usart` and instantiate the same devices. No custom binding is needed yet.

The consuming baseline remains Zephyr
`577e42ad187825cc30d4b51423c32872eb4cf054` and `hal_wch`
`1713a445d44278e902a00e0fee3a111d7bde0d60`. The community snapshot predates that
baseline; compare subsequent upstream fixes before hardware qualification.

## Compatibility fixture

The DMA1 node in `tests/drivers/uart_dma/boards/bluepillplus_ch32v203.overlay`
is adapted from Zephyr's `dts/riscv/wch/common/ch32v20x_30x_common.dtsi` at
`577e42ad187825cc30d4b51423c32872eb4cf054`. Original James Bennion-Pedley
copyright and Apache-2.0 notice are retained. The local label is `test_dma1`,
status is explicit, and the overlay supplies USART1/2 test wiring. It fills
a missing release DT declaration for compile testing; no SoC/core files are
patched and no additional board hardware is qualified by this fixture.

## DMA baseline

- Repository: https://github.com/zephyrproject-rtos/zephyr
- Exact commit: `577e42ad187825cc30d4b51423c32872eb4cf054`
- Original and local path: `drivers/dma/dma_wch.c`
- License: Apache-2.0; Paul Wedeck's original copyright retained.
- The initial import is byte-for-byte identical and deliberately not enabled.
  Follow-up changes provide opt-in registration and CH32V203 fixes independently
  of the Zephyr checkout. No SoC or core patches are applied during builds.

## USB

No USB implementation is imported in this revision. The community branch's
`drivers/usb/udc/udc_wch.c` remains a reference for a later, separately reviewed
port. Endpoint DMA belongs to USBFS, not the general DMA1 controller.
