# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(wch_uart_dma_tests LANGUAGES C)
target_sources(app PRIVATE src/main.c src/api_contract.c)
target_sources_ifdef(CONFIG_WCH_UART_ASYNC_TX app PRIVATE src/tx.c)
target_sources_ifdef(CONFIG_WCH_UART_ASYNC_RX app PRIVATE src/rx.c)
