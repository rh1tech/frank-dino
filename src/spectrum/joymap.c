/*
 * frank-dino — an RP2350B acting as the whole machine around a real Z80
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-dino
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Report layouts and button roles after pico-speccy
 * (https://github.com/drewpo28/pico-speccy, GPL-3.0): hid_app.cpp
 * process_hid_gamepad / process_ds4_gamepad / process_ds5_gamepad and
 * joy_hid.cpp; the slot order is their Config::joydef.
 */

/*
 * One pad, whatever is plugged in.
 *
 * Every decoder below turns its own report into the same fourteen bits --
 * four directions, Start, Select and eight buttons -- and joymap_apply()
 * turns those into the machine: a direction or a fire bit of the
 * joystick, or one of the forty keys, per settings.joymap. The decoders
 * are pure functions of a report, which is what lets the pads this bench
 * does not have be tested against their documented layouts on a host.
 *
 * A stick counts as the D-pad once it is a quarter of the way over, so a
 * game that only knows four switches can be played with either.
 */
#include "joymap.h"

#include "settings.h"
#include "spectrum.h"

// ---------------------------------------------------------------------------
// Names
// ---------------------------------------------------------------------------
static const char *const kKeyNames[40] = {
    "Caps Shift", "Z", "X", "C", "V",
    "A", "S", "D", "F", "G",
    "Q", "W", "E", "R", "T",
    "1", "2", "3", "4", "5",
    "0", "9", "8", "7", "6",
    "P", "O", "I", "U", "Y",
    "Enter", "L", "K", "J", "H",
    "Space", "Symbol Shift", "M", "N", "B",
};

const char *joymap_target_name(const uint8_t t) {
    static const char *const fixed[JT_KEY] = {
        "Default", "Nothing", "Up", "Down", "Left", "Right", "Fire", "Fire 2", "Fire 3", "Fire 4",
    };
    if (t < JT_KEY) return fixed[t];
    if (t < JT_COUNT) return kKeyNames[t - JT_KEY];
    return "?";
}

// ---------------------------------------------------------------------------
// The decoders
// ---------------------------------------------------------------------------
static uint16_t hat(const uint8_t h) {
    switch (h & 0x0Fu) {
        case 0: return PAD_BIT(PAD_UP);
        case 1: return PAD_BIT(PAD_UP) | PAD_BIT(PAD_RIGHT);
        case 2: return PAD_BIT(PAD_RIGHT);
        case 3: return PAD_BIT(PAD_DOWN) | PAD_BIT(PAD_RIGHT);
        case 4: return PAD_BIT(PAD_DOWN);
        case 5: return PAD_BIT(PAD_DOWN) | PAD_BIT(PAD_LEFT);
        case 6: return PAD_BIT(PAD_LEFT);
        case 7: return PAD_BIT(PAD_UP) | PAD_BIT(PAD_LEFT);
        default: return 0;                          // 8: centred
    }
}

/* An 8-bit axis pair centred on 0x80: a quarter of the way is a direction. */
static uint16_t stick8(const uint8_t x, const uint8_t y) {
    uint16_t p = 0;
    if (x < 0x40) p |= PAD_BIT(PAD_LEFT);
    if (x > 0xC0) p |= PAD_BIT(PAD_RIGHT);
    if (y < 0x40) p |= PAD_BIT(PAD_UP);
    if (y > 0xC0) p |= PAD_BIT(PAD_DOWN);
    return p;
}

/*
 * The cheap SNES-shaped pads and the DragonRise clones:
 *   [0] X, [1] Y (0x7F centre); [5] high nibble X 0x10, A 0x20, B 0x40,
 *   Y 0x80; [6] L 0x01, R 0x02, LT 0x04, RT 0x08, Select 0x10, Start 0x20.
 */
bool pad_from_generic(const uint8_t *r, const uint16_t len, uint16_t *pad) {
    if (len < 7) return false;
    uint16_t p = stick8(r[0], r[1]);
    const uint8_t f = r[5], s = r[6];
    if (f & 0x10u) p |= PAD_BIT(PAD_X);
    if (f & 0x20u) p |= PAD_BIT(PAD_A);
    if (f & 0x40u) p |= PAD_BIT(PAD_B);
    if (f & 0x80u) p |= PAD_BIT(PAD_Y);
    if (s & 0x01u) p |= PAD_BIT(PAD_Z);
    if (s & 0x02u) p |= PAD_BIT(PAD_C);
    if (s & 0x04u) p |= PAD_BIT(PAD_L2);
    if (s & 0x08u) p |= PAD_BIT(PAD_R2);
    if (s & 0x10u) p |= PAD_BIT(PAD_SELECT);
    if (s & 0x20u) p |= PAD_BIT(PAD_START);
    *pad = p;
    return true;
}

/* Sony's face byte (hat in the low nibble) and shoulder byte, shared by both. */
static uint16_t sony(const uint8_t face, const uint8_t sh) {
    uint16_t p = hat(face);
    if (face & 0x10u) p |= PAD_BIT(PAD_X);          // Square
    if (face & 0x20u) p |= PAD_BIT(PAD_A);          // Cross
    if (face & 0x40u) p |= PAD_BIT(PAD_B);          // Circle
    if (face & 0x80u) p |= PAD_BIT(PAD_Y);          // Triangle
    if (sh & 0x01u) p |= PAD_BIT(PAD_Z);            // L1
    if (sh & 0x02u) p |= PAD_BIT(PAD_C);            // R1
    if (sh & 0x04u) p |= PAD_BIT(PAD_L2);
    if (sh & 0x08u) p |= PAD_BIT(PAD_R2);
    if (sh & 0x10u) p |= PAD_BIT(PAD_SELECT);       // Share / Create
    if (sh & 0x20u) p |= PAD_BIT(PAD_START);        // Options
    return p;
}

/* DualShock 4, report 0x01: LX LY RX RY, face+hat, shoulders. */
bool pad_from_ds4(const uint8_t *r, const uint16_t len, uint16_t *pad) {
    if (len < 10 || r[0] != 0x01) return false;
    const uint8_t *d = r + 1;
    *pad = (uint16_t)(stick8(d[0], d[1]) | sony(d[4], d[5]));
    return true;
}

/* DualSense, report 0x01: LX LY RX RY, L2 R2 analogue, a counter, face+hat, shoulders. */
bool pad_from_ds5(const uint8_t *r, const uint16_t len, uint16_t *pad) {
    if (len < 11 || r[0] != 0x01) return false;
    const uint8_t *d = r + 1;
    *pad = (uint16_t)(stick8(d[0], d[1]) | sony(d[7], d[8]));
    return true;
}

/* XInput's XINPUT_GAMEPAD, already unpacked by the class driver. */
bool pad_from_xinput(const uint16_t b, const uint8_t lt, const uint8_t rt,
                     const int16_t lx, const int16_t ly, uint16_t *pad) {
    uint16_t p = 0;
    if ((b & 0x0001u) || ly >  8000) p |= PAD_BIT(PAD_UP);
    if ((b & 0x0002u) || ly < -8000) p |= PAD_BIT(PAD_DOWN);
    if ((b & 0x0004u) || lx < -8000) p |= PAD_BIT(PAD_LEFT);
    if ((b & 0x0008u) || lx >  8000) p |= PAD_BIT(PAD_RIGHT);
    if (b & 0x0010u) p |= PAD_BIT(PAD_START);
    if (b & 0x0020u) p |= PAD_BIT(PAD_SELECT);      // Back
    if (b & 0x0100u) p |= PAD_BIT(PAD_Z);           // LB
    if (b & 0x0200u) p |= PAD_BIT(PAD_C);           // RB
    if (b & 0x1000u) p |= PAD_BIT(PAD_A);
    if (b & 0x2000u) p |= PAD_BIT(PAD_B);
    if (b & 0x4000u) p |= PAD_BIT(PAD_X);
    if (b & 0x8000u) p |= PAD_BIT(PAD_Y);
    if (lt > 0x40u) p |= PAD_BIT(PAD_L2);
    if (rt > 0x40u) p |= PAD_BIT(PAD_R2);
    *pad = p;
    return true;
}

padkind_t pad_kind(const uint16_t vid, const uint16_t pid) {
    if (vid != 0x054Cu) return PADKIND_GENERIC;
    if (pid == 0x05C4u || pid == 0x09CCu) return PADKIND_DS4;
    if (pid == 0x0CE6u || pid == 0x0DF2u) return PADKIND_DS5;     // DualSense, and its Edge
    return PADKIND_GENERIC;
}

// ---------------------------------------------------------------------------
// The machine
// ---------------------------------------------------------------------------
/* What each slot does until it is told otherwise: the pad as a joystick
 * with A and B both fire, as the one-button games want. */
static const uint8_t kDefault[PAD_SLOTS] = {
    JT_LEFT, JT_RIGHT, JT_UP, JT_DOWN, JT_NONE, JT_NONE,
    JT_FIRE, JT_FIRE, JT_NONE, JT_FIRE2, JT_FIRE3, JT_NONE, JT_NONE, JT_NONE,
};

uint8_t joymap_target(const unsigned slot) {
    if (slot >= PAD_SLOTS || slot == PAD_START) return JT_NONE;
    const uint8_t t = settings.joymap[slot];
    return (t == JT_DEFAULT || t >= JT_COUNT) ? kDefault[slot] : t;
}

static uint8_t  held_joy;           // joystick bits the pad is holding
static uint64_t held_keys;          // keys the pad is holding, bit = row * 5 + bit

static void push(const uint8_t joy, const uint64_t keys) {
    for (unsigned b = 0; b < 8; b++)
        if (((joy ^ held_joy) >> b) & 1u) spec_joy_set((uint8_t)b, (joy >> b) & 1u);
    for (unsigned k = 0; k < 40; k++)
        if (((keys ^ held_keys) >> k) & 1u)
            spec_key_set((uint8_t)(k / 5u), (uint8_t)(k % 5u), (keys >> k) & 1u);
    held_joy = joy;
    held_keys = keys;
}

void joymap_apply(const uint16_t pad) {
    static const uint8_t joybit[JT_KEY] = {
        0, 0, 1u << JOY_BIT_UP, 1u << JOY_BIT_DOWN, 1u << JOY_BIT_LEFT, 1u << JOY_BIT_RIGHT,
        1u << JOY_BIT_FIRE, 1u << JOY_BIT_FIRE2, 1u << JOY_BIT_FIRE3, 1u << JOY_BIT_FIRE4,
    };
    uint8_t joy = 0;
    uint64_t keys = 0;
    for (unsigned s = 0; s < PAD_SLOTS; s++) {
        if (!(pad & PAD_BIT(s))) continue;
        const uint8_t t = joymap_target(s);
        if (t < JT_KEY) joy |= joybit[t];
        else            keys |= 1ull << (t - JT_KEY);
    }
    push(joy, keys);
}

void joymap_release(void) { push(0, 0); }
