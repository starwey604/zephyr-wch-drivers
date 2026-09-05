# Devicetree extensions

Place new bindings under `bindings/` and explicitly included SoC additions
under `riscv/wch/`. The module registers this directory through `dts_root`.
Do not shadow upstream files by reusing their include paths. Product board
definitions and pin assignments remain in the consuming firmware repository.

`bindings/usb/wch,ch32v203-usbfs.yaml` describes the experimental downstream
USBFS controller, distinct from USBD and from DMA1. Opt-in peripheral nodes
live in `tests/subsys/usb_endpoints/boards/`; no upstream SoC include is shadowed.
See [USB integration and shared-pad safety](../docs/usb-udc.md).
