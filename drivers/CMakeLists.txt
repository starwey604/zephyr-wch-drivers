# SPDX-License-Identifier: Apache-2.0

if(CONFIG_WCH_UART AND CONFIG_UART_WCH_USART)
  message(FATAL_ERROR "Disable CONFIG_UART_WCH_USART before enabling CONFIG_WCH_UART")
endif()

add_subdirectory_ifdef(CONFIG_WCH_UART serial)
if(CONFIG_WCH_DMA AND CONFIG_DMA_WCH)
  message(FATAL_ERROR "Disable CONFIG_DMA_WCH before enabling CONFIG_WCH_DMA")
endif()
add_subdirectory_ifdef(CONFIG_WCH_DMA dma)
if(CONFIG_WCH_UART_ASYNC_TX AND (CONFIG_UART_INTERRUPT_DRIVEN OR CONFIG_UART_WIDE_DATA))
  message(FATAL_ERROR "WCH async TX requires UART_INTERRUPT_DRIVEN=n and UART_WIDE_DATA=n")
endif()
