/* SPDX-License-Identifier: Apache-2.0 */

#ifndef WCH_UART_DMA_DT_H_
#define WCH_UART_DMA_DT_H_

#include <zephyr/devicetree.h>
#include <zephyr/sys/util.h>

/* Only default-remap USART1/2 on CH32V203 are in scope for this first stage. */
#define WCH_DMA_CTLR(i, dir) DT_INST_DMAS_CTLR_BY_NAME(i, dir)
#define WCH_DMA_CH(i, dir) DT_INST_DMAS_CELL_BY_NAME(i, dir, channel)
#define WCH_UART1_ADDR 0x40013800U
#define WCH_UART2_ADDR 0x40004400U

#define WCH_UART_DMA_DT_CHECK(i)                                                               \
	BUILD_ASSERT(DT_INST_DMAS_HAS_NAME(i, tx) && DT_INST_DMAS_HAS_NAME(i, rx),               \
		     "WCH UART DMA requires tx and rx dma-names");                              \
	BUILD_ASSERT(DT_INST_PROP_LEN(i, dmas) == 2, "WCH UART requires exactly two DMA requests"); \
	BUILD_ASSERT(DT_INST_REG_ADDR(i) == WCH_UART1_ADDR ||                                  \
		     DT_INST_REG_ADDR(i) == WCH_UART2_ADDR, "DMA preparation supports USART1/2"); \
	BUILD_ASSERT(DT_NODE_HAS_STATUS(WCH_DMA_CTLR(i, tx), okay) &&                          \
		     DT_NODE_HAS_STATUS(WCH_DMA_CTLR(i, rx), okay), "DMA controller is disabled"); \
	BUILD_ASSERT(DT_NODE_HAS_COMPAT(WCH_DMA_CTLR(i, tx), wch_wch_dma) &&                    \
		     DT_SAME_NODE(WCH_DMA_CTLR(i, tx), WCH_DMA_CTLR(i, rx)), "Use WCH DMA1");   \
	BUILD_ASSERT(DT_REG_ADDR(WCH_DMA_CTLR(i, tx)) == 0x40020000U, "Use DMA1, not DMA2");    \
	BUILD_ASSERT(WCH_DMA_CH(i, tx) < DT_PROP(WCH_DMA_CTLR(i, tx), dma_channels) &&           \
		     WCH_DMA_CH(i, rx) < DT_PROP(WCH_DMA_CTLR(i, rx), dma_channels),             \
		     "DMA channel index is out of range");                                    \
	BUILD_ASSERT(WCH_DMA_CH(i, tx) == (DT_INST_REG_ADDR(i) == WCH_UART1_ADDR ? 3 : 6) &&     \
		     WCH_DMA_CH(i, rx) == (DT_INST_REG_ADDR(i) == WCH_UART1_ADDR ? 4 : 5),        \
		     "Incorrect CH32V203 UART DMA request mapping (indices are zero-based)");

#endif /* WCH_UART_DMA_DT_H_ */
