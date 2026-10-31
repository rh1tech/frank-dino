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

#include "menu.h"
#include "settings.h"
#include "spectrum.h"

static bool    mounted;
static uint8_t last_report[8];
static bool    menu_wanted;
static bool    menu_wanted_settings;

bool usbkbd_present(void) { return mounted; }

bool usbkbd_take_menu_request(void) {
    const bool w = menu_wanted;
    menu_wanted = false;
    return w;
}

bool usbkbd_menu_request_was_settings(void) { return menu_wanted_settings; }

static volatile bool pause_wanted;

bool usbkbd_take_pause_request(void) {
    const bool w = pause_wanted;
    pause_wanted = false;
    return w;
}

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
    // Backspace, Esc and Caps Lock are what pico-speccy and SpeccyP make
    // them; Tab is SpeccyP's EDIT.
    [0x2A] = CS | KEY(R_09876, 0),  // Backspace  = CAPS + 0 (DELETE)
    [0x29] = CS | KEY(R_SPACE, 0),  // Escape     = CAPS + SPACE (BREAK)
    [0x4C] = CS | KEY(R_09876, 0),  // Delete     = the same DELETE
    [0x39] = CS | KEY(R_12345, 1),  // Caps Lock  = CAPS + 2 (CAPS LOCK)
    [0x2B] = CS | KEY(R_12345, 0),  // Tab        = CAPS + 1 (EDIT)

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
    // SpeccyP's, except \ -- it gives ':' there, and the Spectrum has a
    // backslash of its own.
    [0x2F] = SS | KEY(R_09876, 2),  // [  = SYM + 8  (()
    [0x30] = SS | KEY(R_09876, 1),  // ]  = SYM + 9  ())
    [0x31] = SS | KEY(R_ASDFG, 2),  // \  = SYM + D
    [0x35] = SS | KEY(R_ASDFG, 0),  // `  = SYM + A  (STOP)
    [0x32] = SS | KEY(R_ASDFG, 0),  // the non-US # beside Enter, the same

    // The keypad types what is printed on it, as on SpeccyP. Its '.' is
    // left out: that is joystick fire, below.
    [0x54] = SS | KEY(R_CSZXCV, 4), // KP /  = SYM + V
    [0x55] = SS | KEY(R_SPACE, 4),  // KP *  = SYM + B
    [0x56] = SS | KEY(R_ENTER, 3),  // KP -  = SYM + J
    [0x57] = SS | KEY(R_ENTER, 2),  // KP +  = SYM + K
    [0x58] = KEY(R_ENTER, 0),       // KP Enter
    [0x59] = KEY(R_12345, 0),       // KP 1
    [0x5A] = KEY(R_12345, 1),       // KP 2
    [0x5B] = KEY(R_12345, 2),       // KP 3
    [0x5C] = KEY(R_12345, 3),       // KP 4
    [0x5D] = KEY(R_12345, 4),       // KP 5
    [0x5E] = KEY(R_09876, 4),       // KP 6
    [0x5F] = KEY(R_09876, 3),       // KP 7
    [0x60] = KEY(R_09876, 2),       // KP 8
    [0x61] = KEY(R_09876, 1),       // KP 9
    [0x62] = KEY(R_09876, 0),       // KP 0
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
 * Which joystick bit a HID usage stands for, or -1.
 *
 * The four arrows and the keypad's '.', which is SpeccyP's fire key.
 * Held apart from the matrix table because these are in addition to the
 * keys, not instead of them: an arrow still presses CAPS SHIFT and its
 * digit, as it does in pico-speccy, so the 128K menu and BASIC never
 * lose the cursor keys to a game's joystick.
 */
static int joy_dir(const uint8_t usage) {
    switch (usage) {
        case 0x4F: return JOY_BIT_RIGHT;
        case 0x50: return JOY_BIT_LEFT;
        case 0x51: return JOY_BIT_DOWN;
        case 0x52: return JOY_BIT_UP;
        case 0x63: return JOY_BIT_FIRE;     // keypad '.'
        default:   return -1;
    }
}

void usbkbd_release_all(void) {
    for (uint8_t row = 0; row < 8; row++)
        for (uint8_t bit = 0; bit < 5; bit++)
            spec_key_set(row, bit, false);
    for (uint8_t b = 0; b <= JOY_BIT_FIRE; b++) spec_joy_set(b, false);
}

static void apply_report(const uint8_t *r) {
    usbkbd_release_all();

    const bool joy = settings.joystick != JOY_NONE && settings.cursor_as_joy;

    // Shift is CAPS SHIFT and Ctrl is SYMBOL SHIFT, in both pico-speccy
    // and SpeccyP. Alt is not on the matrix in either; here it is fire,
    // as LAlt is in pico-speccy and RAlt in SpeccyP.
    const uint8_t mods = r[0];
    bool caps = (mods & 0x22) != 0;         // either Shift
    bool sym  = (mods & 0x11) != 0;         // either Ctrl
    if (joy && (mods & 0x44)) spec_joy_set(JOY_BIT_FIRE, true);

    for (int i = 2; i < 8; i++) {
        const uint8_t usage = r[i];
        if (!usage || usage >= 0x68) continue;

        if (joy) {
            const int d = joy_dir(usage);
            if (d >= 0) spec_joy_set((uint8_t)d, true);
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
// The gamepad
// ---------------------------------------------------------------------------
/*
 * Ported from pico-speccy src/hid_app.cpp, process_hid_gamepad().
 *
 * Their comment carries the layout, and it is the one every cheap
 * SNES-shaped USB pad and every DragonRise clone sends:
 *
 *   [0] X axis : 0x00 left, 0x7F centre, 0xFF right
 *   [1] Y axis : 0x00 up,   0x7F centre, 0xFF down
 *   [5] high nibble, the face buttons : 0x10 X, 0x20 A, 0x40 B, 0x80 Y
 *   [6] shoulders and system : 0x01 L, 0x02 R, 0x10 Select, 0x20 Start
 *
 * Their thresholds come with it: a third of the way off centre counts as
 * a direction, which is what makes an analogue stick usable on a machine
 * whose joystick is four switches.
 *
 * What is not here is the rest of their file -- the DualShock 4 and 5
 * report formats, the xinput class driver, and a table of per-vendor
 * quirks. Those are two thousand lines for pads this has no way to test
 * against, and this is a copy_to_ram build.
 */
/* Defined with the keyboard's translation, below. */
static void menu_arrow(char last);

static bool    mouse_present;
static uint8_t mouse_addr, mouse_inst;

static bool    pad_present;
static uint8_t pad_addr, pad_inst;
static uint8_t pad_prev;            // the last state, so only edges are sent

/* B is fire: the bottom button of the diamond, which is where a thumb sits. */
#define PAD_BIT_A      0x20
#define PAD_BIT_B      0x40
#define PAD_BIT_START  0x20
#define PAD_BIT_SELECT 0x10

bool usbkbd_pad_present(void) { return pad_present; }

static void pad_decode(const uint8_t *report, const uint16_t len) {
    if (len < 7) return;

    const bool left  = report[0] < 0x40;
    const bool right = report[0] > 0xC0;
    const bool up    = report[1] < 0x40;
    const bool down  = report[1] > 0xC0;
    const uint8_t b5 = report[5], b6 = report[6];
    const bool fire  = (b5 & (PAD_BIT_A | PAD_BIT_B)) != 0;   // either, for one-button games
    const bool start = (b6 & PAD_BIT_START)  != 0;
    const bool sel   = (b6 & PAD_BIT_SELECT) != 0;

    const uint8_t now = (uint8_t)((up    ? 1u << JOY_BIT_UP    : 0)
                                | (down  ? 1u << JOY_BIT_DOWN  : 0)
                                | (left  ? 1u << JOY_BIT_LEFT  : 0)
                                | (right ? 1u << JOY_BIT_RIGHT : 0)
                                | (fire  ? 1u << JOY_BIT_FIRE  : 0));

    if (menu_is_open()) {
        /*
         * In the UI the pad drives the cursor, which is the difference
         * between a pad you can pick a game with and one you have to put
         * down to do it. Edges only: the report repeats while a
         * direction is held.
         */
        const uint8_t went_down = (uint8_t)(now & ~pad_prev);
        if (went_down & (1u << JOY_BIT_UP))    menu_arrow('A');
        if (went_down & (1u << JOY_BIT_DOWN))  menu_arrow('B');
        if (went_down & (1u << JOY_BIT_RIGHT)) menu_arrow('C');
        if (went_down & (1u << JOY_BIT_LEFT))  menu_arrow('D');
        if (went_down & (1u << JOY_BIT_FIRE))  menu_key('\r');
        if (start && !(pad_prev & 0x80)) menu_key('\t');     // Start closes
        if (sel)   menu_key(0x7F);                           // Select goes up
    } else {
        for (uint8_t b = 0; b < 5; b++)
            if (((now ^ pad_prev) >> b) & 1)
                spec_joy_set(b, (now >> b) & 1);
        /* Start is the way into the browser without reaching for F11. */
        if (start && !(pad_prev & 0x80)) { menu_wanted_settings = false; menu_wanted = true; }
    }

    pad_prev = (uint8_t)(now | (start ? 0x80 : 0));
}

// ---------------------------------------------------------------------------
// TinyUSB host callbacks
// ---------------------------------------------------------------------------

void tuh_hid_mount_cb(uint8_t dev_addr, uint8_t instance,
                      uint8_t const *desc_report, uint16_t desc_len) {
    (void)desc_report; (void)desc_len;

    const uint8_t proto = tuh_hid_interface_protocol(dev_addr, instance);

    if (proto == HID_ITF_PROTOCOL_NONE) {
        /*
         * Neither a boot keyboard nor a boot mouse, which in practice on
         * this port means a gamepad: the cheap SNES-shaped ones and the
         * DragonRise clones all announce protocol NONE and then send the
         * fixed eight-byte report pad_decode() reads.
         *
         * Taken without parsing the report descriptor, exactly as theirs
         * does for this family -- the descriptor would have to be walked
         * to learn a layout that is the same on every one of them.
         */
        tuh_hid_receive_report(dev_addr, instance);
        pad_addr = dev_addr;
        pad_inst = instance;
        pad_present = true;
        printf("[usbkbd] gamepad on address %u instance %u\n", dev_addr, instance);
        return;
    }

    if (proto == HID_ITF_PROTOCOL_MOUSE) {
        /*
         * A Kempston mouse. Boot protocol: three or four bytes -- the
         * buttons, X, Y, and on most mice the wheel -- with no descriptor
         * to walk. pico-speccy asks for report protocol to be sure of the
         * wheel; boot mode keeps this to one shape, and most wheel mice
         * send it there too.
         */
        tuh_hid_set_protocol(dev_addr, instance, HID_PROTOCOL_BOOT);
        tuh_hid_receive_report(dev_addr, instance);
        mouse_addr = dev_addr;
        mouse_inst = instance;
        mouse_present = true;
        spec_mouse_seen = true;
        printf("[usbkbd] mouse on address %u instance %u\n", dev_addr, instance);
        return;
    }

    if (proto != HID_ITF_PROTOCOL_KEYBOARD) return;

    // Boot protocol, so the report is the fixed eight bytes above rather
    // than something that has to be parsed out of a report descriptor.
    tuh_hid_set_protocol(dev_addr, instance, HID_PROTOCOL_BOOT);
    tuh_hid_receive_report(dev_addr, instance);

    mounted = true;
    printf("[usbkbd] keyboard on address %u instance %u\n", dev_addr, instance);
}

void tuh_hid_umount_cb(uint8_t dev_addr, uint8_t instance) {
    if (mouse_present && dev_addr == mouse_addr && instance == mouse_inst) {
        mouse_present = false;
        spec_mouse_buttons = 0;
        return;
    }
    if (pad_present && dev_addr == pad_addr && instance == pad_inst) {
        pad_present = false;
        pad_prev = 0;
        /* A pad unplugged with a direction held otherwise leaves the
         * machine walking into a wall for ever. */
        for (uint8_t b = 0; b < 5; b++) spec_joy_set(b, false);
        printf("[usbkbd] gamepad gone\n");
        return;
    }
    mounted = false;
    memset(last_report, 0, sizeof last_report);

    // Release everything. A keyboard unplugged mid-keystroke otherwise
    // leaves that key held for ever, and the machine goes on repeating it.
    for (uint8_t row = 0; row < 8; row++)
        for (uint8_t bit = 0; bit < 5; bit++)
            spec_key_set(row, bit, false);

    printf("[usbkbd] keyboard gone\n");
}

// ---------------------------------------------------------------------------
// The menu, which is not the Spectrum
// ---------------------------------------------------------------------------
/*
 * F1 opens the file browser, and while it is open the keyboard drives
 * that instead of the machine.
 *
 * It has to be a key the Spectrum has not got, because every key it has
 * belongs to the matrix -- and in a USB_KEYBOARD build there is no
 * console to press Tab at, so if the keyboard cannot reach the browser
 * then nothing can.
 *
 * What the browser wants is bytes, the same ones a terminal sends: a
 * cursor key is ESC, '[' and a letter. That is why menu_key() feeds a
 * queue rather than a single slot.
 */
#define HID_F11 0x44
#define HID_F12 0x45

static bool is_new(const uint8_t *now, const uint8_t *before, const uint8_t usage) {
    for (int i = 2; i < 8; i++) if (before[i] == usage) return false;
    for (int i = 2; i < 8; i++) if (now[i] == usage)    return true;
    return false;
}

/* Send a cursor or paging key as the three bytes the menus reassemble. */
static void menu_arrow(const char last) {
    menu_key(0x1B);
    menu_key('[');
    menu_key(last);
}

/* Page Up / Page Down are four: the digit carries a '~' after it. */
static void menu_tilde(const char digit) {
    menu_key(0x1B);
    menu_key('[');
    menu_key(digit);
    menu_key('~');
}

/*
 * Turn newly-pressed keys into browser input.
 *
 * Newly pressed, not pressed: a boot-protocol keyboard repeats the same
 * report while a key is held, and a menu that took every one of them
 * would scroll a directory past in a blink.
 */
static void feed_menu(const uint8_t *now, const uint8_t *before) {
    for (int i = 2; i < 8; i++) {
        const uint8_t u = now[i];
        if (!u || !is_new(now, before, u)) continue;

        switch (u) {
            case 0x52: menu_arrow('A'); break;      // up
            case 0x51: menu_arrow('B'); break;      // down
            case 0x4F: menu_arrow('C'); break;      // right
            case 0x50: menu_arrow('D'); break;      // left
            case 0x4B: menu_tilde('5'); break;      // page up
            case 0x4E: menu_tilde('6'); break;      // page down
            case 0x4A: menu_arrow('H'); break;      // home
            case 0x4D: menu_arrow('F'); break;      // end
            case 0x28: menu_key('\r'); break;       // enter
            case 0x2A: menu_key(0x7F); break;       // backspace
            /*
             * Escape is its own byte, not a synonym for Tab.
             *
             * It used to be one, because the browser is a flat list with
             * nothing to back out of and leaving was the only thing Esc
             * could mean. The settings menu has levels, and its footer
             * offers Esc as the way up one -- so the two have to arrive
             * here as different keys. Both menus read the continuation of
             * an escape sequence with a timeout, so a bare 0x1B is safe.
             */
            case 0x29: menu_key(0x1B); break;       // escape
            /*
             * F3 opens the browser's incremental search.
             *
             * 0x06 rather than a letter because every letter is a jump:
             * the browser's list answers to its initial, so a verb key
             * has to be something the list can never mean.
             */
            case 0x3C: menu_key(0x06); break;       // F3: find
            case HID_F11:
            case 0x2B: menu_key('\t'); break;       // tab: leave
            case HID_F12:
            case 0x31: menu_key('\\'); break;       // backslash: settings
            default: {
                // Everything printable, so that typing a letter jumps to
                // the next file beginning with it.
                if (u >= 0x04 && u <= 0x1D)      menu_key((char)('a' + (u - 0x04)));
                else if (u >= 0x1E && u <= 0x26) menu_key((char)('1' + (u - 0x1E)));
                else if (u == 0x27)              menu_key('0');
                else if (u == 0x2D)              menu_key('-');
                else if (u == 0x2C)              menu_key(' ');
                break;
            }
        }
    }
}

/*
 * Auto-repeat, which a keyboard does not do for you.
 *
 * A boot-protocol report is the set of keys currently down, and it is
 * only sent when that set *changes* -- so holding a key produces exactly
 * one report and nothing more. The Spectrum does not care: its matrix is
 * a level, and the ROM's own REPDEL/REPPER handle the rest. The menus do
 * care, because they are fed discrete keystrokes, and holding Down in a
 * two-hundred-entry directory did nothing at all.
 *
 * So the repeat is made here, on the same schedule a Spectrum uses: a
 * pause of about a third of a second, then five or six a second.
 */
#define REPEAT_DELAY_MS  350
#define REPEAT_RATE_MS    60

static uint8_t         repeat_usage;
static absolute_time_t repeat_due;

static bool held(const uint8_t usage) {
    for (int i = 2; i < 8; i++) if (last_report[i] == usage) return true;
    return false;
}

static void repeat_arm(const uint8_t usage) {
    repeat_usage = usage;
    repeat_due   = make_timeout_time_ms(REPEAT_DELAY_MS);
}

static void repeat_task(void) {
    if (!repeat_usage) return;

    // Let go, or left the menu, and it stops.
    if (!menu_is_open() || !held(repeat_usage)) { repeat_usage = 0; return; }

    if (absolute_time_diff_us(get_absolute_time(), repeat_due) > 0) return;

    const uint8_t one[8] = { 0, 0, repeat_usage, 0, 0, 0, 0, 0 };
    const uint8_t none[8] = { 0 };
    feed_menu(one, none);
    repeat_due = make_timeout_time_ms(REPEAT_RATE_MS);
}

/*
 * A mouse report onto the Kempston counters.
 *
 * Sensitivity is in halves, with the remainder carried so slow movement
 * is not lost to rounding. The Kempston Y counter goes UP when the mouse
 * goes away from you; USB's goes down.
 */
static void mouse_decode(const uint8_t *r, const uint16_t len) {
    if (len < 3) return;
    static int32_t fx, fy;
    const int32_t s = settings.mouse_sens ? settings.mouse_sens : 2;
    fx += (int8_t)r[1] * s;
    fy += (int8_t)r[2] * s;
    spec_mouse_x = (uint8_t)(spec_mouse_x + fx / 2);
    spec_mouse_y = (uint8_t)(spec_mouse_y - fy / 2);
    fx %= 2;
    fy %= 2;
    spec_mouse_buttons = r[0] & 7u;
    if (len >= 4) spec_mouse_wheel = (uint8_t)(spec_mouse_wheel + (int8_t)r[3]);
}

void tuh_hid_report_received_cb(uint8_t dev_addr, uint8_t instance,
                                uint8_t const *report, uint16_t len) {
    if (mouse_present && dev_addr == mouse_addr && instance == mouse_inst) {
        mouse_decode(report, len);
        tuh_hid_receive_report(dev_addr, instance);
        return;
    }
    if (pad_present && dev_addr == pad_addr && instance == pad_inst) {
        pad_decode(report, len);
        tuh_hid_receive_report(dev_addr, instance);
        return;
    }

    if (tuh_hid_interface_protocol(dev_addr, instance) == HID_ITF_PROTOCOL_KEYBOARD &&
        len >= 8) {
        uint8_t before[8];
        memcpy(before, last_report, 8);
        memcpy(last_report, report, 8);

        if (menu_is_open()) {
            feed_menu(last_report, before);

            // Whatever was pressed last is what repeats. A second key
            // taking over is what a real keyboard does too.
            repeat_usage = 0;
            for (int i = 2; i < 8; i++)
                if (last_report[i] && is_new(last_report, before, last_report[i]))
                    repeat_arm(last_report[i]);
        } else if (is_new(last_report, before, 0x48)) {
            // Pause: not the Spectrum's, so it never reaches the matrix.
            pause_wanted = true;
        } else if (is_new(last_report, before, HID_F11) ||
                   is_new(last_report, before, HID_F12)) {
            /*
             * Asked for, not opened. This runs inside tuh_task(), and
             * the browser is modal and calls tuh_task() itself -- so it
             * is started from the main loop once this has returned.
             */
            menu_wanted = true;
            menu_wanted_settings = is_new(last_report, before, HID_F12);
        } else {
            apply_report(last_report);
        }
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

void usbkbd_task(void) {
    tuh_task();
    repeat_task();
}
