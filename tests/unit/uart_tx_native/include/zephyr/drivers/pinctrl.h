/* SPDX-License-Identifier: Apache-2.0 */
#ifndef WCH_TX_TEST_PINCTRL_H_
#define WCH_TX_TEST_PINCTRL_H_
#define PINCTRL_STATE_DEFAULT 0
struct pinctrl_dev_config {
	int unused;
};
static int pinctrl_result;
static inline int pinctrl_apply_state(const struct pinctrl_dev_config *config, uint8_t state)
{
	ARG_UNUSED(config);
	ARG_UNUSED(state);
	return pinctrl_result;
}
#endif
