# Driver test bring-up

These are Zephyr/Twister applications, host unit checks and targeted hardware
runners. **Host unit checks exercise helpers or modeled registers, not hardware.**
Selected UART IRQ/polling hardware runs are documented below. Nothing is flashed
automatically; no UART DMA or USB transfer is validated.

## Layout and current coverage

| Directory | Current behavior | Next hardware checks |
| --- | --- | --- |
| `drivers/uart_polling` | UART1/CH340 heartbeat and paced ASCII echo, first hardware run passed | Baud measurement, USART2; see dated hardware report |
| `drivers/uart_direction` | Bounded IRQ RX/TX, batch/duplex and immediate echo at 921600 | Capture USB return stalls; not high-rate qualification |
| `drivers/uart_dma` | Readiness/ownership, optional TX and paced local RX loopback | Capture dual-UART bytes/baud, tail/abort, RX timing |
| `subsys/usb_endpoints` | Real vendor EP0/bulk echo firmware; build-only | Enumeration, DMA bytes/timing, reset/reconnect |
| `host` | Targeted PyUSB runner + 4 executable host-script tests | Execute USB checks on the DUT; UART host generator pending |
| `configs` | Optional Ragtime C++20 workspace compatibility fragment | Keep product-only dependencies out of test defaults |
| `unit/uart_contract` | Host CTest: BRR arithmetic, DMA lengths, RX progress | Extend with state/IRQ tests as implementation arrives |
| `unit/dma_native` | Production DMA driver + real Zephyr API, fake registers; 12 executed tests | Real PFIC/bus timing, request gating |
| `unit/udc_native` | Production UDC/common API, fake MMIO; 18 executed tests | Real USB SIE/DMA/clock/pad behavior |
| `unit/uart_tx_native` | Shared production UART fixture; TX-only 20, RX-enabled 35 tests | Actual DMA requests, wire timing and stress |
| `drivers/dma` | Bounded 8/16/32-bit memory-copy test, currently compile-only | Run on DMA1 with debugger result capture |
| `check_build_guards.py` | Twelve negative builds requiring specific diagnostics | UART/DMA config plus USB SDI/clock/address guards |

`tests.yaml` registers UART interrupt/polling/TX/RX variants and USB bulk firmware.
Hardware applications are `build_only: true`; native_sim is executable.
The USB firmware is not a ztest app: the host performs its assertions. See
[USB build, host commands and safety gates](../docs/usb-udc.md) before flashing.
Skipped cases are not successful transfer tests.
The `test-uart0`/`test-uart1` aliases come from the Gello test overlay. The UART
driver uses DMA only in the explicit asynchronous TX/RX variants. A readiness check is not proof
of correct pin routing, request-channel selection, IRQ delivery or baud rate.

## Build in the Ragtime dev workspace

For binary interrupt TX/RX on both USARTs, use the separate
[`drivers/uart_irq`](drivers/uart_irq/README.md) fixture and targeted
`host/uart_irq.py` runner. Bounded dual-port IRQ echo passed at 115200 after
correcting J1 TX/RX wiring. USART1 passed at 921600, but USART2 burst return
bytes were lost; high-rate diagnosis and dual 3-Mbaud-capable bridges remain pending.
The [35-trial follow-up](../docs/hardware-20260910-921600.md), using
`host/uart_irq_repeat.py`, also found USART1 loss. Earlier individual passes
must not be treated as 921600 qualification.
Optional `--trace-address` on that runner decodes the diagnostic driver's
RAM records after traffic; see [IRQ tracing](../docs/uart-irq-trace.md).
The [direction-isolation campaign](../docs/hardware-20260910-uart-direction.md)
then recovered missing 64-byte USART2 echoes by sending one extra byte. Use
[`drivers/uart_direction`](drivers/uart_direction/README.md) and
`host/uart_direction.py` to distinguish incomplete delivery from byte corruption.
Earlier failures are retained; not all loss cases are explained.

For first-power testing with an onboard CH340, see
[`drivers/uart_polling`](drivers/uart_polling/README.md) and the
[first hardware report](../docs/hardware-20260910.md). The fixture keeps SDI,
uses only USART1 and disables DMA/USB. Its host runner is separate from the
USB host tests and needs pyserial, not PyUSB.

Run from `~/codings/Ragtime_Firmwares` (`dev/wirelink-p0-hardening`) after
updating the manifest pin. The old `ch32` worktree is no longer the dev target:

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

For experimental TX (no async RX), add all three arguments in a new build:
`-DCONFIG_UART_INTERRUPT_DRIVEN=n -DCONFIG_UART_ASYNC_API=y
-DCONFIG_WCH_UART_ASYNC_TX=y`. Or select Twister scenario `wch.uart_dma.tx`.
It sends deterministic 1-/128-byte buffers on both ports and checks terminal
events. This remains compile-only until hardware and external capture are ready.
See [TX constraints](../docs/uart-tx.md), especially abort counts and deadlines.

For RX add `-DCONFIG_WCH_UART_ASYNC_RX=y` as well, or select scenario
`wch.uart_dma.rx_loopback`. It tests full/partial buffers and a deliberately
paced handoff using physical TX->RX jumpers on both ports. **No hardware run
has occurred.** Start with the low-baud overlay and ordered
[hardware gates](../docs/hardware-bringup.md); stop before IDLE/continuous-load
implementation until those measurements are available. Only `SYS_FOREVER_US`
is supported; finite timeouts return `-ENOTSUP`.

For uncommitted development, replace the source and configuration paths with
those in `../zephyr-wch-drivers` and also pass
`-DEXTRA_ZEPHYR_MODULES=$PWD/../zephyr-wch-drivers`. This overrides the discovered
module of the same name without editing the west-managed copy. Verify the
source path in the build output before testing.

An empty upstream `drivers__serial` library warning is expected when its only
driver is disabled in favor of the external UART. Check that the external
`uart_wch_usart.c` is compiled and `CONFIG_WCH_UART=y` is in `zephyr/.config`.
Selecting both UART drivers is a configuration error, not an alternate test
mode: CMake must stop before compiling duplicate device instances. The same
rule applies to `WCH_DMA` versus `DMA_WCH`; DMA and UART tests enable the
external DMA replacement explicitly.

To compile all scenarios via Twister:

```sh
.venv/bin/west twister -T modules/drivers/wch/tests \
  --board-root firmware -p ragtime_gello/ch32v203 --build-only \
  --outdir build/test-wch-twister \
  -x EXTRA_CONF_FILE=$PWD/modules/drivers/wch/tests/configs/ragtime-cpp20.conf
```

## Executable host tests and negative builds

From the Ragtime workspace (substitute `../zephyr-wch-drivers` while developing):

```sh
cmake -S modules/drivers/wch/tests/unit/uart_contract \
  -B build/test-wch-contract -DCMAKE_BUILD_TYPE=Release
cmake --build build/test-wch-contract
ctest --test-dir build/test-wch-contract --output-on-failure
.venv/bin/python modules/drivers/wch/tests/check_build_guards.py \
  --workspace "$PWD" --sdk /home/ww/zephyr-sdk-1.0.1
```

The three CTest suites test the same BRR helper used in driver initialization,
DMA length limits, and future non-cyclic RX progress arithmetic. They do not
simulate registers, IRQ delivery, callbacks or buffer ownership. Their checks
remain enabled with `NDEBUG`. The negative runner checks twelve specific errors:
duplicate UART/DMA drivers, init order, channel range, wrong request, missing RX, and
disabled DMA, plus IRQ/async and wide-data/async conflicts. An unrelated build
failure is a test failure, not a pass. USB adds missing SDI-release opt-in,
wrong USB clock and accidental use of the USBD address instead of USBFS.

Run the production DMA driver against the native register model:

```sh
.venv/bin/west twister -T modules/drivers/wch/tests/unit/dma_native \
  -p native_sim/native/64 --outdir build/test-wch-dma-native-twister \
  -x EXTRA_CONF_FILE=$PWD/modules/drivers/wch/tests/configs/ragtime-cpp20.conf
```

This uses the real Zephyr allocator and API, not hand-written API stubs. The
minimal fake HAL is private to the test target. It explicitly emulates flag
clearing and injects count/IRQ changes; there is no simulated data transfer.

For UART native tests use the same command with
`-T modules/drivers/wch/tests/unit/uart_tx_native` and a separate output directory.
This compiles the entire production UART C file against a fault-injectable DMA
provider, modeled UART registers and pinctrl. Zephyr APIs, spinlocks, allocation
and workqueue are real. Test-only compile definitions enable the UART TX source
branch on native_sim without enabling the actual CH32V203 SoC module globally.
Scenarios `wch.uart_tx.native` and `wch.uart_rx.native` reuse this fixture;
the latter sets test-only CMake `WCH_TEST_RX=ON` and adds 15 RX cases. Normal
board builds must use Kconfig `WCH_UART_ASYNC_RX`, not that test selector.

To compile the hardware DMA memory-copy test, use the same west build options
as above with `-s modules/drivers/wch/tests/drivers/dma` and
`-d build/test-wch-dma-hardware`. It requires no loopback cable. It waits at most
one second per width, checks copied bytes/count and quiesces DMA on failures.
Do not remove `build_only` until real hardware execution/reporting is available.

## Release compatibility check

Use an **independent** west workspace whose manifest repository is Zephyr
v4.4.0 (`684c9e8f...`), and its matching `hal_wch` (`dd3855ea...`). Do not
switch the active Ragtime Zephyr checkout. Point `ZEPHYR_MODULES` at just that
HAL and this module; no Ragtime/OnePID/C++ fragment is needed for BluePill:

```sh
/path/to/Ragtime_Firmwares/.venv/bin/west build \
  -s /path/to/zephyr-wch-drivers/tests/drivers/uart_dma \
  -b bluepillplus_ch32v203 -d build/test-wch-v440-irq --pristine always -- \
  '-DZEPHYR_MODULES=/path/to/release-hal-wch;/path/to/zephyr-wch-drivers' \
  -DZEPHYR_SDK_INSTALL_DIR=/path/to/zephyr-sdk-1.0.1
```

For the second variant add `-DCONFIG_UART_INTERRUPT_DRIVEN=n` and use a new
build directory. Both include compile-time UART/DMA public API signature
checks. The BluePill overlay disables USART3/console, wires USART1/2, and
supplies the DMA1 node missing in v4.4.0. This is a compile fixture, not a
hardware validation or an unmodified release DT configuration. Gello itself
does not build on that release because its EXTI node is also absent.

## USB increment verification record

Same Zephyr/HAL revisions and SDK as the RX record below; neither checkout is
patched. All hardware results remain **NOT RUN**.

| Check | Result | Evidence type |
| --- | --- | --- |
| UDC native, current 4.4.99 / official 4.4.0 | 18/18 each | Production UDC/common API, modeled registers |
| Full current native matrix | 85/85 | UART TX 20 + RX 35 + DMA 12 + UDC 18 |
| Gello + BluePill board matrix | 12/12 built | UART polling/IRQ/TX/RX, DMA copy, USB on each board |
| Host USB script, normal / Python `-O` | 4/4 each | Fake USB API; no physical device access |
| Negative configuration guards | 12/12 | Specific expected build diagnostics |
| Current Gello USB / release BluePill USB | Built | ROM 37,648 / 34,192 B; RAM 14,328 / 14,056 B |
| STM32 Florid LED | Built | Module-disabled regression |

Local evidence: `build/test-wch-usb-matrix/`, `build/test-wch-udc-native/`,
`build/test-wch-udc-native-v440/`, `build/test-wch-usb-hil-dev/`,
`build/test-wch-usb-v440/` and `build/test-wch-usb-stm32/`. Final UDC lifecycle
adjustments were rerun in both native fixtures and both USB firmware builds
after the full regression matrix. See the [USB runbook](../docs/usb-udc.md)
for the intentionally unverified clock/SIE/DMA/pad assumptions and joint gates.

## RX increment verification record

Zephyr `577e42ad187825cc30d4b51423c32872eb4cf054` (4.4.99) with HAL
`1713a445d44278e902a00e0fee3a111d7bde0d60`; independent v4.4.0
`684c9e8f32e4373a21098559f748f06915f950c9` with HAL
`dd3855ea624b05de7e6e95584789615d2058a0f3`. SDK 1.0.1; host GCC 16.2.1.

| Check | Result | Evidence type |
| --- | --- | --- |
| TX-only / RX-enabled native, 4.4.99 | 20/20 + 35/35 | Executed production UART, modeled DMA/MMIO |
| RX-enabled native, v4.4.0 | 35/35 | Same source, release API compatibility |
| DMA native / arithmetic / negative guards | 12/12 / 3/3 / 9/9 | Modeled DMA / host CTest / expected build diagnostics |
| Gello + BluePill UART polling/IRQ/TX/RX and DMA copy; Gello USB placeholder | 11/11 built | No hardware execution |
| v4.4.0 BluePill RX with DMA1 overlay | Built | Release driver/test compatibility |
| Gello RX with 115200 overlay | Built: ROM 51,720 B, RAM 11,824 B | ztest image, not production footprint |
| STM32 Florid LED | Built | Module-disabled regression |

Local Ragtime evidence: `build/test-wch-rx-matrix/` (board builds and initial
native run), `build/test-wch-rx-native-final/` (final 55 UART cases),
`build/test-wch-rx-native-v440/`, `build/test-wch-rx-hardware-v440/`,
`build/test-wch-rx-low-baud/`, `build/test-wch-rx-contract/` and
`build/test-wch-rx-stm32/`. Build artifacts are intentionally untracked.
No flashing occurred. RX inactivity timeout, continuous traffic qualification
and USB remain pending at the [hardware gate](../docs/hardware-bringup.md).

## TX increment verification record (historical)

Same pinned Zephyr/HAL pairs and SDK as below. No hardware was flashed.

| Check | Result | Evidence type |
| --- | --- | --- |
| UART TX native, 4.4.99 and v4.4.0 | 20/20 each | Executed production UART with modeled provider/MMIO |
| Existing DMA native suite, 4.4.99 | 12/12 | Executed production DMA with modeled MMIO |
| UART arithmetic helpers | 3/3 | Host CTest |
| Negative guards | 9/9 expected diagnostics | Compile-failure checks |
| Gello + BluePill polling/IRQ/TX, DMA copy; Gello USB placeholder | 9/9 built | No device execution |
| v4.4.0 BluePill TX with DMA1 overlay | Built | Release API/binding compatibility |
| STM32 Florid LED | Built | Module-disabled regression |

RX/USB were unsupported at that milestone; passing native tests is not hardware qualification.

## DMA prerequisite verification record (historical)

Same pinned Zephyr/HAL pairs and SDK as round 1; host compiler GCC 16.2.1.

| Check | Result | Evidence type |
| --- | --- | --- |
| DMA native_sim, Ragtime 4.4.99 | 12/12 passed | Production driver with modeled MMIO, executed |
| DMA native_sim, stock v4.4.0 | 12/12 passed | Production driver with modeled MMIO, executed |
| UART arithmetic helpers | 3/3 passed | Host CTest |
| Negative configuration guards | 7/7 expected diagnostics | Compile-failure checks |
| Gello + BluePill UART IRQ/polling and DMA copy; Gello USB placeholder | 7/7 built | No board execution |
| v4.4.0 BluePill UART IRQ/polling with DMA1 overlay | 2/2 built | Release compatibility, not Gello |
| STM32 Florid LED | Built | Module-disabled regression |

No hardware was flashed. Native tests do not prove actual data movement;
the hardware DMA copy test is implemented but remains unexecuted. UART async
and USB transfer cases still skip. See [the DMA contract](../docs/dma-driver.md).

## Round-1 verification record (historical)

Zephyr SDK 1.0.1; exact Zephyr/HAL pairs are in the [audit](../docs/uart-dma-design.md).

| Check | Result | Evidence type |
| --- | --- | --- |
| Host baud/buffer/progress | 3/3 passed | Executed on host |
| Negative build guards | 6/6 expected diagnostics | Executed compile-failure checks |
| 4.4.99 Gello UART interrupt/polling and USB placeholder | 3/3 built | Compile only, no device run |
| 4.4.99 BluePill UART interrupt/polling | 2/2 built | Compile fixture regression |
| v4.4.0 BluePill + DMA1 fixture, UART interrupt/polling | 2/2 built | Compile only, not Gello |
| 4.4.99 Gello hello_world, external UART enabled / DMA disabled | Built | Non-DMA UART regression |
| 4.4.99 STM32 Florid LED, module disabled | Built | Compile-only regression |
| v4.4.0 Gello, unchanged board | Fails on missing `exti` | Known board compatibility gap |

UART DMA transfer and USB cases remain explicit skips. No hardware was flashed.

## Hardware procedure to complete later

1. Keep Gello's console disabled: both UARTs belong to the test fixture. No
   serial ztest report is currently available; inspect ztest state with a
   debugger. Add an independent reporting transport before automated HIL runs.
2. UART fixture: 3.3 V logic and common ground; never use RS-232 levels. For
   future single-port loopback join PA9 to PA10 and PA2 to PA3. Do not connect
   two TX outputs together. Establish low-speed operation before 3 Mbaud and
   simultaneous two-port load. The TX-only scenario now sends test data:
   attach high-impedance analyzer inputs or two 3.3 V UART adapter RX inputs
   to PA9 and PA2, with common ground. Record bytes and last-stop-bit timing.
   No TX hardware run has been performed yet.
3. USB fixture: data-capable cable and host test process. Gello USB shares pins
   with SDI; disconnect the debugger from the USB pair before enumeration and
   retain NRST access for recovery. Do not configure USB metadata as GPIOs.
4. Keep `build_only` until real execution and reliable result capture exist.
   USB assertions are in the host runner, not the firmware status marker;
   follow the USB runbook for packet boundaries and independent reconnect tests.
5. Record board revision, wiring, Zephyr/HAL/module SHAs, firmware config,
   host command and captured results. Build logs alone are not HIL evidence.
