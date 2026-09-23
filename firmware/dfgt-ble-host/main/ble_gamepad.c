/* BLE HID gamepad on top of esp_hid (IDF 4.4: Bluedroid, BLE only).
 *
 * Report map: 16 buttons, X/Y (16-bit), Z/Rz (8-bit), hat switch.
 *   buttons 16 bits | X 16 | Y 16 | Z 8 | Rz 8 | hat 4 + 4 pad  = 72 bits = 9 B
 * The descriptor is the single source of truth for that layout; PAD_REPORT_LEN
 * and pack_report() below must agree with it, and the static assert checks the
 * arithmetic rather than trusting a comment.
 */
#include <inttypes.h>
#include <string.h>

#include "esp_assert.h"
#include "esp_bt.h"
#include "esp_check.h"
#include "esp_bt_main.h"
#include "esp_gap_ble_api.h"
#include "esp_gatts_api.h"
#include "esp_hid_common.h"
#include "esp_hidd.h"
#include "esp_hidd_gatts.h"
#include "esp_log.h"

#include "ble_gamepad.h"

#define TAG "ble-pad"

/* 0 = Just Works + bonding (no prompt on the Mac).
 * 1 = passkey pairing; macOS then asks you to confirm a code. Kept as a knob
 * because a host that insists on MITM is exactly the kind of thing that only
 * shows up on real hardware. */
#define DFGT_BLE_MITM		0
#define DFGT_BLE_PASSKEY	1234

#define PAD_REPORT_LEN		9

static const uint8_t k_report_map[] = {
	0x05, 0x01,		/* Usage Page (Generic Desktop) */
	0x09, 0x05,		/* Usage (Game Pad) */
	0xA1, 0x01,		/* Collection (Application) */
	0x05, 0x09,		/*   Usage Page (Button) */
	0x19, 0x01,		/*   Usage Minimum (1) */
	0x29, 0x10,		/*   Usage Maximum (16) */
	0x15, 0x00,		/*   Logical Minimum (0) */
	0x25, 0x01,		/*   Logical Maximum (1) */
	0x75, 0x01,		/*   Report Size (1) */
	0x95, 0x10,		/*   Report Count (16) */
	0x81, 0x02,		/*   Input (Data,Var,Abs) */
	0x05, 0x01,		/*   Usage Page (Generic Desktop) */
	0x09, 0x30,		/*   Usage (X) — steering */
	0x09, 0x31,		/*   Usage (Y) — reserved */
	0x16, 0x00, 0x00,	/*   Logical Minimum (0) */
	0x26, 0xFF, 0xFF,	/*   Logical Maximum (65535) */
	0x75, 0x10,		/*   Report Size (16) */
	0x95, 0x02,		/*   Report Count (2) */
	0x81, 0x02,		/*   Input (Data,Var,Abs) */
	0x09, 0x32,		/*   Usage (Z) — throttle */
	0x09, 0x35,		/*   Usage (Rz) — brake */
	0x15, 0x00,		/*   Logical Minimum (0) */
	0x26, 0xFF, 0x00,	/*   Logical Maximum (255) */
	0x75, 0x08,		/*   Report Size (8) */
	0x95, 0x02,		/*   Report Count (2) */
	0x81, 0x02,		/*   Input (Data,Var,Abs) */
	0x09, 0x39,		/*   Usage (Hat switch) */
	0x15, 0x00,		/*   Logical Minimum (0) */
	0x25, 0x07,		/*   Logical Maximum (7) */
	0x35, 0x00,		/*   Physical Minimum (0) */
	0x46, 0x3B, 0x01,	/*   Physical Maximum (315) */
	0x65, 0x14,		/*   Unit (degrees) */
	0x75, 0x04,		/*   Report Size (4) */
	0x95, 0x01,		/*   Report Count (1) */
	0x81, 0x42,		/*   Input (Data,Var,Abs,Null State) */
	0x65, 0x00,		/*   Unit (None) */
	0x75, 0x04,		/*   Report Size (4) — padding to a byte */
	0x95, 0x01,		/*   Report Count (1) */
	0x81, 0x03,		/*   Input (Const,Var,Abs) */
	0xC0,			/* End Collection */
};

/* Descriptor payload is fixed at 83 bytes; the report it declares is
 * 16 + 16 + 16 + 8 + 8 + 4 + 4 bits = 72 bits = 9 bytes (PAD_REPORT_LEN).
 * Both numbers are asserted so that editing one without re-checking the other
 * breaks the build instead of quietly sending wrongly framed reports. */
ESP_STATIC_ASSERT(sizeof(k_report_map) == 83, "report map changed: re-check PAD_REPORT_LEN");
ESP_STATIC_ASSERT(PAD_REPORT_LEN == 9, "PAD_REPORT_LEN must match the 72-bit report map");

/* Non-const because esp_hid_device_config_t::report_maps is a plain pointer. */
static esp_hid_raw_report_map_t k_report_maps[] = {
	{ .data = k_report_map, .len = sizeof(k_report_map) },
};

static const esp_hid_device_config_t k_hid_config = {
	.vendor_id		= DFGT_BLE_VID,
	.product_id		= DFGT_BLE_PID,
	.version		= DFGT_BLE_VERSION,
	.device_name		= DFGT_BLE_NAME,
	.manufacturer_name	= "dfgt-ble-host",
	.serial_number		= "stage1",
	.report_maps		= k_report_maps,
	.report_maps_len	= 1,
};

static struct {
	esp_hidd_dev_t	*dev;
	bool		 connected;
	bool		 have_last;
	uint8_t		 last[PAD_REPORT_LEN];
} s;

/* --------------------------------------------------------------------- GAP */

static void start_advertising(void)
{
	/* Same parameters the IDF esp_hid example uses: connectable, fast enough
	 * for a host to find us, and it keeps the appearance we advertise. */
	static esp_ble_adv_params_t params = {
		.adv_int_min	   = 0x20,
		.adv_int_max	   = 0x30,
		.adv_type	   = ADV_TYPE_IND,
		.own_addr_type	   = BLE_ADDR_TYPE_PUBLIC,
		.channel_map	   = ADV_CHNL_ALL,
		.adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
	};
	esp_err_t err = esp_ble_gap_start_advertising(&params);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "start_advertising: %s", esp_err_to_name(err));
	}
}

static void gap_event_cb(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param)
{
	switch (event) {
	case ESP_GAP_BLE_ADV_START_COMPLETE_EVT:
		ESP_LOGI(TAG, "advertising as \"%s\" (%04x:%04x), appearance 0x%04x",
			 DFGT_BLE_NAME, DFGT_BLE_VID, DFGT_BLE_PID,
			 ESP_HID_APPEARANCE_GAMEPAD);
		break;
	case ESP_GAP_BLE_ADV_STOP_COMPLETE_EVT:
		ESP_LOGI(TAG, "advertising stopped");
		break;
	case ESP_GAP_BLE_AUTH_CMPL_EVT:
		ESP_LOGI(TAG, "pairing: %s (bonded=%d, encrypt=%d)",
			 param->ble_security.auth_cmpl.success ? "ok" : "failed",
			 param->ble_security.auth_cmpl.dev_type,
			 param->ble_security.auth_cmpl.auth_mode);
		break;
	case ESP_GAP_BLE_PASSKEY_NOTIF_EVT:
		ESP_LOGI(TAG, "passkey to confirm on the Mac: %06" PRIu32,
			 param->ble_security.key_notif.passkey);
		break;
	default:
		break;
	}
}

static esp_err_t gap_init(void)
{
	esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
	ESP_RETURN_ON_ERROR(esp_bt_controller_init(&bt_cfg), TAG, "controller_init");
	ESP_RETURN_ON_ERROR(esp_bt_controller_enable(ESP_BT_MODE_BLE), TAG, "controller_enable");
	ESP_RETURN_ON_ERROR(esp_bluedroid_init(), TAG, "bluedroid_init");
	ESP_RETURN_ON_ERROR(esp_bluedroid_enable(), TAG, "bluedroid_enable");

	ESP_RETURN_ON_ERROR(esp_ble_gap_register_callback(gap_event_cb), TAG, "gap_cb");
	ESP_RETURN_ON_ERROR(esp_ble_gatts_register_callback(esp_hidd_gatts_event_handler),
			    TAG, "gatts_cb");

#if DFGT_BLE_MITM
	esp_ble_auth_req_t auth_req = ESP_LE_AUTH_REQ_SC_MITM_BOND;
	esp_ble_io_cap_t iocap = ESP_IO_CAP_IO;
#else
	esp_ble_auth_req_t auth_req = ESP_LE_AUTH_REQ_SC_BOND;
	esp_ble_io_cap_t iocap = ESP_IO_CAP_NONE;
#endif
	uint8_t init_key = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
	uint8_t rsp_key = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
	uint8_t key_size = 16;

	ESP_RETURN_ON_ERROR(esp_ble_gap_set_security_param(ESP_BLE_SM_AUTHEN_REQ_MODE, &auth_req, 1),
			    TAG, "auth_req");
	ESP_RETURN_ON_ERROR(esp_ble_gap_set_security_param(ESP_BLE_SM_IOCAP_MODE, &iocap, 1),
			    TAG, "iocap");
	ESP_RETURN_ON_ERROR(esp_ble_gap_set_security_param(ESP_BLE_SM_SET_INIT_KEY, &init_key, 1),
			    TAG, "init_key");
	ESP_RETURN_ON_ERROR(esp_ble_gap_set_security_param(ESP_BLE_SM_SET_RSP_KEY, &rsp_key, 1),
			    TAG, "rsp_key");
	ESP_RETURN_ON_ERROR(esp_ble_gap_set_security_param(ESP_BLE_SM_MAX_KEY_SIZE, &key_size, 1),
			    TAG, "key_size");
#if DFGT_BLE_MITM
	uint32_t passkey = DFGT_BLE_PASSKEY;
	ESP_RETURN_ON_ERROR(esp_ble_gap_set_security_param(ESP_BLE_SM_SET_STATIC_PASSKEY, &passkey,
							   sizeof(passkey)), TAG, "passkey");
#endif

	/* Advertising data: the HID service (0x1812) plus name, appearance and TX
	 * power. The Mac filters on the HID service UUID, so it must be here. */
	const uint8_t hid_service_uuid128[] = {
		0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80,
		0x00, 0x10, 0x00, 0x00, 0x12, 0x18, 0x00, 0x00,
	};
	esp_ble_adv_data_t adv = {
		.set_scan_rsp	    = false,
		.include_name	    = true,
		.include_txpower    = true,
		.min_interval	    = 0x0006,
		.max_interval	    = 0x0010,
		.appearance	    = ESP_HID_APPEARANCE_GAMEPAD,
		.service_uuid_len   = sizeof(hid_service_uuid128),
		.p_service_uuid	    = (uint8_t *)hid_service_uuid128,
		.flag		    = 0x06,
	};
	ESP_RETURN_ON_ERROR(esp_ble_gap_set_device_name(DFGT_BLE_NAME), TAG, "device_name");
	ESP_RETURN_ON_ERROR(esp_ble_gap_config_adv_data(&adv), TAG, "adv_data");
	return ESP_OK;
}

/* --------------------------------------------------------------- esp_hid */

static void hidd_event_cb(void *handler_args, esp_event_base_t base, int32_t id,
			  void *event_data)
{
	esp_hidd_event_t event = (esp_hidd_event_t)id;
	esp_hidd_event_data_t *param = (esp_hidd_event_data_t *)event_data;

	switch (event) {
	case ESP_HIDD_START_EVENT:
		ESP_LOGI(TAG, "HID device ready");
		start_advertising();
		break;
	case ESP_HIDD_CONNECT_EVENT:
		s.connected = true;
		ESP_LOGI(TAG, "connected to a host");
		break;
	case ESP_HIDD_DISCONNECT_EVENT:
		s.connected = false;
		s.have_last = false;
		ESP_LOGW(TAG, "disconnected — advertising again");
		/* esp_hid does not restart advertising by itself. */
		start_advertising();
		break;
	case ESP_HIDD_OUTPUT_EVENT:
		/* Evidence, not features: if the Mac ever writes to a gamepad, this
		 * is where we find out (and Stage 2's FFB channel would land here). */
		ESP_LOGI(TAG, "host wrote output report id %u len %u",
			 param->output.report_id, param->output.length);
		ESP_LOG_BUFFER_HEX_LEVEL(TAG, param->output.data, param->output.length,
					 ESP_LOG_INFO);
		break;
	case ESP_HIDD_FEATURE_EVENT:
		ESP_LOGI(TAG, "host wrote feature report id %u len %u",
			 param->feature.report_id, param->feature.length);
		break;
	case ESP_HIDD_PROTOCOL_MODE_EVENT:
		ESP_LOGI(TAG, "protocol mode -> %s", param->protocol_mode.protocol_mode ?
			 "report" : "boot");
		break;
	case ESP_HIDD_CONTROL_EVENT:
		ESP_LOGI(TAG, "control: %s", param->control.control ? "exit suspend" :
			 "suspend");
		break;
	default:
		break;
	}
}

static esp_err_t pack_report(const struct pad_state *p, uint8_t *buf)
{
	/* Order must match k_report_map: buttons, X, Y, Z, Rz, hat. */
	buf[0] = (uint8_t)(p->buttons & 0xff);
	buf[1] = (uint8_t)(p->buttons >> 8);
	buf[2] = (uint8_t)(p->x & 0xff);
	buf[3] = (uint8_t)(p->x >> 8);
	buf[4] = (uint8_t)(p->y & 0xff);
	buf[5] = (uint8_t)(p->y >> 8);
	buf[6] = p->z;
	buf[7] = p->rz;
	buf[8] = (uint8_t)(p->hat & 0x0f);
	return ESP_OK;
}

/* ------------------------------------------------------------------ public */

esp_err_t ble_gamepad_start(void)
{
	ESP_RETURN_ON_ERROR(gap_init(), TAG, "gap_init");
	ESP_RETURN_ON_ERROR(esp_hidd_dev_init(&k_hid_config, ESP_HID_TRANSPORT_BLE,
					      hidd_event_cb, &s.dev),
			    TAG, "esp_hidd_dev_init");
	ESP_LOGI(TAG, "input reports are %d bytes (buttons, X, Y, Z, Rz, hat)",
		 PAD_REPORT_LEN);
	return ESP_OK;
}

bool ble_gamepad_connected(void)
{
	return s.connected;
}

const char *ble_gamepad_state_str(void)
{
	if (!s.dev) {
		return "down";
	}
	return s.connected ? "connected" : "advertising";
}

esp_err_t ble_gamepad_send(const struct pad_state *p)
{
	uint8_t buf[PAD_REPORT_LEN];

	if (!s.dev || !s.connected) {
		return ESP_ERR_INVALID_STATE;
	}
	pack_report(p, buf);
	if (s.have_last && memcmp(buf, s.last, sizeof(buf)) == 0) {
		return ESP_OK;	/* nothing changed: no airtime spent */
	}
	memcpy(s.last, buf, sizeof(buf));
	s.have_last = true;
	return esp_hidd_dev_input_set(s.dev, 0, 0, buf, sizeof(buf));
}
