/* SPDX-License-Identifier: Apache-2.0 */
#include <zephyr/kernel.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/sys/printk.h>
#include <hal_ch32fun.h>

static const struct device *const port = DEVICE_DT_GET(DT_ALIAS(serial0));

/* Debugger-readable progress; never a substitute for checking wire bytes. */
volatile struct {
	uint32_t magic;
	uint32_t stage;
	uint32_t loops;
	uint32_t tx_bytes;
	uint32_t rx_bytes;
	uint32_t last_rx;
	uint32_t uptime_ms;
	uint32_t boot_cfgr0;
	uint32_t boot_exten;
	uint32_t boot_brr;
} wch_poll_diag;

static void send(const char *text)
{
	while (*text != '\0') {
		uart_poll_out(port, *text++);
		wch_poll_diag.tx_bytes++;
	}
}

int main(void)
{
	char line[64];
	size_t used = 0;
	bool overflow = false;
	int64_t last_rx = 0;
	int64_t next_heartbeat = 0;

	wch_poll_diag.magic = 0x57434850;
	wch_poll_diag.boot_cfgr0 = RCC->CFGR0;
	wch_poll_diag.boot_exten = EXTEN->EXTEN_CTR;
	wch_poll_diag.boot_brr = USART1->BRR;
	wch_poll_diag.stage = 1;
	if (!device_is_ready(port)) {
		wch_poll_diag.stage = 0xff;
		return -ENODEV;
	}
	wch_poll_diag.stage = 2;
	send("\r\nWCH POLL HIL v1; UART1 115200 8N1; USB/DMA off\r\n");
	char clocks[112];
	snprintk(clocks, sizeof(clocks), "BOOT cfgr0=%08x exten=%08x brr=%08x\r\n",
		 wch_poll_diag.boot_cfgr0, wch_poll_diag.boot_exten, wch_poll_diag.boot_brr);
	send(clocks);
	wch_poll_diag.stage = 3;
	for (;;) {
		unsigned char byte;

		wch_poll_diag.loops++;
		while (uart_poll_in(port, &byte) == 0) {
			wch_poll_diag.rx_bytes++;
			wch_poll_diag.last_rx = byte;
			last_rx = k_uptime_get();
			if (byte == '\r') {
				continue;
			}
			if (byte == '\n') {
				line[used] = '\0';
				if (overflow) {
					send("ERR line too long\r\n");
				} else {
					send("ECHO ");
					send(line);
					send("\r\n");
				}
				used = 0;
				overflow = false;
			} else if (used < sizeof(line) - 1) {
				line[used++] = byte;
			} else {
				overflow = true;
			}
		}
		int64_t now = k_uptime_get();
		wch_poll_diag.uptime_ms = now;

		if (now >= next_heartbeat && now - last_rx >= 500 && used == 0) {
			char status[96];

			snprintk(status, sizeof(status), "WCH ALIVE uptime_ms=%lld cpu_hz=%u\r\n",
				 (long long)now, (unsigned int)CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC);
			send(status);
			next_heartbeat = now + 1000;
		}
		k_sleep(K_MSEC(1));
	}
}
