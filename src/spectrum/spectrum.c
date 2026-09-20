/*
 * frank-dino — an RP2350B acting as the whole machine around a real Z80
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-dino
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "spectrum.h"

#include <string.h>
#include <hardware/gpio.h>

#include "dino.h"
#include "z80_bus.h"

// ============================================================================
// The ROMs
// ============================================================================
//
// Welded into the binary rather than read off the card: the board has a
// microSD slot that nothing reads yet, and a machine that cannot start
// without a file on a card is a machine that cannot be tested by a
// script. See roms/README.md for where these two came from.
#define IMPORT_BIN(file, sym) asm (\
    ".section .rodata." #sym "\n" \
    ".balign 4\n"                 \
    ".global " #sym "\n"          \
    #sym ":\n"                    \
    ".incbin \"" file "\"\n"      \
    ".balign 4\n"                 \
    ".section \".text\"\n")

IMPORT_BIN("roms/128-0.rom", spec_rom0);
IMPORT_BIN("roms/128-1.rom", spec_rom1);
extern const uint8_t spec_rom0[SPEC_BANK_SIZE];
extern const uint8_t spec_rom1[SPEC_BANK_SIZE];

// ============================================================================
// State
// ============================================================================
uint8_t spec_ram[SPEC_RAM_BANKS][SPEC_BANK_SIZE] __attribute__((aligned(4)));

volatile uint8_t  spec_screen_bank = 5;
volatile uint8_t  spec_border      = 7;
volatile bool     spec_flash       = false;
volatile uint32_t spec_frames      = 0;
volatile uint8_t  spec_port_7ffd   = 0;

// Once 0x7FFD bit 5 is set the machine cannot page again until it is
// reset. That is how a 128K pretends to be a 48K, and how the 48K BASIC
// mode in the menu works — so it has to be honoured, not ignored.
static bool paging_locked = false;

// Keyboard: bit set = key up. Five keys per half-row, bits 0-4.
static volatile uint8_t kbd[8];

// ============================================================================
// Paging
// ============================================================================

static void apply_paging(void) {
    const uint8_t v = spec_port_7ffd;

    // Bottom window: ROM 1 (48K BASIC) when bit 4 is set, ROM 0 otherwise.
    // Read-only, which here means "its write pointer goes to the bucket".
    z80_map_page(0, (uint8_t *)((v & 0x10) ? spec_rom1 : spec_rom0), NULL);

    // The two fixed windows never move. Mapped every time anyway, because
    // the cost is two stores on a path taken a few hundred times a second
    // and the alternative is a special case that can be got wrong.
    z80_map_page(1, spec_ram[5], spec_ram[5]);
    z80_map_page(2, spec_ram[2], spec_ram[2]);

    const uint8_t bank = v & 0x07;
    z80_map_page(3, spec_ram[bank], spec_ram[bank]);

    spec_screen_bank = (v & 0x08) ? 7 : 5;
}

// ============================================================================
// Ports
// ============================================================================
/*
 * Two ports, and the decode is the interesting part of both.
 *
 * Nothing on a Spectrum decodes an address fully. The ULA answers every
 * port with A0 low — 0xFE is merely the one the ROM happens to use — and
 * the 128K's paging latch answers every port with A15 and A1 both low.
 * Programs rely on this: half the loaders in existence write the border
 * through 0xFF or 0x7E rather than 0xFE.
 *
 * The port argument is the whole 16-bit address, which matters twice
 * over. The keyboard is read by pulling one of A8-A15 low and seeing
 * which of the five bits comes back clear, and the paging latch is
 * selected by A15/A1. Neither is visible in the low byte alone.
 */
static void spec_io_out(const uint16_t port, const uint8_t value) {
    if ((port & 0x0001) == 0) {                 // the ULA
        spec_border = value & 0x07;

        /*
         * The beeper, and it is not a metaphor.
         *
         * Bit 4 of this port is the speaker on a real Spectrum, driven
         * straight from the ULA's latch. GP46 on this board is the
         * speaker too. So the real Z80 executing a real OUT reaches a
         * real transducer through one store — no sample buffer, no DAC,
         * no mixer, and no question about the sample rate, because there
         * are no samples. The waveform is whatever the CPU makes it,
         * which is exactly what 1-bit audio is.
         */
        gpio_put(SPEAKER_PIN, (value >> 4) & 1);
    }

    if ((port & 0x8002) == 0 && !paging_locked) {   // the 128K paging latch
        spec_port_7ffd = value;
        if (value & 0x20) paging_locked = true;
        apply_paging();
    }
}

static uint8_t spec_io_in(const uint16_t port) {
    if ((port & 0x0001) == 0) {                 // the ULA: keyboard
        uint8_t result = 0x1F;                  // five keys, all up

        // A half-row is selected by its address line being LOW, and more
        // than one can be low at once — which is not a corner case, it is
        // how the ROM scans the whole keyboard with a single IN from
        // 0x00FE. So this ANDs every selected row rather than picking one.
        for (unsigned row = 0; row < 8; row++)
            if (!(port & (0x0100u << row))) result &= kbd[row];

        // Bit 6 is the tape input. Nothing is connected, and a real
        // machine with nothing connected reads it high.
        return result | 0xA0;
    }

    /*
     * Everything else is the floating bus, and 0xFF is the honest answer
     * for it here. On real hardware an unclaimed IN returns whatever the
     * ULA happened to be fetching, which a handful of effects depend on;
     * reproducing that needs the video and the CPU to share a clock, and
     * they do not on this board.
     */
    return 0xFF;
}

// ============================================================================
// Keyboard
// ============================================================================

void spec_key_set(const uint8_t row, const uint8_t bit, const bool pressed) {
    if (row > 7 || bit > 4) return;
    if (pressed) kbd[row] &= ~(1u << bit);
    else         kbd[row] |=  (1u << bit);
}

/*
 * The matrix, as the ULA sees it.
 *
 * Row order is address-line order — row 0 is A8 — and within a row bit 0
 * is the key nearest the outside of the keyboard. Both halves run inward
 * from the edges, which is why 1-5 and 0-6 look reversed with respect to
 * each other and are not a typo.
 */
enum { R_CSZXCV = 0, R_ASDFG, R_QWERT, R_12345, R_09876, R_POIUY, R_ENTER, R_SPACE };

typedef struct { uint8_t row, bit; } key_t;

static const key_t key_letters[26] = {
    /* A */ {R_ASDFG, 0}, /* B */ {R_SPACE, 4}, /* C */ {R_CSZXCV, 3},
    /* D */ {R_ASDFG, 2}, /* E */ {R_QWERT, 2}, /* F */ {R_ASDFG, 3},
    /* G */ {R_ASDFG, 4}, /* H */ {R_ENTER, 4}, /* I */ {R_POIUY, 2},
    /* J */ {R_ENTER, 3}, /* K */ {R_ENTER, 2}, /* L */ {R_ENTER, 1},
    /* M */ {R_SPACE, 2}, /* N */ {R_SPACE, 3}, /* O */ {R_POIUY, 1},
    /* P */ {R_POIUY, 0}, /* Q */ {R_QWERT, 0}, /* R */ {R_QWERT, 3},
    /* S */ {R_ASDFG, 1}, /* T */ {R_QWERT, 4}, /* U */ {R_POIUY, 3},
    /* V */ {R_CSZXCV, 4}, /* W */ {R_QWERT, 1}, /* X */ {R_CSZXCV, 2},
    /* Y */ {R_POIUY, 4}, /* Z */ {R_CSZXCV, 1},
};

static const key_t key_digits[10] = {
    /* 0 */ {R_09876, 0}, /* 1 */ {R_12345, 0}, /* 2 */ {R_12345, 1},
    /* 3 */ {R_12345, 2}, /* 4 */ {R_12345, 3}, /* 5 */ {R_12345, 4},
    /* 6 */ {R_09876, 4}, /* 7 */ {R_09876, 3}, /* 8 */ {R_09876, 2},
    /* 9 */ {R_09876, 1},
};

// How long a console keystroke is held. The ROM scans the keyboard once
// per frame and debounces across frames, so a press that lasts one frame
// is a press it can miss entirely.
#define KEY_HOLD_FRAMES 3

static struct { uint8_t row, bit; } held[3];
static unsigned held_count;
static unsigned held_frames;

static void hold(const uint8_t row, const uint8_t bit) {
    if (held_count >= count_of(held)) return;
    held[held_count].row = row;
    held[held_count].bit = bit;
    held_count++;
    spec_key_set(row, bit, true);
}

/*
 * Cursor keys arrive as three bytes, so this needs one byte of memory.
 *
 * A terminal sends ESC [ A for up. The Spectrum has no cursor keys at
 * all — they are CAPS SHIFT plus 5, 6, 7 and 8, which is what the 128K
 * menu reads — so the sequence is reassembled here and pressed as the
 * shifted digit. Two bytes of state rather than a parser, because there
 * is exactly one sequence shape worth recognising.
 */
static uint8_t esc_state;   // 0 = idle, 1 = seen ESC, 2 = seen ESC [

void spec_key_ascii(char c) {
    if (esc_state == 1) { esc_state = (c == '[') ? 2 : 0; return; }
    if (esc_state == 2) {
        esc_state = 0;
        if (held_count) return;
        held_frames = KEY_HOLD_FRAMES;
        hold(R_CSZXCV, 0);                      // CAPS SHIFT
        switch (c) {
            case 'A': hold(R_09876, 3); break;  // up    = CAPS + 7
            case 'B': hold(R_09876, 4); break;  // down  = CAPS + 6
            case 'C': hold(R_09876, 2); break;  // right = CAPS + 8
            case 'D': hold(R_12345, 4); break;  // left  = CAPS + 5
            default:  held_count = 0; held_frames = 0; break;
        }
        return;
    }
    if (c == 0x1B) { esc_state = 1; return; }

    if (held_count) return;                     // one keystroke at a time

    held_frames = KEY_HOLD_FRAMES;

    if (c >= 'a' && c <= 'z') { hold(key_letters[c - 'a'].row, key_letters[c - 'a'].bit); return; }
    if (c >= 'A' && c <= 'Z') {
        hold(R_CSZXCV, 0);                      // CAPS SHIFT
        hold(key_letters[c - 'A'].row, key_letters[c - 'A'].bit);
        return;
    }
    if (c >= '0' && c <= '9') { hold(key_digits[c - '0'].row, key_digits[c - '0'].bit); return; }

    switch (c) {
        case '\r': case '\n': hold(R_ENTER,  0); break;
        case ' ':             hold(R_SPACE,  0); break;
        // Backspace is CAPS SHIFT + 0 on a Spectrum; there is no key for it.
        case 0x7F: case '\b': hold(R_CSZXCV, 0); hold(R_09876, 0); break;
        case '.':             hold(R_SPACE,  1); hold(R_SPACE, 2); break;   // SYM+M
        case ',':             hold(R_SPACE,  1); hold(R_SPACE, 3); break;   // SYM+N
        case '"':             hold(R_SPACE,  1); hold(R_POIUY, 0); break;   // SYM+P
        default: held_frames = 0; break;
    }
}

void spec_key_tick(void) {
    if (!held_count) return;
    if (held_frames) { held_frames--; return; }

    for (unsigned i = 0; i < held_count; i++)
        spec_key_set(held[i].row, held[i].bit, false);
    held_count = 0;
}

// ============================================================================
// Bring-up and the frame loop
// ============================================================================

void spec_init(void) {
    memset(spec_ram, 0, sizeof spec_ram);
    memset((void *)kbd, 0x1F, sizeof kbd);      // every key up
    held_count = 0;

    esc_state      = 0;
    paging_locked  = false;
    spec_port_7ffd = 0;
    spec_border    = 7;
    spec_frames    = 0;
    apply_paging();

    z80_io_in  = spec_io_in;
    z80_io_out = spec_io_out;

    gpio_put(SPEAKER_PIN, 0);

    z80_reset();
}

void spec_run_frame(void) {
    /*
     * One frame, then the interrupt, and nothing in between.
     *
     * This is the whole main loop of the machine, and its shortness is
     * the point: an emulator spends a frame stepping a CPU and counting
     * contention, and this spends a frame letting a Z84C00 be a Z80.
     * The only thing the ULA owes it is an interrupt every 70908
     * T-states, and the only thing that has to be right is the count.
     */
    z80_run(SPEC_TSTATES_PER_FRAME - SPEC_INT_TSTATES);

    z80_set_int(true);
    z80_run(SPEC_INT_TSTATES);
    z80_set_int(false);

    const uint32_t f = spec_frames + 1;
    spec_frames = f;

    // The ULA inverts FLASH cells every 16 frames, so the full cycle is
    // 32 and works out at about two-thirds of a second.
    spec_flash = (f & 0x10) != 0;

    spec_key_tick();
}
