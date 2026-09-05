/* SPDX-License-Identifier: Apache-2.0 */

#include <zephyr/drivers/uart.h>
#include <zephyr/drivers/dma.h>

/* Compile-only signature checks against each supported Zephyr checkout. */
#define CHECK_API(name, type) _Static_assert(_Generic(&(name), type: 1, default: 0), #name)

typedef int (*callback_api)(const struct device *, uart_callback_t, void *);
typedef int (*tx_api)(const struct device *, const uint8_t *, size_t, int32_t);
typedef int (*rx_api)(const struct device *, uint8_t *, size_t, int32_t);
typedef int (*rx_buffer_api)(const struct device *, uint8_t *, size_t);
typedef int (*device_api)(const struct device *);
typedef int (*dma_status_api)(const struct device *, uint32_t, struct dma_status *);
typedef int (*dma_config_api)(const struct device *, uint32_t, struct dma_config *);

CHECK_API(uart_callback_set, callback_api);
CHECK_API(uart_tx, tx_api);
CHECK_API(uart_tx_abort, device_api);
CHECK_API(uart_rx_enable, rx_api);
CHECK_API(uart_rx_buf_rsp, rx_buffer_api);
CHECK_API(uart_rx_disable, device_api);
CHECK_API(dma_get_status, dma_status_api);
CHECK_API(dma_config, dma_config_api);
