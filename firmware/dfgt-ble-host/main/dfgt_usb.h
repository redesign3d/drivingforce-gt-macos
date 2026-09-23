/* ESP32-S3 USB *host* for the Driving Force GT (046d:c29a).
 *
 * Discovery-first: every attach dumps the device descriptor, the config
 * descriptor, all string descriptors, the interface/endpoint list and the HID
 * report descriptor, then streams interrupt-IN reports to the caller. Nothing in
 * this file assumes the 8-byte layout — that lives in dfgt_decode.c, and the log
 * shows raw and decoded side by side so the assumption can be checked against
 * what this hardware actually does.
 *
 * Hardware note: the S3 has one USB peripheral, so the native port must be a
 * *host* here — VBUS (5 V) has to be fed to the wheel by hand, see README.md.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef void (*dfgt_report_cb_t)(const uint8_t *data, size_t len, void *arg);

struct dfgt_usb_info {
	bool	 attached;	/* device enumerated and opened */
	bool	 claimed;	/* HID interface claimed, reports flowing */
	uint8_t	 speed;		/* usb_speed_t */
	uint16_t vid;
	uint16_t pid;
	uint16_t bcd_device;
	uint16_t bcd_usb;
	uint8_t	 iface;
	uint8_t	 ep_in;
	uint8_t	 ep_out;
	uint32_t reports;	/* interrupt-IN reports received */
	uint32_t bad_len;	/* reports that were not DFGT_REPORT_LEN bytes */
	uint32_t outs_ok;	/* vendor writes that completed */
	uint32_t outs_fail;
};

/* Installs the host library, registers one client and starts its task.
 * cb() is called from the USB client task for every interrupt-IN report. */
esp_err_t dfgt_usb_start(dfgt_report_cb_t cb, void *arg);

const struct dfgt_usb_info *dfgt_usb_info(void);

/* Re-print the descriptors captured at attach (console command `d`). */
void dfgt_usb_dump(void);

/* One 7-byte vendor output report (the wheel's FFB endpoint). Stage 1 uses this
 * for the range command only. Blocks until the transfer completes or times out. */
esp_err_t dfgt_usb_send_vendor(const uint8_t *cmd, size_t len);
