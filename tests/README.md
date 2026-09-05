# Driver test bring-up

These are Zephyr ztest applications and Twister build scenarios for future
hardware qualification. **This revision verifies compilation only.** Nothing
is flashed automatically, and no UART DMA or USB transfer has been validated.

## Layout and current coverage

| Directory | Current behavior | Next hardware checks |
| --- | --- | --- |
| `drivers/uart_dma` | Device readiness and four distinct DMA request mappings; DMA transfer case skips | Dual-UART async TX/RX, buffers, timeout, abort, overrun |
| `subsys/usb_endpoints` | EP0 and bulk cases explicitly skip; no USB stack/UDC | Enumeration, bulk IN/OUT, endpoint DMA, reset/reconnect |
| `host` | Host-fixture requirements only | UART traffic generator and USB bulk exerciser |
| `configs` | Optional Ragtime C++20 workspace compatibility fragment | Keep product-only dependencies out of test defaults |

`tests.yaml` registers UART interrupt/polling variants and the USB placeholder.
All are `build_only: true`; skipped cases are not successful transfer tests.
The `test-uart0`/`test-uart1` aliases come from the Gello test overlay. The UART
driver does not consume the DMA mappings yet. A readiness check is not proof
of correct pin routing, request-channel selection, IRQ delivery or baud rate.

## Build in the CH32 workspace

Run from `~/codings/Ragtime_Firmwares-ch32` after updating the manifest pin:

```sh
.venv/bin/west update zephyr-wch-drivers
.venv/bin/west build -s modules/drivers/wch/tests/drivers/uart_dma \
  -b ragtime_gello -d build/test-wch-uart-irq --pristine always -- \
  -DEXTRA_CONF_FILE=$PWD/modules/drivers/wch/tests/configs/ragtime-cpp20.conf
.venv/bin/west build -s modules/drivers/wch/tests/subsys/usb_endpoints \
  -b ragtime_gello -d build/test-wch-usb --pristine always -- \
  -DEXTRA_CONF_FILE=$PWD/modules/drivers/wch/tests/configs/ragtime-cpp20.conf
```

For polling-only coverage, build the UART application in a separate directory
with the additional CMake argument `-DCONFIG_UART_INTERRUPT_DRIVEN=n`.
If necessary, add `-DZEPHYR_SDK_INSTALL_DIR=/path/to/zephyr-sdk-1.0.1`.

For uncommitted development, replace the source and configuration paths with
those in `../zephyr-wch-drivers` and also pass
`-DEXTRA_ZEPHYR_MODULES=$PWD/../zephyr-wch-drivers`. This overrides the discovered
module of the same name without editing the west-managed copy. Verify the
source path in the build output before testing.

An empty upstream `drivers__serial` library warning is expected when its only
driver is disabled in favor of the external UART. Check that the external
`uart_wch_usart.c` is compiled and `CONFIG_WCH_UART=y` is in `zephyr/.config`.
Selecting both UART drivers is a configuration error, not an alternate test
mode: CMake must stop before compiling duplicate device instances.

To compile all scenarios via Twister:

```sh
.venv/bin/west twister -T modules/drivers/wch/tests \
  --board-root firmware -p ragtime_gello/ch32v203 --build-only \
  --outdir build/test-wch-twister \
  -x EXTRA_CONF_FILE=$PWD/modules/drivers/wch/tests/configs/ragtime-cpp20.conf
```

## Hardware procedure to complete later

1. Keep Gello's console disabled: both UARTs belong to the test fixture. No
   serial ztest report is currently available; inspect ztest state with a
   debugger. Add an independent reporting transport before automated HIL runs.
2. UART fixture: 3.3 V logic and common ground; never use RS-232 levels. For
   future single-port loopback join PA9 to PA10 and PA2 to PA3. Do not connect
   two TX outputs together. Establish low-speed operation before 3 Mbaud and
   simultaneous two-port load. Current tests do not send traffic.
3. USB fixture: data-capable cable and host test process. Gello USB shares pins
   with SDI; disconnect the debugger from the USB pair before enumeration and
   retain NRST access for recovery. Do not configure USB metadata as GPIOs.
4. Replace placeholder skips with real assertions only as drivers arrive. Add
   fixture metadata, bounded waits and reliable result capture before removing
   `build_only`. Exercise USB packet boundaries and reconnects independently.
5. Record board revision, wiring, Zephyr/HAL/module SHAs, firmware config,
   host command and captured results. Build logs alone are not HIL evidence.
