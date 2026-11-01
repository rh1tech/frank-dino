/*
 * frank-dino — an RP2350B acting as the whole machine around a real Z80
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-dino
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Gamepads, whatever they are, as one pad -- and that pad as the
 * Spectrum's joystick and keys. See joymap.c.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* The pad's controls, in pico-speccy's joydef slot order. */
enum {
    PAD_LEFT = 0, PAD_RIGHT, PAD_UP, PAD_DOWN, PAD_START, PAD_SELECT,
    PAD_A, PAD_B, PAD_C, PAD_X, PAD_Y, PAD_Z, PAD_L2, PAD_R2,
    PAD_SLOTS
};
#define PAD_BIT(s) ((uint16_t)(1u << (s)))

/*
 * Where a control goes. 0 is the slot's default; then nothing, the
 * joystick's directions and fire buttons, and the forty keys from
 * JT_KEY (row * 5 + bit, the half-rows in the ULA's order).
 */
enum {
    JT_DEFAULT = 0, JT_NONE,
    JT_UP, JT_DOWN, JT_LEFT, JT_RIGHT, JT_FIRE, JT_FIRE2, JT_FIRE3, JT_FIRE4,
    JT_KEY,
    JT_COUNT = JT_KEY + 40
};

/* A target's name, for the menu ("Fire 2", "Key Q", "Symbol Shift"). */
const char *joymap_target_name(uint8_t target);

/* The report decoders: true if the report was one of theirs. */
bool pad_from_generic(const uint8_t *r, uint16_t len, uint16_t *pad);    // DragonRise and friends
bool pad_from_ds4(const uint8_t *r, uint16_t len, uint16_t *pad);        // report ID first
bool pad_from_ds5(const uint8_t *r, uint16_t len, uint16_t *pad);
bool pad_from_xinput(uint16_t buttons, uint8_t lt, uint8_t rt,
                     int16_t lx, int16_t ly, uint16_t *pad);

/* Which known pad a VID:PID is. */
typedef enum { PADKIND_GENERIC = 0, PADKIND_DS4, PADKIND_DS5 } padkind_t;
padkind_t pad_kind(uint16_t vid, uint16_t pid);

/* The pad's state now: presses and releases go to the machine, through
 * settings.joymap. Start is never mapped -- it is the way to the menu. */
void joymap_apply(uint16_t pad);

/* Let go of everything the pad was holding. */
void joymap_release(void);

/* A slot's target with JT_DEFAULT resolved. */
uint8_t joymap_target(unsigned slot);
