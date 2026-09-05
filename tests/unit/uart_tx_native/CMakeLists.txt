# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(wch_uart_tx_native LANGUAGES C)
target_include_directories(app PRIVATE include ../../../drivers/serial)
# Compile the production UART source without pretending native_sim is CH32V203.
# Only this app target gets the two device-specific branch selectors.
target_compile_definitions(app PRIVATE CONFIG_WCH_UART_ASYNC_TX=1 CONFIG_WCH_UART_DMA_PREPARE=1)
target_sources(app PRIVATE src/main.c)
if(WCH_TEST_RX)
  target_compile_definitions(app PRIVATE CONFIG_WCH_UART_ASYNC_RX=1)
endif()
