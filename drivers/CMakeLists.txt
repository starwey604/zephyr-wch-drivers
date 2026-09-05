# SPDX-License-Identifier: Apache-2.0

if(CONFIG_WCH_UART AND CONFIG_UART_WCH_USART)
  message(FATAL_ERROR "Disable CONFIG_UART_WCH_USART before enabling CONFIG_WCH_UART")
endif()

add_subdirectory_ifdef(CONFIG_WCH_UART serial)
