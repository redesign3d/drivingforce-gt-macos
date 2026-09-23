/* Stage 1: ESP32-S3 hosts the DFGT, decodes it, and presents it to macOS as a
 * BLE HID gamepad.
 *
 * Console is UART0 = the board's CH343 bridge port (the native USB port is busy
 * being the wheel's host). Default behaviour is discovery: raw report next to
 * the decoded one, so the protocol carried over from the macOS driver is
 * re-confirmed on this hardware before anything trusts it.
 */
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"

#include "ble_gamepad.h"
#include "dfgt_decode.h"
#include "dfgt_usb.h"
#include "pad_map.h"

#define TAG "dfgt"

#define PRINT_MAX_HZ	40	/* the console cannot carry every change at 115200 */
#define SUMMARY_MS	10000

static struct {
	bool		 show_raw;
	bool		 show_state;
	uint32_t	 reports;
	uint32_t	 changes;
	uint32_t	 suppressed;
	uint32_t	 last_print_us;
	uint8_t		 last_raw[DFGT_REPORT_LEN];
	bool		 have_last;
	QueueHandle_t	 pad_q;
} g;

/* ------------------------------------------------------------- report path */

/* Runs in the USB client task: decode, publish, and print at a readable rate.
 * The BLE send happens on another task because it can wait on the GATT confirm;
 * nothing on this path is allowed to stall the wheel's polling. */
static void on_report(const uint8_t *data, size_t len, void *arg)
{
	struct dfgt_state st;
	struct pad_state pad;
	char line[256];
	uint32_t now;
	bool changed;

	if (!dfgt_decode(data, len, &st)) {
		return;	/* the USB layer already logged the unexpected length */
	}

	now = (uint32_t)(esp_timer_get_time() / 1000);
	st.t_ms = now;
	g.reports++;

	changed = !g.have_last || memcmp(g.last_raw, data, DFGT_REPORT_LEN) != 0;
	memcpy(g.last_raw, data, DFGT_REPORT_LEN);
	g.have_last = true;
	if (changed) {
		g.changes++;
	}

	pad_map_from_wheel(&st, &pad);
	xQueueOverwrite(g.pad_q, &pad);		/* latest state wins; never blocks */

	if (!changed) {
		return;				/* change-driven device: no news is not news */
	}
	if (now - g.last_print_us < 1000 / PRINT_MAX_HZ) {
		g.suppressed++;
		return;
	}
	g.last_print_us = now;

	if (g.show_raw) {
		ESP_LOGI(TAG, "raw  %02x %02x %02x %02x %02x %02x %02x %02x",
			 st.raw[0], st.raw[1], st.raw[2], st.raw[3],
			 st.raw[4], st.raw[5], st.raw[6], st.raw[7]);
	}
	if (g.show_state) {
		dfgt_state_line(&st, line, sizeof(line));
		ESP_LOGI(TAG, "%s", line);
		ESP_LOGI(TAG, "pad  x=%u y=%u z=%u rz=%u hat=%u buttons=0x%04x -> %s",
			 pad.x, pad.y, pad.z, pad.rz, pad.hat, pad.buttons,
			 ble_gamepad_state_str());
	}
}

/* ---------------------------------------------------------------- BLE task */

static void ble_task(void *arg)
{
	struct pad_state pad;

	for (;;) {
		if (xQueueReceive(g.pad_q, &pad, portMAX_DELAY) == pdTRUE) {
			esp_err_t err = ble_gamepad_send(&pad);
			if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
				ESP_LOGW(TAG, "ble send: %s", esp_err_to_name(err));
			}
		}
	}
}

/* ----------------------------------------------------------------- console */

static void help(void)
{
	ESP_LOGI(TAG, "commands:");
	ESP_LOGI(TAG, "  h            this help");
	ESP_LOGI(TAG, "  i            status (usb, reports, ble, vendor writes)");
	ESP_LOGI(TAG, "  d            dump the HID report descriptor again");
	ESP_LOGI(TAG, "  x            toggle raw report printing");
	ESP_LOGI(TAG, "  w            toggle decoded state printing");
	ESP_LOGI(TAG, "  range <deg>  send the wheel's range command (40..900)");
}

static void status(void)
{
	const struct dfgt_usb_info *u = dfgt_usb_info();

	ESP_LOGI(TAG, "usb: %s%s speed=%u vid:pid=%04x:%04x bcdDevice=0x%04x "
		 "bcdUSB=0x%04x iface=%u ep_in=0x%02x ep_out=0x%02x",
		 u->attached ? "attached" : "waiting for the wheel",
		 u->claimed ? ", claimed" : "", u->speed, u->vid, u->pid,
		 u->bcd_device, u->bcd_usb, u->iface, u->ep_in, u->ep_out);
	ESP_LOGI(TAG, "reports=%u changes=%u bad_len=%u suppressed=%u "
		 "vendor_writes=%u ok/%u fail",
		 u->reports, g.changes, u->bad_len, g.suppressed, u->outs_ok, u->outs_fail);
	ESP_LOGI(TAG, "ble: %s (%04x:%04x, \"%s\")",
		 ble_gamepad_state_str(), DFGT_BLE_VID, DFGT_BLE_PID, DFGT_BLE_NAME);
}

static void handle(const char *cmd)
{
	int deg;
	const struct dfgt_usb_info *u = dfgt_usb_info();

	if (cmd[0] == 'h') {
		help();
	} else if (cmd[0] == 'i') {
		status();
	} else if (cmd[0] == 'd') {
		dfgt_usb_dump();
	} else if (cmd[0] == 'x') {
		g.show_raw = !g.show_raw;
		ESP_LOGI(TAG, "raw printing %s", g.show_raw ? "on" : "off");
	} else if (cmd[0] == 'w') {
		g.show_state = !g.show_state;
		ESP_LOGI(TAG, "state printing %s", g.show_state ? "on" : "off");
	} else if (sscanf(cmd, "range %d", &deg) == 1) {
		uint8_t c[DFGT_CMD_LEN];
		dfgt_cmd_range((unsigned)deg, c);
		if (!u->claimed) {
			ESP_LOGW(TAG, "no wheel claimed yet");
			return;
		}
		if (dfgt_usb_send_vendor(c, sizeof(c)) == ESP_OK) {
			ESP_LOGI(TAG, "range %d applied", deg);
		} else {
			ESP_LOGW(TAG, "range command failed");
		}
	} else if (cmd[0]) {
		ESP_LOGW(TAG, "? \"%s\" — try h", cmd);
	}
}

static void console_task(void *arg)
{
	char line[64];
	size_t n = 0;
	uint32_t last_summary = 0;
	uint32_t last_reports = 0;

	/* The IDF console already wrote to UART0 before app_main; installing the
	 * driver here is what makes reading possible (the log output keeps working). */
	esp_err_t err = uart_driver_install(UART_NUM_0, 1024, 0, 0, NULL, 0);
	if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
		ESP_LOGW(TAG, "uart_driver_install: %s", esp_err_to_name(err));
	}

	for (;;) {
		uint8_t c;
		int r = uart_read_bytes(UART_NUM_0, &c, 1, pdMS_TO_TICKS(200));

		if (r == 1) {
			if (c == '\r' || c == '\n') {
				if (n) {
					line[n] = '\0';
					handle(line);
					n = 0;
				}
			} else if (n < sizeof(line) - 1) {
				line[n++] = (char)c;
			}
		}

		uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
		const struct dfgt_usb_info *u = dfgt_usb_info();
		if (now - last_summary >= SUMMARY_MS && u->reports != last_reports) {
			last_summary = now;
			last_reports = u->reports;
			ESP_LOGI(TAG, "summary: %u reports, %u changes, ble %s",
				 u->reports, g.changes, ble_gamepad_state_str());
		}
	}
}

/* -------------------------------------------------------------------- main */

void app_main(void)
{
	esp_err_t err = nvs_flash_init();

	if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
		ESP_ERROR_CHECK(nvs_flash_erase());
		err = nvs_flash_init();
	}
	ESP_ERROR_CHECK(err);

	g.show_raw = true;
	g.show_state = true;
	g.pad_q = xQueueCreate(1, sizeof(struct pad_state));
	ESP_ERROR_CHECK(g.pad_q ? ESP_OK : ESP_ERR_NO_MEM);

	ESP_LOGI(TAG, "dfgt-ble-host (stage 1): DFGT -> BLE gamepad");
	ESP_LOGI(TAG, "wheel goes on the native USB port; that port must be fed 5 V VBUS");
	ESP_LOGI(TAG, "pair \"%s\" in macOS Bluetooth settings while the wheel is unplugged",
		 DFGT_BLE_NAME);

	/* BLE first: the Mac can pair and the console stays useful even if no wheel
	 * is attached yet. */
	ESP_ERROR_CHECK(ble_gamepad_start());
	ESP_ERROR_CHECK(dfgt_usb_start(on_report, NULL));

	xTaskCreate(ble_task, "ble_tx", 4096, NULL, 5, NULL);
	xTaskCreate(console_task, "console", 4096, NULL, 3, NULL);
	help();
}
