/*
 * frank-dino — an RP2350B acting as the whole machine around a real Z80
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-dino
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The enumeration and boot-protocol handling here follows frank-xt8086's
 * usbhid driver, which solved both on this chip. The mapping is this
 * firmware's, because its destination is a 40-key matrix and not a PC.
 */

#include "usbkbd.h"

#include <stdio.h>
#include <string.h>

#include <bsp/board_api.h>
#include <tusb.h>

#include "settings.h"
#include "spectrum.h"

static bool    mounted;
static uint8_t last_report[8];

bool usbkbd_present(void) { return mounted; }

// ---------------------------------------------------------------------------
// USB HID usage codes to the Spectrum's matrix
// ---------------------------------------------------------------------------
//
// A Spectrum has forty keys and no others: no symbols, no cursor keys, no
// backspace. Everything a modern keyboard has beyond those forty is a
// *chord* on the real machine — CAPS SHIFT with a digit for the cursors
// and DELETE, SYMBOL SHIFT with a letter for most punctuation — so the
// mapping's job is to turn one key into up to two matrix positions.
//
// The rows are in address-line order, row 0 being A8. Within a row bit 0
// is the key nearest the outside of the keyboard, which is why the two
// halves appear to run in opposite directions and are not a mistake.
enum { R_CSZXCV = 0, R_ASDFG, R_QWERT, R_12345, R_09876, R_POIUY, R_ENTER, R_SPACE };

#define CS  0x80u       // needs CAPS SHIFT
#define SS  0x40u       // needs SYMBOL SHIFT
#define KEY(row, bit) (uint8_t)(((row) << 3) | (bit))
#define NONE 0xFFu

// Indexed by HID usage. 0x00-0x67 covers everything a boot-protocol
// keyboard reports that this machine has anywhere to put.
static const uint16_t hid_to_spec[0x68] = {
    [0x04] = KEY(R_ASDFG, 0),  // a
    [0x05] = KEY(R_SPACE, 4),  // b
    [0x06] = KEY(R_CSZXCV, 3), // c
    [0x07] = KEY(R_ASDFG, 2),  // d
    [0x08] = KEY(R_QWERT, 2),  // e
    [0x09] = KEY(R_ASDFG, 3),  // f
    [0x0A] = KEY(R_ASDFG, 4),  // g
    [0x0B] = KEY(R_ENTER, 4),  // h
    [0x0C] = KEY(R_POIUY, 2),  // i
    [0x0D] = KEY(R_ENTER, 3),  // j
    [0x0E] = KEY(R_ENTER, 2),  // k
    [0x0F] = KEY(R_ENTER, 1),  // l
    [0x10] = KEY(R_SPACE, 2),  // m
    [0x11] = KEY(R_SPACE, 3),  // n
    [0x12] = KEY(R_POIUY, 1),  // o
    [0x13] = KEY(R_POIUY, 0),  // p
    [0x14] = KEY(R_QWERT, 0),  // q
    [0x15] = KEY(R_QWERT, 3),  // r
    [0x16] = KEY(R_ASDFG, 1),  // s
    [0x17] = KEY(R_QWERT, 4),  // t
    [0x18] = KEY(R_POIUY, 3),  // u
    [0x19] = KEY(R_CSZXCV, 4), // v
    [0x1A] = KEY(R_QWERT, 1),  // w
    [0x1B] = KEY(R_CSZXCV, 2), // x
    [0x1C] = KEY(R_POIUY, 4),  // y
    [0x1D] = KEY(R_CSZXCV, 1), // z

    [0x1E] = KEY(R_12345, 0),  // 1
    [0x1F] = KEY(R_12345, 1),  // 2
    [0x20] = KEY(R_12345, 2),  // 3
    [0x21] = KEY(R_12345, 3),  // 4
    [0x22] = KEY(R_12345, 4),  // 5
    [0x23] = KEY(R_09876, 4),  // 6
    [0x24] = KEY(R_09876, 3),  // 7
    [0x25] = KEY(R_09876, 2),  // 8
    [0x26] = KEY(R_09876, 1),  // 9
    [0x27] = KEY(R_09876, 0),  // 0

    [0x28] = KEY(R_ENTER, 0),  // Enter
    [0x2C] = KEY(R_SPACE, 0),  // Space

    // The chords. A Spectrum spells these with a shift and a key, and so
    // does this — pressing DELETE really does hold CAPS SHIFT and 0.
    [0x2A] = CS | KEY(R_09876, 0),  // Backspace  = CAPS + 0 (DELETE)
    [0x29] = CS | KEY(R_SPACE, 0),  // Escape     = CAPS + SPACE (BREAK)
    [0x4C] = CS | KEY(R_09876, 0),  // Delete     = the same DELETE

    [0x50] = CS | KEY(R_12345, 4),  // Left   = CAPS + 5
    [0x51] = CS | KEY(R_09876, 4),  // Down   = CAPS + 6
    [0x52] = CS | KEY(R_09876, 3),  // Up     = CAPS + 7
    [0x4F] = CS | KEY(R_09876, 2),  // Right  = CAPS + 8

    [0x2D] = SS | KEY(R_ENTER, 3),  // -  = SYM + J
    [0x2E] = SS | KEY(R_ENTER, 1),  // =  = SYM + L
    [0x33] = SS | KEY(R_POIUY, 1),  // ;  = SYM + O
    [0x34] = SS | KEY(R_POIUY, 0),  // '  = SYM + P  (")
    [0x36] = SS | KEY(R_SPACE, 3),  // ,  = SYM + N
    [0x37] = SS | KEY(R_SPACE, 2),  // .  = SYM + M
    [0x38] = SS | KEY(R_CSZXCV, 4), // /  = SYM + V
};

// The two shifts, which are keys in their own right and also the thing
// every chord above is built from.
#define CAPS_SHIFT_ROW R_CSZXCV
#define CAPS_SHIFT_BIT 0
#define SYM_SHIFT_ROW  R_SPACE
#define SYM_SHIFT_BIT  1

/*
 * Rebuild the whole matrix from the report, rather than tracking changes.
 *
 * A boot-protocol report is the complete set of keys currently down, so
 * the matrix can be derived from it outright. Diffing against the last
 * report instead means every dropped or reordered report leaves a key
 * stuck down — and a stuck CAPS SHIFT on a Spectrum turns every
 * subsequent keystroke into a different one, which reads as the keyboard
 * being broken rather than as one lost packet.
 */
/*
 * Which joystick direction a HID usage stands for, or -1.
 *
 * The four arrows and the two obvious fire keys. Held apart from the
 * matrix table because a direction is not a key: with a Kempston
 * selected it reaches the guest through a port that has nothing to do
 * with the keyboard, and with a Sinclair selected it reaches the same
 * rows the arrows already use -- which would be the arrows pressing
 * themselves twice if this were merged into that table.
 */
static int joy_dir(const uint8_t usage) {
    switch (usage) {
        case 0x4F: return JOY_BIT_RIGHT;
        case 0x50: return JOY_BIT_LEFT;
        case 0x51: return JOY_BIT_DOWN;
        case 0x52: return JOY_BIT_UP;
        case 0x2C: return JOY_BIT_FIRE;     // Space
        case 0x2B: return JOY_BIT_FIRE;     // Tab
        default:   return -1;
    }
}

static void apply_report(const uint8_t *r) {
    for (uint8_t row = 0; row < 8; row++)
        for (uint8_t bit = 0; bit < 5; bit++)
            spec_key_set(row, bit, false);

    const bool arrows_are_joy = settings.joystick != JOY_NONE && settings.cursor_as_joy;
    for (uint8_t b = 0; b <= JOY_BIT_FIRE; b++) spec_joy_set(b, false);

    const uint8_t mods = r[0];
    bool caps = (mods & 0x22) != 0;         // either Shift
    bool sym  = (mods & 0x11) != 0 ||       // either Ctrl
                (mods & 0x44) != 0;         // either Alt

    for (int i = 2; i < 8; i++) {
        const uint8_t usage = r[i];
        if (!usage || usage >= 0x68) continue;

        if (arrows_are_joy) {
            const int d = joy_dir(usage);
            if (d >= 0) {
                spec_joy_set((uint8_t)d, true);
                // Space and Tab are still their own keys; only the four
                // arrows are taken over by the joystick.
                if (usage == 0x4F || usage == 0x50 || usage == 0x51 || usage == 0x52) continue;
            }
        }

        const uint16_t m = hid_to_spec[usage];
        if (!m) continue;

        if (m & CS) caps = true;
        if (m & SS) sym  = true;

        const uint8_t pos = (uint8_t)(m & 0x3F);
        spec_key_set((uint8_t)(pos >> 3), (uint8_t)(pos & 7), true);
    }

    if (caps) spec_key_set(CAPS_SHIFT_ROW, CAPS_SHIFT_BIT, true);
    if (sym)  spec_key_set(SYM_SHIFT_ROW,  SYM_SHIFT_BIT,  true);
}

// ---------------------------------------------------------------------------
// TinyUSB host callbacks
// ---------------------------------------------------------------------------

void tuh_hid_mount_cb(uint8_t dev_addr, uint8_t instance,
                      uint8_t const *desc_report, uint16_t desc_len) {
    (void)desc_report; (void)desc_len;

    if (tuh_hid_interface_protocol(dev_addr, instance) != HID_ITF_PROTOCOL_KEYBOARD) {
        // A mouse or a gamepad. Nothing here wants one yet, and asking
        // for its reports would only cost time in the host stack.
        return;
    }

    // Boot protocol, so the report is the fixed eight bytes above rather
    // than something that has to be parsed out of a report descriptor.
    tuh_hid_set_protocol(dev_addr, instance, HID_PROTOCOL_BOOT);
    tuh_hid_receive_report(dev_addr, instance);

    mounted = true;
    printf("[usbkbd] keyboard on address %u instance %u\n", dev_addr, instance);
}

void tuh_hid_umount_cb(uint8_t dev_addr, uint8_t instance) {
    (void)dev_addr; (void)instance;
    mounted = false;
    memset(last_report, 0, sizeof last_report);

    // Release everything. A keyboard unplugged mid-keystroke otherwise
    // leaves that key held for ever, and the machine goes on repeating it.
    for (uint8_t row = 0; row < 8; row++)
        for (uint8_t bit = 0; bit < 5; bit++)
            spec_key_set(row, bit, false);

    printf("[usbkbd] keyboard gone\n");
}

void tuh_hid_report_received_cb(uint8_t dev_addr, uint8_t instance,
                                uint8_t const *report, uint16_t len) {
    if (tuh_hid_interface_protocol(dev_addr, instance) == HID_ITF_PROTOCOL_KEYBOARD &&
        len >= 8) {
        memcpy(last_report, report, 8);
        apply_report(last_report);
    }

    // Ask for the next one. Forgetting this is the classic way to get a
    // keyboard that works for exactly one keystroke.
    tuh_hid_receive_report(dev_addr, instance);
}

// ---------------------------------------------------------------------------

void usbkbd_init(void) {
    board_init();
    tuh_init(BOARD_TUH_RHPORT);
    printf("[usbkbd] host stack up; S1 must be in USB-A (host) position\n");
}

void usbkbd_task(void) { tuh_task(); }
