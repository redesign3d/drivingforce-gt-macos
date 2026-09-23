/* Driving Force GT: input report decode + vendor command bytes.
 *
 * The layout here is a *hypothesis* carried over from the macOS driver
 * (PLAN.md §3), where it was confirmed against two captures
 * (tests/capture-controls.hex, tests/capture-turn-replug.hex). Stage 1 exists to
 * re-confirm it from the ESP32's own USB host stack: the firmware logs the raw
 * report next to the decoded one, so a wrong assumption is visible, not hidden.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DFGT_REPORT_LEN 8
#define DFGT_CMD_LEN    7

struct dfgt_state {
	bool	 valid;
	uint8_t	 raw[DFGT_REPORT_LEN];
	unsigned hat;		/* 0..7 direction, 8 = centred (null) */
	uint32_t buttons;	/* 21-bit mask; bit (n-1) = button n (1-based) */
	unsigned vendor7;	/* raw[3][7:1] — device state, meaning partly unknown */
	unsigned vendor2;	/* raw[5][7:6] — horn duplicate */
	unsigned steer;		/* 0..16383, 0 = full left, centre ~8192 */
	unsigned throttle;	/* raw, 255 = released */
	unsigned brake;		/* raw, 255 = released */
	uint32_t t_ms;
};

/* Returns false (and leaves *s alone) if the report is not DFGT_REPORT_LEN long,
 * so unexpected report lengths get logged instead of silently mis-decoded. */
bool dfgt_decode(const uint8_t *r, size_t len, struct dfgt_state *s);

const char *dfgt_hat_name(unsigned hat);
const char *dfgt_button_name(unsigned bit);	/* bit = 0..20 */
void dfgt_pressed_list(uint32_t buttons, char *out, size_t n);
void dfgt_state_line(const struct dfgt_state *s, char *out, size_t n);

/* 7-byte vendor output report on the wheel's FFB endpoint.
 *
 * Stage 1 sends range only — it is a configuration write, not force feedback,
 * and it matters because the wheel reverts to ~200° on every replug (PLAN.md §3
 * fact 9), which makes steering twitchy. Bytes are copied verbatim from
 * src/dfgt.c:cmd_range() (verified on hardware there, PLAN §6 A2). Force and
 * autocenter commands are deliberately absent until Stage 2. */
void dfgt_cmd_range(unsigned deg, uint8_t c[DFGT_CMD_LEN]);
