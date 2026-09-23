#include <stdio.h>
#include <string.h>

#include "dfgt_decode.h"

/* Control names, verified on macOS by pressing every control one at a time
 * (PLAN.md §5 M1). Index = buttons bit. Stage 1 logs these names so the same
 * identification can be repeated on this hardware without another capture. */
static const char *const k_button_names[21] = {
	"x", "square", "circle", "triangle",		/* 1-4   face buttons */
	"r1", "l1", "r2", "l2",				/* 5-8   shoulders (right first) */
	"select", "start", "r3", "l3",			/* 9-12 */
	"shift-up", "shift-down", "dial-press",		/* 13-15 */
	"rocker-plus", "dial-cw", "dial-ccw",		/* 16-18 */
	"rocker-minus", "horn", "ps",			/* 19-21 */
};

/* hat switch: 0=N, 2=E, 4=S, 6=W, odd = diagonals, 8 = centred */
static const char *const k_hat_names[9] = {
	"up", "up-right", "right", "down-right",
	"down", "down-left", "left", "up-left", "centred",
};

bool dfgt_decode(const uint8_t *r, size_t len, struct dfgt_state *s)
{
	if (len != DFGT_REPORT_LEN) {
		return false;
	}
	s->hat      = r[0] & 0x0f;
	s->buttons  = ((uint32_t)(r[0] >> 4) | ((uint32_t)r[1] << 4) |
		       ((uint32_t)r[2] << 12) | ((uint32_t)(r[3] & 1) << 20)) & 0x1fffff;
	s->vendor7  = (r[3] >> 1) & 0x7f;
	s->steer    = (unsigned)r[4] | ((unsigned)(r[5] & 0x3f) << 8);
	s->vendor2  = (r[5] >> 6) & 0x03;
	s->throttle = r[6];
	s->brake    = r[7];
	memcpy(s->raw, r, DFGT_REPORT_LEN);
	s->valid = true;
	return true;
}

const char *dfgt_hat_name(unsigned hat)
{
	return hat < 9 ? k_hat_names[hat] : "?";
}

const char *dfgt_button_name(unsigned bit)
{
	return bit < 21 ? k_button_names[bit] : "?";
}

void dfgt_pressed_list(uint32_t buttons, char *out, size_t n)
{
	size_t used = 0;

	if (n == 0) {
		return;
	}
	out[0] = '\0';
	for (unsigned i = 0; i < 21 && used + 1 < n; i++) {
		if (!(buttons & (1u << i))) {
			continue;
		}
		int w = snprintf(out + used, n - used, "%s%s",
				 used ? "," : "", k_button_names[i]);
		if (w < 0) {
			break;
		}
		used += (size_t)w;
	}
	if (buttons == 0) {
		out[0] = '-';
		out[1] = '\0';
	}
}

void dfgt_state_line(const struct dfgt_state *s, char *out, size_t n)
{
	char pressed[160];

	dfgt_pressed_list(s->buttons, pressed, sizeof(pressed));
	snprintf(out, n,
		 "steer=%u throttle=%u brake=%u hat=%u(%s) buttons=0x%06x "
		 "vendor7=0x%02x vendor2=%u pressed=%s",
		 s->steer, s->throttle, s->brake, s->hat, dfgt_hat_name(s->hat),
		 (unsigned)s->buttons, s->vendor7, s->vendor2, pressed);
}

void dfgt_cmd_range(unsigned deg, uint8_t c[DFGT_CMD_LEN])
{
	if (deg < 40) {
		deg = 40;
	}
	if (deg > 900) {
		deg = 900;
	}
	memset(c, 0, DFGT_CMD_LEN);
	c[0] = 0xf8;
	c[1] = 0x81;
	c[2] = (uint8_t)(deg & 0x00ff);
	c[3] = (uint8_t)((deg & 0xff00) >> 8);
}
