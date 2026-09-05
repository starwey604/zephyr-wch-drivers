/* SPDX-License-Identifier: Apache-2.0 */
#ifndef WCH_TX_TEST_HAL_H_
#define WCH_TX_TEST_HAL_H_
#include <stdint.h>
/* Layout/values match hal_wch ch32v20xhw.h. Tests inject hardware flags and
 * explicitly account for STATR write-zero-to-clear; no UART is simulated.
 */
typedef struct {
	volatile uint16_t STATR;
	uint16_t reserved0;
	volatile uint16_t DATAR;
	uint16_t reserved1;
	volatile uint16_t BRR;
	uint16_t reserved2;
	volatile uint16_t CTLR1;
	uint16_t reserved3;
	volatile uint16_t CTLR2;
	uint16_t reserved4;
	volatile uint16_t CTLR3;
	uint16_t reserved5;
	volatile uint16_t GPR;
	uint16_t reserved6;
} USART_TypeDef;
#define USART_STATR_PE   0x001U
#define USART_STATR_FE   0x002U
#define USART_STATR_NE   0x004U
#define USART_STATR_ORE  0x008U
#define USART_STATR_RXNE 0x020U
#define USART_STATR_TC   0x040U
#define USART_STATR_TXE  0x080U
#define USART_STATR_LBD  0x100U
#define USART_CTLR1_RE   0x004U
#define USART_CTLR1_TE   0x008U
#define USART_CTLR1_TCIE 0x040U
#define USART_CTLR1_PEIE 0x100U
#define USART_CTLR1_UE   0x2000U
#define USART_CTLR3_DMAT 0x080U
#define USART_CTLR3_DMAR 0x040U
#define USART_CTLR3_EIE  0x001U
#endif
