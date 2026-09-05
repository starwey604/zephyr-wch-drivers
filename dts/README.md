# Devicetree extensions

Place new bindings under `bindings/` and explicitly included SoC additions
under `riscv/wch/`. The module registers this directory through `dts_root`.
Do not shadow upstream files by reusing their include paths. Product board
definitions and pin assignments remain in the consuming firmware repository.

No bindings or peripheral nodes are provided by this initial scaffold.
