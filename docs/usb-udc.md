# USBFS UDC bring-up boundary

**Experimental, awaiting hardware.** USB is advanced independently at the
product owner's request; the UART RX hardware gate remains unchanged. This
revision implements a controller and real host-driven fixture, not a qualified
USB product. No board has been flashed and no USB enumeration has been observed.

## Scope and source provenance

The reference is BOJIT/zephyr `driver/ch32_usb`, exact commit
`dc53b3104fbbe6db5d35d3276d281ffdc3da6483`, path
`drivers/usb/udc/udc_wch.c` (Bootlin 2025, Apache-2.0). Attribution is retained.
Its register operations informed this implementation; transfer/control and
lifetime handling are substantially rewritten. The old code's control-stage
helpers are absent from the tested Zephyr APIs; it also lacks robust busy,
cancellation and bounds handling. We did not import its printf/delay workarounds,
direct net_buf DMA addressing or claims of ISO/high-speed/remote wakeup support.
WCH vendor examples were consulted for registers/clocking, not copied.

Supported subset: CH32V203 USBFS/USBHD at `0x50000000`, IRQ 59, Full Speed,
EP0 IN/OUT control (64-byte MPS), EP1 IN (`0x81`) and EP2 OUT (`0x02`) bulk
(up to 64-byte MPS). The narrow capability table deliberately rejects other
directions/types. No interrupt/ISO, SOF reporting, remote wakeup, VBUS sensing,
power management, low/high-speed, or combined UART/USB throughput is qualified.
This is **not** the separate USBD peripheral at `0x40005c00`.

## Integration and DMA

Enable `WCH_DRIVERS`, `USB_DEVICE_STACK_NEXT` and `WCH_UDC`, and provide the
`wch,ch32v203-usbfs` DT node. The test overlays are opt-in examples; Gello's
board defaults still preserve SDI and do not enable USB. The binding requires
the AHB bit-12 clock and a 48/96/144 MHz PLL description; the driver additionally
requires SYSCLK=PLL and undivided AHB, then sets USB PLL /1, /2 or /3.
Clock arithmetic does not establish HSI tolerance or actual USB clock accuracy.
Use the fixture's `CONFIG_UDC_WORKQUEUE_STACK_SIZE=2048` as the bring-up baseline;
the upstream 512-byte default has not been qualified for this call path. Measure
stack high-water usage on hardware before tuning memory or enabling more classes.

Three four-byte-aligned 64-byte SRAM staging buffers are permanently assigned
to EP0/1/2 DMA registers. USBFS moves packets through its **own endpoint DMA**;
there is no DMA1 channel request and no dependency on the UART/general DMA fix.
Software copies between staging and `net_buf`, trading zero-copy throughput
for alignment, short-allocation safety and EP0 SETUP preemption safety.
EP1 is TX-only and EP2 RX-only, avoiding shared-direction DMA offset modes.

IN progress advances only after completion, handles multiple packets and an
optional terminating ZLP, and then returns exactly one request. OUT validates
packet size/tailroom before copying; full requests and short/ZLP packets complete
the request, while wrong-toggle duplicates do not consume application bytes.
Oversize OUT completes with `-EMSGSIZE`, without copying beyond the allocation.

## Control, events and lifetime

The ISR masks controller interrupts and schedules per-instance work. Hardware
TRANSFER remains latched: `INT_BUSY` is intended to NAK further traffic while
the workqueue consumes staging data. The worker holds the UDC mutex and calls
`udc_setup_received()` in thread context; Zephyr owns Chapter 9/data/status
sequencing. The driver never uses the removed control-stage helpers and does
not change the address early (`addr_before_status=false`).

New SETUP resets EP0 toggles/protocol halt and uses the common cancellation/cache
logic. RESET takes precedence over stale completion, clears address and cached
SETUP, cancels requests and disables non-control endpoints. FIFO overflow also
beats completion: detach, cancel, report ERROR and reject further loans until
the application disables/reinitializes the stack. Suspend/resume events are
reported but no power-saving or remote-resume signal is implemented.

Cancellation first NAKs the endpoint, waits at most 100 microseconds for
`SIE_FREE`, then discards its latched completion before returning requests with
`-ECONNABORTED`. Timeout returns `-EBUSY` and retains the loan. Functional halt
preserves progress of a previously ACKed packet; clear-halt resets DATA0 and
restarts pending work. Disable removes the pull-up/port/DMA enable, masks IRQs,
cancels queues and makes stale work harmless. Shutdown restores the saved USB
divider and, where requested, SDI selection. These register-side effects and
in-flight cancellation timing **require silicon validation**.

This out-of-tree driver includes Zephyr's private `drivers/usb/udc/udc_common.h`
from the selected tree, not a copied shim. Both tested revisions provide the
required UDC API. The test class has a small CMake-detected adapter because
official 4.4.0 supplies control-IN buffers while Ragtime's 4.4.99 API allocates
them in the class. No Zephyr or HAL checkout is patched.

## Firmware and host fixture

From Ragtime dev, after updating the pinned module:

```sh
.venv/bin/west build -s modules/drivers/wch/tests/subsys/usb_endpoints \
  -b ragtime_gello -d build/test-wch-usb-hil --pristine always -- \
  -DEXTRA_CONF_FILE=$PWD/modules/drivers/wch/tests/configs/ragtime-cpp20.conf \
  -DZEPHYR_SDK_INSTALL_DIR=/home/ww/zephyr-sdk-1.0.1
```

For standalone module development, use its test source/config paths and add
`-DEXTRA_ZEPHYR_MODULES=/path/to/zephyr-wch-drivers`. The image waits **five seconds**
before USB initialization releases shared SDI pins. Its status variable is
diagnostic only, not a PASS indicator. There is no UART console or debug channel
available on the shared USB pair while USB is active.

The vendor-class fixture supports EP0 write/read requests `0x5b`/`0x5c` and
one outstanding bulk echo frame (maximum 256 bytes). It rearms OUT after IN
completion. Short exact-MPS OUT frames require a host ZLP; a full 256-byte
buffer does not. Exact-MPS IN echoes append a ZLP for a larger host read.
Temporary VID/PID `1209:ffff` are laboratory placeholders, **not an allocation
claimed by this project**. Override `WCH_TEST_USB_VID/PID` before deployment.

The host needs PyUSB and libusb. Once the board and host permissions are ready:

```sh
lsusb
.venv/bin/python modules/drivers/wch/tests/host/usb_endpoints.py \
  --bus <DUT-bus-number> --address <DUT-device-number> --cycles 10
```

The script requires an exact bus/address plus matching IDs, product string and
endpoint descriptors before configuration writes; it does not automatically
detach kernel drivers. Each transfer has a timeout. It compares EP0 and bulk
bytes at lengths 0/1/63/64/65/127/128/255/256, checks protocol STALL recovery,
endpoint halt/clear-halt and post-halt traffic, and emits a JSON result.
Clear-halt uses the libusb backend operation to synchronize host-side toggles,
not just a raw control request to the device.
`--reset-after` only requests reset after those checks: rerun on the new address
to verify re-enumeration. Replug/suspend/stress are separate manual gates.
Host unit tests use a fake API and do not access any USB device.

## Mandatory joint hardware gates — stop here until the board arrives

1. **Wiring/recovery before flashing.** Confirm actual QFN20 schematic/pinout,
   supply and USB routing. USB D+ is package 16 and D- package 17, shared with
   SDI. Preserve NRST access; verify connect-under-reset during the startup
   delay. Disconnect the probe from the pair before connecting host USB. Do
   not assume the five-second delay makes electrical contention safe.
2. **Clock/attach/EP0.** Validate the actual 48 MHz source and HSI accuracy;
   inspect host enumeration/descriptors and SET_ADDRESS after status. Repeated
   resets, retries or disconnects are a stop signal, not evidence of working bulk.
3. **Packet DMA.** Run the bounded host fixture, inspect missing/duplicate bytes,
   short/ZLP termination and EP0 SETUP preemption, including truncated/aborted
   control transfers. Confirm INT_BUSY really protects staging until consumed.
4. **Lifecycle/races.** Measure SIE_FREE/cancel behavior during actual IN/OUT,
   clear-halt DATA0, bus reset during traffic, unplug/replug and host suspend/
   resume. Validate clocks/debug restoration separately, only after USB is
   electrically disconnected. Do not breakpoint timing paths with shared SDI.
5. **Later qualification.** Only after those pass, test sustained traffic,
   voltage/temperature, USB electrical requirements and combined UART+USB RAM/
   IRQ load. No production USB or throughput claim is made by this increment.

Record board revision/wiring, firmware/Zephyr/HAL/module SHAs, `.config`, host
command/version, JSON result, host USB logs and analyzer traces. Mark each gate
PASS/FAIL/NOT RUN explicitly. No hardware report exists yet.

References: [pinned community source](https://github.com/BOJIT/zephyr/blob/dc53b3104fbbe6db5d35d3276d281ffdc3da6483/drivers/usb/udc/udc_wch.c),
[Zephyr 4.4.0 UDC contract](https://github.com/zephyrproject-rtos/zephyr/blob/v4.4.0/drivers/usb/udc/udc_common.h),
[WCH USBFS example](https://github.com/openwch/ch32v20x/blob/main/EVT/EXAM/USB/USBFS/DEVICE/SimulateCDC/User/USB_Device/ch32v20x_usbfs_device.c).
