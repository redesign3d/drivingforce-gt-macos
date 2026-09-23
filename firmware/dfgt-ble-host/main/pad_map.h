/* Wheel state -> gamepad state.
 *
 * This is the seam the task asked for: the USB side produces struct dfgt_state,
 * this file decides what the host sees, and ble_gamepad.c decides how it is
 * encoded. Swapping Stage 2 (G29/wheel emulation) in means replacing this file
 * and ble_gamepad.c, not touching the USB host or the decode.
 *
 * Mapped:   steering -> X, throttle -> Z, brake -> Rz, D-pad -> hat,
 *           16 of the wheel's 21 buttons -> buttons 1..16.
 * Unmapped: Y is held centred (reserved), and the wheel's dial rotation
 *           (dial-cw/dial-ccw), rocker-plus/minus and ps are decoded in
 *           dfgt_state but not published — a standard pad has no slot for them,
 *           and Stage 1 is about getting steering/pedals/buttons accepted.
 */
#pragma once

#include <stdint.h>

#include "dfgt_decode.h"

struct pad_state {
	uint16_t buttons;	/* bit (n-1) = gamepad button n, 1..16 */
	uint16_t x;		/* steering: 0 = full left .. 65535 = full right */
	uint16_t y;		/* reserved, centred */
	uint8_t	 z;		/* throttle: 0 = released .. 255 = pressed */
	uint8_t	 rz;		/* brake:    0 = released .. 255 = pressed */
	uint8_t	 hat;		/* 0..7, 8 = centred (same encoding as the wheel) */
};

void pad_map_from_wheel(const struct dfgt_state *w, struct pad_state *p);

/* Wheel button (1-based, 1..21) -> gamepad button (1..16), 0 = not published. */
unsigned pad_map_button(unsigned wheel_button);
