/* ESP32-S3 USB host for the DFGT — discovery-first.
 *
 * The log on attach is the deliverable of Stage 1's first half: device
 * descriptor, speed, string descriptors, config descriptor, endpoint list and
 * the HID report descriptor. Reports then stream in raw + decoded form.
 *
 * Two tasks are required by the IDF 4.4 host stack: one services the library
 * itself (enumeration), one is the client (our device + transfers).
 */
#include <string.h>

/* Must come first: the other FreeRTOS headers below depend on it. */
#include "freertos/FreeRTOS.h" /* IWYU pragma: keep */
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_check.h"
#include "esp_log.h"
#include "usb/usb_host.h"
#include "usb/usb_helpers.h"

#include "dfgt_decode.h"
#include "dfgt_usb.h"

#define TAG "dfgt-usb"

#define DFGT_VID		0x046d
#define DFGT_PID		0xc29a
#define HID_CLASS		0x03
#define REPORT_DESC_MAX		512
#define CLIENT_EVENT_MSG	5
#define XFER_TIMEOUT_MS		500
#define POLL_RETRY_DELAY_MS	10
#define DFGT_RANGE_DEFAULT	900	/* wheel reverts to ~200 deg on every replug */

static struct {
	usb_host_client_handle_t client;
	usb_device_handle_t	 dev;
	usb_transfer_t		*in_xfer;
	usb_transfer_t		*ctrl_xfer;
	usb_transfer_t		*out_xfer;
	SemaphoreHandle_t	 out_done;
	volatile uint8_t	 open_addr;	/* != 0: open this address next tick */
	volatile bool		 close_req;	/* DEV_GONE seen */
	bool			 in_flight;
	struct dfgt_usb_info	 info;
	dfgt_report_cb_t	 cb;
	void			*cb_arg;
	uint8_t			 report_desc[REPORT_DESC_MAX];
	size_t			 report_desc_len;
} s;

/* ------------------------------------------------------------------ helpers */

static const char *speed_name(uint8_t speed)
{
	switch (speed) {
	case USB_SPEED_LOW:  return "low";
	case USB_SPEED_FULL: return "full";
	case USB_SPEED_HIGH: return "high";
	default:	     return "?";
	}
}

static void hex_dump(const char *what, const uint8_t *b, size_t len)
{
	ESP_LOGI(TAG, "%s (%u bytes):", what, (unsigned)len);
	for (size_t i = 0; i < len; i += 16) {
		char line[3 * 16 + 1];
		size_t n = len - i > 16 ? 16 : len - i;
		for (size_t j = 0; j < n; j++) {
			snprintf(line + 3 * j, 4, "%02x ", b[i + j]);
		}
		line[3 * n] = '\0';
		ESP_LOGI(TAG, "  %04x: %s", (unsigned)i, line);
	}
}

/* --------------------------------------------------------------- transfers */

static void submit_in(void)
{
	s.in_xfer->device_handle = s.dev;
	s.in_xfer->bEndpointAddress = s.info.ep_in;
	s.in_xfer->num_bytes = DFGT_REPORT_LEN;
	s.in_flight = true;
	esp_err_t err = usb_host_transfer_submit(s.in_xfer);
	if (err != ESP_OK) {
		s.in_flight = false;
		ESP_LOGE(TAG, "interrupt-IN submit failed: %s", esp_err_to_name(err));
	}
}

static void in_cb(usb_transfer_t *xfer)
{
	s.in_flight = false;

	if (xfer->status == USB_TRANSFER_STATUS_COMPLETED) {
		s.info.reports++;
		if (xfer->actual_num_bytes != DFGT_REPORT_LEN) {
			/* The descriptor says 8 bytes; anything else is a real finding. */
			s.info.bad_len++;
			ESP_LOGW(TAG, "report length %d (expected %d)",
				 xfer->actual_num_bytes, DFGT_REPORT_LEN);
		}
		if (s.cb) {
			s.cb(xfer->data_buffer, (size_t)xfer->actual_num_bytes, s.cb_arg);
		}
	} else if (xfer->status != USB_TRANSFER_STATUS_NO_DEVICE) {
		/* A stall or error would otherwise spin at full speed. */
		ESP_LOGW(TAG, "interrupt-IN status %d, retrying", (int)xfer->status);
		vTaskDelay(pdMS_TO_TICKS(POLL_RETRY_DELAY_MS));
	}

	if (s.info.attached && !s.close_req) {
		submit_in();
	}
}

static void ctrl_cb(usb_transfer_t *xfer)
{
	if (xfer->status != USB_TRANSFER_STATUS_COMPLETED) {
		ESP_LOGE(TAG, "report descriptor request status %d", (int)xfer->status);
		return;
	}
	if (xfer->actual_num_bytes <= USB_SETUP_PACKET_SIZE) {
		ESP_LOGW(TAG, "report descriptor came back empty (%d bytes)",
			 xfer->actual_num_bytes);
		return;
	}
	s.report_desc_len = (size_t)xfer->actual_num_bytes - USB_SETUP_PACKET_SIZE;
	if (s.report_desc_len > REPORT_DESC_MAX) {
		s.report_desc_len = REPORT_DESC_MAX;
	}
	memcpy(s.report_desc, xfer->data_buffer + USB_SETUP_PACKET_SIZE, s.report_desc_len);
	hex_dump("HID report descriptor", s.report_desc, s.report_desc_len);
}

static void out_cb(usb_transfer_t *xfer)
{
	xSemaphoreGive(s.out_done);
}

/* Get the HID report descriptor: GET_DESCRIPTOR(Report, interface).
 *
 * Deliberately fire-and-forget: this runs in the USB *client* task, which is the
 * only task that dispatches transfer callbacks, so waiting for the completion
 * here would wait for itself. The result is stashed and printed by ctrl_cb. */
static void fetch_report_descriptor(void)
{
	usb_setup_packet_t setup = {
		.bmRequestType = USB_BM_REQUEST_TYPE_DIR_IN |
				 USB_BM_REQUEST_TYPE_TYPE_STANDARD |
				 USB_BM_REQUEST_TYPE_RECIP_INTERFACE,
		.bRequest = USB_B_REQUEST_GET_DESCRIPTOR,
		.wValue = 0x22 << 8,	/* HID report descriptor: no macro for it in IDF 4.4 */
		.wIndex = s.info.iface,
		.wLength = REPORT_DESC_MAX,
	};

	memcpy(s.ctrl_xfer->data_buffer, setup.val, USB_SETUP_PACKET_SIZE);
	s.ctrl_xfer->num_bytes = USB_SETUP_PACKET_SIZE + REPORT_DESC_MAX;
	s.ctrl_xfer->device_handle = s.dev;

	if (usb_host_transfer_submit_control(s.client, s.ctrl_xfer) != ESP_OK) {
		ESP_LOGE(TAG, "report descriptor request failed to submit");
	}
}

/* The HID interface carries 8B interrupt IN + 7B interrupt OUT (PLAN.md §3). */
static void find_hid_interface(const usb_config_desc_t *cfg)
{
	int offset = 0;
	const usb_intf_desc_t *intf = NULL;

	for (int i = 0; i < cfg->bNumInterfaces; i++) {
		int off = 0;
		const usb_intf_desc_t *cand =
			usb_parse_interface_descriptor(cfg, (uint8_t)i, 0, &off);
		if (cand && cand->bInterfaceClass == HID_CLASS) {
			intf = cand;
			offset = off;
			break;
		}
	}
	if (!intf) {
		ESP_LOGE(TAG, "no HID interface in the config descriptor");
		return;
	}

	s.info.iface = intf->bInterfaceNumber;
	ESP_LOGI(TAG, "HID interface %u: %u endpoint(s), subclass %u protocol %u",
		 intf->bInterfaceNumber, intf->bNumEndpoints,
		 intf->bInterfaceSubClass, intf->bInterfaceProtocol);

	for (int i = 0; i < intf->bNumEndpoints; i++) {
		int off = offset;
		const usb_ep_desc_t *ep =
			usb_parse_endpoint_descriptor_by_index(intf, i, cfg->wTotalLength, &off);
		if (!ep) {
			continue;
		}
		bool is_int = (ep->bmAttributes & 0x03) == 0x03;
		bool is_in = (ep->bEndpointAddress & 0x80) != 0;
		ESP_LOGI(TAG, "  ep 0x%02x: %s interrupt=%d mps=%u interval=%u",
			 ep->bEndpointAddress, is_in ? "IN " : "OUT", is_int,
			 ep->wMaxPacketSize, ep->bInterval);
		if (!is_int) {
			continue;
		}
		if (is_in) {
			s.info.ep_in = ep->bEndpointAddress;
		} else {
			s.info.ep_out = ep->bEndpointAddress;
		}
	}
	if (!s.info.ep_in) {
		ESP_LOGE(TAG, "no interrupt IN endpoint — cannot read the wheel");
	}
}

/* ------------------------------------------------------------------- attach */

static void attach(void)
{
	uint8_t addr = s.open_addr;
	s.open_addr = 0;

	ESP_LOGI(TAG, "=== attach: address %u ===", addr);
	esp_err_t err = usb_host_device_open(s.client, addr, &s.dev);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "device_open: %s", esp_err_to_name(err));
		return;
	}

	usb_device_info_t di;
	if (usb_host_device_info(s.dev, &di) == ESP_OK) {
		s.info.speed = di.speed;
		ESP_LOGI(TAG, "speed %s, bConfigurationValue %u",
			 speed_name(di.speed), di.bConfigurationValue);
		/* The wheel's own strings are part of the evidence. */
		ESP_LOGI(TAG, "manufacturer:");
		usb_print_string_descriptor(di.str_desc_manufacturer);
		ESP_LOGI(TAG, "product:");
		usb_print_string_descriptor(di.str_desc_product);
		ESP_LOGI(TAG, "serial:");
		usb_print_string_descriptor(di.str_desc_serial_num);
	}

	const usb_device_desc_t *dd = NULL;
	if (usb_host_get_device_descriptor(s.dev, &dd) == ESP_OK && dd) {
		usb_print_device_descriptor(dd);
		s.info.vid = dd->idVendor;
		s.info.pid = dd->idProduct;
		s.info.bcd_device = dd->bcdDevice;
		s.info.bcd_usb = dd->bcdUSB;
		ESP_LOGI(TAG, "idVendor 0x%04x idProduct 0x%04x bcdDevice 0x%04x bcdUSB 0x%04x",
			 dd->idVendor, dd->idProduct, dd->bcdDevice, dd->bcdUSB);
		if (dd->idVendor != DFGT_VID || dd->idProduct != DFGT_PID) {
			ESP_LOGW(TAG, "not a Driving Force GT (expected %04x:%04x) — continuing",
				 DFGT_VID, DFGT_PID);
		}
	}

	const usb_config_desc_t *cfg = NULL;
	if (usb_host_get_active_config_descriptor(s.dev, &cfg) != ESP_OK || !cfg) {
		ESP_LOGE(TAG, "no active config descriptor");
		goto fail;
	}
	usb_print_config_descriptor(cfg, NULL);
	find_hid_interface(cfg);
	s.info.attached = true;

	if (!s.info.ep_in) {
		goto fail;
	}

	err = usb_host_interface_claim(s.client, s.dev, s.info.iface, 0);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "interface_claim: %s", esp_err_to_name(err));
		goto fail;
	}
	s.info.claimed = true;

	fetch_report_descriptor();

	/* No vendor writes from here: the range command waits on a transfer callback
	 * that only this task can dispatch. app_main applies it once `claimed` shows
	 * the wheel is up (see app_main.c). */
	s.in_xfer->device_handle = s.dev;
	s.in_xfer->num_bytes = DFGT_REPORT_LEN;
	s.in_xfer->bEndpointAddress = s.info.ep_in;
	submit_in();
	ESP_LOGI(TAG, "polling interrupt IN 0x%02x; wheel reports start now",
		 s.info.ep_in);
	return;

fail:
	ESP_LOGE(TAG, "attach incomplete");
	s.close_req = true;
}

static void detach(void)
{
	s.close_req = false;
	if (s.info.claimed) {
		usb_host_interface_release(s.client, s.dev, s.info.iface);
		s.info.claimed = false;
	}
	if (s.dev) {
		usb_host_device_close(s.client, s.dev);
		s.dev = NULL;
	}
	s.info.attached = false;
	s.report_desc_len = 0;
	ESP_LOGW(TAG, "device gone — waiting for a replug");
}

/* -------------------------------------------------------------------- tasks */

static void client_event_cb(const usb_host_client_event_msg_t *msg, void *arg)
{
	switch (msg->event) {
	case USB_HOST_CLIENT_EVENT_NEW_DEV:
		if (!s.info.attached) {
			s.open_addr = msg->new_dev.address;
		}
		break;
	case USB_HOST_CLIENT_EVENT_DEV_GONE:
		s.close_req = true;
		break;
	default:
		break;
	}
}

static void usb_lib_task(void *arg)
{
	uint32_t flags;
	for (;;) {
		usb_host_lib_handle_events(portMAX_DELAY, &flags);
		if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
			ESP_LOGW(TAG, "host library has no clients");
		}
		if (flags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE) {
			ESP_LOGW(TAG, "host library freed all devices");
		}
	}
}

static void usb_client_task(void *arg)
{
	for (;;) {
		usb_host_client_handle_events(s.client, pdMS_TO_TICKS(100));
		if (s.open_addr) {
			attach();
		}
		if (s.close_req && !s.in_flight) {
			detach();
		}
	}
}

/* -------------------------------------------------------------------- public */

esp_err_t dfgt_usb_start(dfgt_report_cb_t cb, void *arg)
{
	s.cb = cb;
	s.cb_arg = arg;
	s.out_done = xSemaphoreCreateBinary();
	if (!s.out_done) {
		return ESP_ERR_NO_MEM;
	}

	usb_host_config_t host_cfg = {
		.skip_phy_setup = false,
		.intr_flags = ESP_INTR_FLAG_LEVEL1,
	};
	ESP_RETURN_ON_ERROR(usb_host_install(&host_cfg), TAG, "usb_host_install");

	usb_host_client_config_t client_cfg = {
		.is_synchronous = false,
		.max_num_event_msg = CLIENT_EVENT_MSG,
		.async = {
			.client_event_callback = client_event_cb,
			.callback_arg = NULL,
		},
	};
	ESP_RETURN_ON_ERROR(usb_host_client_register(&client_cfg, &s.client), TAG,
			    "usb_host_client_register");

	ESP_RETURN_ON_ERROR(usb_host_transfer_alloc(DFGT_CMD_LEN, 0, &s.out_xfer), TAG,
			    "out xfer");
	s.out_xfer->callback = out_cb;
	ESP_RETURN_ON_ERROR(usb_host_transfer_alloc(USB_SETUP_PACKET_SIZE + REPORT_DESC_MAX, 0,
						   &s.ctrl_xfer), TAG, "ctrl xfer");
	s.ctrl_xfer->callback = ctrl_cb;
	ESP_RETURN_ON_ERROR(usb_host_transfer_alloc(DFGT_REPORT_LEN, 0, &s.in_xfer), TAG,
			    "in xfer");
	s.in_xfer->callback = in_cb;

	if (xTaskCreate(usb_lib_task, "dfgt_usb_lib", 4096, NULL, 4, NULL) != pdPASS ||
	    xTaskCreate(usb_client_task, "dfgt_usb", 4096, NULL, 5, NULL) != pdPASS) {
		return ESP_ERR_NO_MEM;
	}
	ESP_LOGI(TAG, "USB host up — plug the wheel into the port the host feeds");
	return ESP_OK;
}

const struct dfgt_usb_info *dfgt_usb_info(void)
{
	return &s.info;
}

void dfgt_usb_dump(void)
{
	if (s.report_desc_len) {
		hex_dump("HID report descriptor", s.report_desc, s.report_desc_len);
	} else {
		ESP_LOGW(TAG, "no report descriptor captured yet");
	}
}

esp_err_t dfgt_usb_send_vendor(const uint8_t *cmd, size_t len)
{
	if (!s.info.claimed || !s.info.ep_out) {
		return ESP_ERR_INVALID_STATE;
	}
	if (len > DFGT_CMD_LEN) {
		return ESP_ERR_INVALID_ARG;
	}
	memcpy(s.out_xfer->data_buffer, cmd, len);
	s.out_xfer->num_bytes = (int)len;
	s.out_xfer->device_handle = s.dev;
	s.out_xfer->bEndpointAddress = s.info.ep_out;

	esp_err_t err = usb_host_transfer_submit(s.out_xfer);
	if (err != ESP_OK) {
		s.info.outs_fail++;
		return err;
	}
	if (xSemaphoreTake(s.out_done, pdMS_TO_TICKS(XFER_TIMEOUT_MS)) != pdTRUE) {
		s.info.outs_fail++;
		return ESP_ERR_TIMEOUT;
	}
	if (s.out_xfer->status != USB_TRANSFER_STATUS_COMPLETED) {
		s.info.outs_fail++;
		ESP_LOGW(TAG, "vendor OUT status %d", (int)s.out_xfer->status);
		return ESP_FAIL;
	}
	s.info.outs_ok++;
	return ESP_OK;
}
