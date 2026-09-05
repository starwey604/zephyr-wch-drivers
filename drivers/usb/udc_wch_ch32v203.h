/* SPDX-License-Identifier: Apache-2.0 */
#include <hal_ch32fun.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/dt-bindings/clock/ch32v20x_30x-clocks.h>

BUILD_ASSERT(offsetof(USBOTG_FS_TypeDef, UEP0_DMA) == WCH_DMA(0));
BUILD_ASSERT(offsetof(USBOTG_FS_TypeDef, UEP2_RX_CTRL) == WCH_RX_CTL(2));
BUILD_ASSERT(offsetof(USBOTG_FS_TypeDef, RX_LEN) == WCH_RX_LEN);

static int wch_prepare(const struct device *dev)
{
	const struct wch_udc_config *config = dev->config;
	struct wch_udc_data *priv = udc_get_private(dev);
	uint32_t divider;

	if (!device_is_ready(config->clock)) {
		return -ENODEV;
	}
	/* Only SYSCLK=PLL and undivided AHB are supported. A divided bus rate
	 * returned by clock_control_get_rate() cannot establish PLL frequency.
	 */
	if (config->pll_hz != CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC ||
	    (RCC->CFGR0 & RCC_SWS) != RCC_SWS_PLL || (RCC->CFGR0 & RCC_HPRE) != 0) {
		return -ENOTSUP;
	}
	switch (config->pll_hz) {
	case 48000000:
		divider = RCC_USBPRE_DIV1;
		break;
	case 96000000:
		divider = RCC_USBPRE_DIV2;
		break;
	case 144000000:
		divider = RCC_USBPRE_DIV3;
		break;
	default:
		return -ENOTSUP;
	}
	int ret = clock_control_on(config->clock,
				   (clock_control_subsys_t)(uintptr_t)config->clock_id);

	if (ret != 0) {
		return ret;
	}
	priv->saved_usbpre = RCC->CFGR0 & RCC_USBPRE;
	RCC->CFGR0 = (RCC->CFGR0 & ~RCC_USBPRE) | divider;
	if (config->release_sdi) {
		ret = clock_control_on(config->clock,
				       (clock_control_subsys_t)(uintptr_t)CH32V20X_V30X_CLOCK_AFIO);
		if (ret != 0) {
			RCC->CFGR0 = (RCC->CFGR0 & ~RCC_USBPRE) | priv->saved_usbpre;
			clock_control_off(config->clock,
					  (clock_control_subsys_t)(uintptr_t)config->clock_id);
			return ret;
		}
		priv->saved_sdi = AFIO->PCFR1 & AFIO_PCFR1_SWJ_CFG;
		AFIO->PCFR1 = (AFIO->PCFR1 & ~AFIO_PCFR1_SWJ_CFG) | AFIO_PCFR1_SWJ_CFG_DISABLE;
	}
	return 0;
}

static void wch_unprepare(const struct device *dev)
{
	const struct wch_udc_config *config = dev->config;
	struct wch_udc_data *priv = udc_get_private(dev);

	if (config->release_sdi) {
		AFIO->PCFR1 = (AFIO->PCFR1 & ~AFIO_PCFR1_SWJ_CFG) | priv->saved_sdi;
	}
	RCC->CFGR0 = (RCC->CFGR0 & ~RCC_USBPRE) | priv->saved_usbpre;
	clock_control_off(config->clock, (clock_control_subsys_t)(uintptr_t)config->clock_id);
}

#define WCH_UDC_DEFINE(n)                                                                          \
	BUILD_ASSERT(!DT_INST_PROP(n, wch_disable_sdi) || IS_ENABLED(CONFIG_WCH_UDC_RELEASE_SDI),  \
		     "Shared USB/SDI pins require explicit CONFIG_WCH_UDC_RELEASE_SDI=y");         \
	BUILD_ASSERT(DT_INST_REG_ADDR(n) == 0x50000000, "Use USBFS, not USBD");                    \
	BUILD_ASSERT(DT_INST_CLOCKS_CELL(n, id) == CH32V20X_V30X_CLOCK_OTG_FS,                     \
		     "USBFS requires the AHB bit 12 clock");                                       \
	static struct udc_ep_config wch_eps_##n[4];                                                \
	static struct wch_udc_data wch_priv_##n;                                                   \
	static struct udc_data wch_data_##n = {.priv = &wch_priv_##n};                             \
	static void wch_irq_config_##n(const struct device *dev)                                   \
	{                                                                                          \
		IRQ_CONNECT(DT_INST_IRQN(n), DT_INST_IRQ(n, priority), wch_isr,                    \
			    DEVICE_DT_INST_GET(n), 0);                                             \
		irq_enable(DT_INST_IRQN(n));                                                       \
	}                                                                                          \
	static const struct wch_udc_config wch_config_##n = {                                      \
		.base = DT_INST_REG_ADDR(n),                                                       \
		.eps = wch_eps_##n,                                                                \
		.prepare = wch_prepare,                                                            \
		.unprepare = wch_unprepare,                                                        \
		.irq_config = wch_irq_config_##n,                                                  \
		.clock = DEVICE_DT_GET(DT_INST_CLOCKS_CTLR(n)),                                    \
		.clock_id = DT_INST_CLOCKS_CELL(n, id),                                            \
		.pll_hz = DT_INST_PROP(n, wch_pll_frequency),                                      \
		.release_sdi = DT_INST_PROP(n, wch_disable_sdi),                                   \
	};                                                                                         \
	DEVICE_DT_INST_DEFINE(n, wch_preinit, NULL, &wch_data_##n, &wch_config_##n, POST_KERNEL,   \
			      CONFIG_KERNEL_INIT_PRIORITY_DEVICE, &wch_api);

DT_INST_FOREACH_STATUS_OKAY(WCH_UDC_DEFINE)
