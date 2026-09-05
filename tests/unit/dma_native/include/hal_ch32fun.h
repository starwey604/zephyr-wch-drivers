/* SPDX-License-Identifier: Apache-2.0 */
#ifndef WCH_DMA_TEST_HAL_H_
#define WCH_DMA_TEST_HAL_H_

/* Minimal register model, not a replacement HAL. Values match hal_wch's
 * ch32v20xhw.h. Production builds always include the real HAL instead.
 * Tests explicitly emulate INTFCR write-one-to-clear and CNTR/flag changes;
 * this model does not move memory or emulate bus timing/arbitration.
 */
#include <stdint.h>
typedef struct {
	volatile uint32_t INTFR;
	volatile uint32_t INTFCR;
} DMA_TypeDef;

#define DMA_GIF1          0x0001U
#define DMA_TCIF1         0x0002U
#define DMA_HTIF1         0x0004U
#define DMA_TEIF1         0x0008U
#define DMA_CFGR1_EN      0x0001U
#define DMA_CFGR1_TCIE    0x0002U
#define DMA_CFGR1_HTIE    0x0004U
#define DMA_CFGR1_TEIE    0x0008U
#define DMA_CFGR1_DIR     0x0010U
#define DMA_CFGR1_CIRC    0x0020U
#define DMA_CFGR1_PINC    0x0040U
#define DMA_CFGR1_MINC    0x0080U
#define DMA_CFGR1_PSIZE_0 0x0100U
#define DMA_CFGR1_MSIZE_0 0x0400U
#define DMA_CFGR1_PL_0    0x1000U
#define DMA_CFGR1_MEM2MEM 0x4000U
#endif
