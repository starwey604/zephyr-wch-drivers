# SPDX-License-Identifier: Apache-2.0
zephyr_library()
# UDC drivers depend on this private Zephyr header; compatibility is tested
# against both pinned Ragtime and release 4.4.0, not assumed across releases.
zephyr_library_include_directories(${ZEPHYR_BASE}/drivers/usb/udc)
zephyr_library_sources(udc_wch.c)
