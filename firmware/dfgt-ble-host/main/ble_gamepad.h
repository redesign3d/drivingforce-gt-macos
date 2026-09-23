/* BLE HID gamepad endpoint.
 *
 * This is the *only* file that knows how the host sees us: the report map, the
 * appearance, the identity and the encoding. Stage 2 (a wheel/G29 that macOS
 * and GeForce NOW treat as a wheel) replaces this file and pad_map.c; nothing
 * below or above it changes.
 *
 * Stage 1 presents a plain 4-axis / 16-button / hat gamepad: the whitelist route
 * (a Logitech wheel identity over USB) is not available while the S3's single
 * USB port is busy hosting the wheel, so the first question to answer is
 * whether the native client accepts a BLE peripheral at all.
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

#include "pad_map.h"

/* Identity the Mac will see. A generic identity on purpose: if the client
 * ignores it, the fix is to claim a known gamepad's ids here and re-test, which
 * is a one-line change and a re-flash (no descriptor or mapping work). */
#define DFGT_BLE_NAME		"DFGT Pad"
#define DFGT_BLE_VID		0x303A		/* Espressif; nothing claims this as a gamepad */
#define DFGT_BLE_PID		0x4001
#define DFGT_BLE_VERSION	0x0100

esp_err_t ble_gamepad_start(void);

bool ble_gamepad_connected(void);

/* Encodes and notifies, but only when connected and only when the report
 * differs from the last one sent — the wheel is change-driven, so a duplicate
 * send is pure airtime. */
esp_err_t ble_gamepad_send(const struct pad_state *p);

/* "connected"/"advertising"/"down", for the console `s` command. */
const char *ble_gamepad_state_str(void);
