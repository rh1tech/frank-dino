/*
 * frank-dino — an RP2350B acting as the whole machine around a real Z80
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-dino
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Ported from pico-speccy's machines/TsConf/ZxEvoAvr.cpp
 * (https://github.com/drewpo28/pico-speccy, GPL-3.0) by drew.po28 -- the
 * USB-to-set-2 tables and the log's ring semantics, which are the AVR
 * firmware's own (pentevo avr/current ps2.c).
 */

/*
 * The ZX Evolution's AVR reads a PS/2 keyboard, and besides turning it
 * into the Spectrum's matrix it keeps the raw set-2 bytes in a sixteen-
 * byte log. Programs written for the Evo alone -- avrconf, Wild
 * Commander, the TS-Conf tools on BaseConf -- read their keys from there
 * and never look at #FE, so without the log they start and then no key
 * does anything.
 *
 * The bytes come from the USB keyboard's reports (and the console's
 * characters, so the bench can type) on core 0, and are read by the Z80
 * through the Gluk cells on core 1: one writer, one reader, and each
 * side owns its own end of the ring.
 *
 * The log's edges are the AVR's: reading an empty log gives 0; filling
 * it freezes it, and the next read gives 0xFF and empties it.
 */
#include "evo_ps2.h"

#define LOG_SIZE 16u

static uint8_t           ring[LOG_SIZE];
static volatile uint32_t head, tail;            // head: core 0's; tail: core 1's
static volatile bool     overflow, clear_req;
static volatile uint8_t  mods0, mods1;

/*
 * USB usage (0x00-0x6F) to set-2 make code. 0 is no code; bit 7 set is
 * an E0-prefixed code, except 0x83 -- F7, the one plain code above 0x7F.
 */
static const uint8_t kHidToSet2[0x70] = {
    0x00, 0x00, 0x00, 0x00, 0x1C, 0x32, 0x21, 0x23, 0x24, 0x2B, 0x34, 0x33, 0x43, 0x3B, 0x42, 0x4B,  // 00
    0x3A, 0x31, 0x44, 0x4D, 0x15, 0x2D, 0x1B, 0x2C, 0x3C, 0x2A, 0x1D, 0x22, 0x35, 0x1A, 0x16, 0x1E,  // 10
    0x26, 0x25, 0x2E, 0x36, 0x3D, 0x3E, 0x46, 0x45, 0x5A, 0x76, 0x66, 0x0D, 0x29, 0x4E, 0x55, 0x54,  // 20
    0x5B, 0x5D, 0x5D, 0x4C, 0x52, 0x0E, 0x41, 0x49, 0x4A, 0x58, 0x05, 0x06, 0x04, 0x0C, 0x03, 0x0B,  // 30
    0x83, 0x0A, 0x01, 0x09, 0x78, 0x07, 0xFC, 0x7E, 0x00, 0xF0, 0xEC, 0xFD, 0xF1, 0xE9, 0xFA, 0xF4,  // 40
    0xEB, 0xF2, 0xF5, 0x77, 0xCA, 0x7C, 0x7B, 0x79, 0xDA, 0x69, 0x72, 0x7A, 0x6B, 0x73, 0x74, 0x6C,  // 50
    0x75, 0x7D, 0x70, 0x71, 0x61, 0xAF, 0x00, 0x0F, 0x08, 0x10, 0x18, 0x20, 0x28, 0x30, 0x38, 0x40,  // 60
};
/* LCtrl LShift LAlt LGui RCtrl RShift RAlt RGui. */
static const uint8_t kModToSet2[8] = { 0x14, 0x12, 0x11, 0x9F, 0x94, 0x59, 0x91, 0xA7 };

// ---------------------------------------------------------------------------
// Core 0: the writer
// ---------------------------------------------------------------------------
static void push(const uint8_t b) {
    if (overflow) return;                           // frozen until read
    const uint32_t h = head;
    if (h - tail >= LOG_SIZE) { overflow = true; return; }
    ring[h % LOG_SIZE] = b;
    __sync_synchronize();
    head = h + 1u;
}

static void set_bit(volatile uint8_t *reg, const uint8_t bit, const bool on) {
    *reg = (uint8_t)(on ? (*reg | bit) : (*reg & (uint8_t)~bit));
}

void evo_ps2_hid(const uint8_t usage, const bool down) {
    uint8_t v;
    if (usage >= 0xE0 && usage <= 0xE7) v = kModToSet2[usage - 0xE0];
    else if (usage < sizeof kHidToSet2) v = kHidToSet2[usage];
    else return;
    if (!v) return;

    switch (usage) {
        case 0xE0: set_bit(&mods0, 0x01, down); break;     // LCtrl
        case 0xE4: set_bit(&mods0, 0x02, down); break;     // RCtrl
        case 0xE2: set_bit(&mods0, 0x04, down); break;     // LAlt
        case 0xE6: set_bit(&mods0, 0x08, down); break;     // RAlt
        case 0xE1: set_bit(&mods0, 0x10, down); break;     // LShift
        case 0xE5: set_bit(&mods0, 0x20, down); break;     // RShift
        case 0x45: set_bit(&mods0, 0x40, down); break;     // F12
        case 0xE3: set_bit(&mods1, 0x01, down); break;     // LWin
        case 0xE7: set_bit(&mods1, 0x02, down); break;     // RWin
        case 0x65: set_bit(&mods1, 0x04, down); break;     // Menu
        default: break;
    }
    const bool ext = (v & 0x80u) && v != 0x83u;
    if (ext)   push(0xE0);
    if (!down) push(0xF0);
    push(ext ? (uint8_t)(v & 0x7Fu) : v);
}

static bool has(const uint8_t r[8], const uint8_t usage) {
    for (int i = 2; i < 8; i++) if (r[i] == usage) return true;
    return false;
}

void evo_ps2_report(const uint8_t now[8], const uint8_t before[8]) {
    const uint8_t diff = now[0] ^ before[0];
    for (unsigned b = 0; b < 8; b++)
        if (diff & (1u << b)) evo_ps2_hid((uint8_t)(0xE0u + b), (now[0] >> b) & 1u);
    for (int i = 2; i < 8; i++)                     // let go before pressing
        if (before[i] > 3 && !has(now, before[i])) evo_ps2_hid(before[i], false);
    for (int i = 2; i < 8; i++)
        if (now[i] > 3 && !has(before, now[i])) evo_ps2_hid(now[i], true);
}

/* A console character's usage, and whether it wants Shift. */
static uint8_t ascii_usage(const char c, bool *shift) {
    static const char plain[]   = "1234567890\r\x1b\x7f\t -=[]\\#;'`,./";
    static const char shifted[] = "!@#$%^&*()\0\0\0\0\0_+{}|~:\"~<>?";
    *shift = false;
    if (c >= 'a' && c <= 'z') return (uint8_t)(0x04 + (c - 'a'));
    if (c >= 'A' && c <= 'Z') { *shift = true; return (uint8_t)(0x04 + (c - 'A')); }
    if (c == '\n') return 0x28;
    if (c == '\b') return 0x2A;
    for (unsigned i = 0; i < sizeof plain - 1u; i++)
        if (plain[i] == c && i != 0x32u - 0x1Eu) return (uint8_t)(0x1E + i);   // 0x32: the non-US key
    for (unsigned i = 0; i < sizeof shifted - 1u; i++)
        if (shifted[i] && shifted[i] == c && i != 0x32u - 0x1Eu) { *shift = true; return (uint8_t)(0x1E + i); }
    return 0;
}

void evo_ps2_ascii(const char c) {
    bool shift;
    const uint8_t u = ascii_usage(c, &shift);
    if (!u) return;
    if (shift) evo_ps2_hid(0xE1, true);
    evo_ps2_hid(u, true);
    evo_ps2_hid(u, false);
    if (shift) evo_ps2_hid(0xE1, false);
}

// ---------------------------------------------------------------------------
// Core 1: the reader
// ---------------------------------------------------------------------------
uint8_t evo_ps2_pop(void) {
    if (clear_req) { clear_req = false; overflow = false; tail = head; }
    if (overflow) { tail = head; overflow = false; return 0xFF; }   // and what it held is gone
    const uint32_t t = tail;
    if (t == head) return 0;
    __sync_synchronize();
    const uint8_t b = ring[t % LOG_SIZE];
    tail = t + 1u;
    return b;
}

void evo_ps2_clear(void) { clear_req = true; }

uint8_t evo_ps2_mods0(void) { return mods0; }
uint8_t evo_ps2_mods1(void) { return mods1; }
