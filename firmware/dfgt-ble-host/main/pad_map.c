#include <string.h>

#include "pad_map.h"

/* Wheel button -> gamepad button.
 *
 * Position-mapped, not label-mapped: the wheel's four face buttons become pad
 * 1..4 in the same physical order, and each shoulder pair keeps its side
 * (r1 -> right bumper, l1 -> left bumper) even though the wheel numbers the
 * right one first. The paddles get the four spare slots (13-16) that a
 * d-pad-as-buttons layout would otherwise take; the d-pad is a hat here. */
static const uint8_t k_map[21] = {
	1,  2,  3,  4,		/* 1-4   x, square, circle, triangle */
	6,  5,  8,  7,		/* 5-8   r1, l1, r2, l2 */
	9,  10, 12, 11,		/* 9-12  select, start, r3, l3 */
	13, 14, 15,		/* 13-15 shift-up, shift-down, dial-press */
	0,  0,  0,		/* 16-18 rocker-plus, dial-cw, dial-ccw (unpublished) */
	0,  16, 0,		/* 19-21 rocker-minus, horn, ps */
};

unsigned pad_map_button(unsigned wheel_button)
{
	if (wheel_button < 1 || wheel_button > 21) {
		return 0;
	}
	return k_map[wheel_button - 1];
}

void pad_map_from_wheel(const struct dfgt_state *w, struct pad_state *p)
{
	uint16_t buttons = 0;

	memset(p, 0, sizeof(*p));

	for (unsigned i = 0; i < 21; i++) {
		if (!(w->buttons & (1u << i))) {
			continue;
		}
		unsigned pad = k_map[i];
		if (pad) {
			buttons |= (uint16_t)(1u << (pad - 1));
		}
	}

	p->buttons = buttons;
	p->x = (uint16_t)(((uint32_t)w->steer * 65535u) / 16383u);
	p->y = 32768;				/* reserved, centred */
	p->z = (uint8_t)(255 - w->throttle);	/* wheel: 255 = released */
	p->rz = (uint8_t)(255 - w->brake);
	p->hat = (uint8_t)(w->hat & 0x0f);	/* same 0..7/8 encoding */
}
