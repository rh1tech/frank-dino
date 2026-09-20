/*
 * frank-dino — an RP2350B acting as the whole machine around a real Z80
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-dino
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "spectrum.h"

#include <string.h>
#include <pico/time.h>
#include <hardware/gpio.h>

#include "audio.h"
#include "ay.h"
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

// ---------------------------------------------------------------------------
// Sound
// ---------------------------------------------------------------------------
volatile bool     spec_beeper;
/*
 * TurboSound on by default.
 *
 * A real TurboSound board is always both chips; software that does not
 * know about it simply never writes 0xFE or 0xFF to the select port, and
 * the 128K ROM only ever names registers 0-13. Turning it off is for
 * chasing a program that does write one of those as a register number.
 */
volatile bool     spec_turbosound = true;
volatile uint32_t spec_ay_writes;

static ay_t    ay[2];
static uint8_t ay_sel;          // which chip the ports are talking to

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
         * The beeper. Bit 4 of this port is the speaker on a real
         * Spectrum, driven from the ULA's latch.
         *
         * This used to be a gpio_put() straight to GP46 — the CPU's own
         * edges reaching the transducer with nothing in between, which
         * is exactly what 1-bit audio is. The AY ended that: the pin is
         * a PWM DAC now and cannot also be a logic output, so the beeper
         * becomes a level that the mixer samples 44100 times a second
         * along with everything else.
         *
         * The loss is real but small. Core 1 runs the frame in
         * sample-sized slices and reads this between them, so an edge
         * still lands in the sample it happened in; what is gone is
         * sub-sample detail, which for a beeper mostly means the very
         * top of its range.
         */
        spec_beeper = (value & 0x10) != 0;
    }

    /*
     * The AY, on two ports that are both "A15 high".
     *
     * Register select is A15 and A14 both high (0xFFFD); data is A15 high
     * with A14 low (0xBFFD). As everywhere else on this machine the
     * decode is partial, and as everywhere else programs rely on it.
     */
    if ((port & 0xC002) == 0xC000) {            // select
        /*
         * TurboSound steals the top of the register range.
         *
         * 0xFF and 0xFE are not register numbers there, they pick which
         * of the two chips the next data writes reach. A single-chip
         * machine must treat them as ordinary selects of a register that
         * does not exist, which is why this only acts when the second
         * chip has been turned on.
         */
        if (spec_turbosound && (value & 0xFE) == 0xFE) {
            ay_sel = (uint8_t)(value & 1 ? 0 : 1);   // 0xFF = first, 0xFE = second
        } else {
            ay_select(&ay[ay_sel], value);
        }
        return;
    }
    if ((port & 0xC002) == 0x8000) {            // data
        ay_write(&ay[ay_sel], value);
        spec_ay_writes++;
        return;
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

    // The AY's read-back port is the select port, not the data one.
    if ((port & 0xC002) == 0xC000) return ay_read(&ay[ay_sel]);

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

    ay_reset(&ay[0]);
    ay_reset(&ay[1]);
    ay_sel         = 0;
    spec_beeper    = false;
    spec_ay_writes = 0;

    esc_state      = 0;
    paging_locked  = false;
    spec_port_7ffd = 0;
    spec_border    = 7;
    spec_frames    = 0;
    apply_paging();

    z80_io_in  = spec_io_in;
    z80_io_out = spec_io_out;

    z80_reset();
}

/*
 * One audio sample's worth of Z80, then one audio sample.
 *
 * A frame is 70908 T-states and 882 samples, which is 80.394 T-states
 * each — so the slice length alternates between 80 and 81 under a
 * fractional accumulator rather than being rounded, because rounding
 * 0.394 away 882 times a frame is 347 T-states of drift every 20 ms.
 */
#define SPEC_TSTATES_PER_SAMPLE (SPEC_TSTATES_PER_FRAME / AUDIO_SAMPLES_PER_FRAME)
#define SPEC_TSTATE_REMAINDER   (SPEC_TSTATES_PER_FRAME % AUDIO_SAMPLES_PER_FRAME)

// AY clocks per T-state, x256: the chip runs at half the CPU clock, so
// this is 128/256. Kept as a shift rather than a divide because it is on
// the per-sample path.
#define AY_TICKS(tstates) ((tstates) >> 1)

static uint32_t slice_acc;

__force_inline static void run_slice(const uint32_t tstates) {
    z80_run(tstates);

    /*
     * The mix, once per sample.
     *
     * The beeper is read here rather than pushed by the port handler, so
     * its level is whatever it was at the end of the slice — which is
     * the sample it belongs in. The AYs are advanced by exactly the
     * cycles this slice covered, so nothing drifts against anything.
     */
    int32_t s = ay_render(&ay[0], AY_TICKS(tstates));
    if (spec_turbosound) s += ay_render(&ay[1], AY_TICKS(tstates));

    /*
     * The beeper at about a quarter of full scale.
     *
     * On a real Spectrum the beeper is considerably louder than the AY,
     * and reproducing that faithfully makes every 128K game's music sit
     * under its own sound effects. This is the compromise every emulator
     * makes; it is a mix decision, not a measurement.
     */
    if (spec_beeper) s += 8000;
    else             s -= 8000;

    audio_push(s);
}

uint32_t spec_tune_clock(void) {
    // Muted, because the machine is being run at deliberately wrong
    // speeds and nobody needs to hear that.
    audio_set_enabled(false);

    // What five frames should take, in microseconds.
    const int64_t want = 5ll * 1000000ll * SPEC_TSTATES_PER_FRAME / SPEC_CPU_HZ;
    const int64_t tol  = want / 400;        // a quarter of a per cent

    int32_t pace = (int32_t)z80_get_pace();
    int64_t us   = want;

    /*
     * Proportional, then fine.
     *
     * Stepping by one and measuring five frames each time is 100 ms an
     * iteration, and the starting point comes from z80_calibrate(), which
     * timed the bus engine on its own and therefore lands a long way off
     * once two AY chips are in the loop. The first version walked ±1 and
     * ran out of its iteration budget mid-descent, leaving the machine
     * 1.9 % fast with the tuner reporting success.
     *
     * One pace unit is about two per cent of a frame, so the error in per
     * mille divided by twenty is roughly how many units to move. That
     * converges in three or four measurements; the ±1 fallback at the end
     * takes care of the last step, where the estimate rounds to nothing.
     */
    for (int iter = 0; iter < 16; iter++) {
        const uint64_t t0 = time_us_64();
        for (int f = 0; f < 5; f++) spec_run_frame();
        us = (int64_t)(time_us_64() - t0);

        const int64_t err = us - want;                  // >0 means too slow
        if (err > -tol && err < tol) break;

        int32_t step = (int32_t)((err * 1000 / want) / 20);
        if (step == 0) step = err > 0 ? 1 : -1;

        pace -= step;                                   // slower -> less padding
        if (pace < 0) pace = 0;
        z80_set_pace((uint32_t)pace);
    }

    /*
     * One more measurement, so the number returned is the one that is
     * true now.
     *
     * The loop's last `us` was taken *before* its last adjustment, which
     * made the report disagree with the machine — it announced 51.5 Hz
     * while the board was actually running 49.75. A figure that is wrong
     * in the direction of looking worse is still wrong, and this one is
     * printed on the boot screen where it will be believed.
     */
    const uint64_t t0 = time_us_64();
    for (int f = 0; f < 5; f++) spec_run_frame();
    us = (int64_t)(time_us_64() - t0);

    audio_set_enabled(true);
    return (uint32_t)((5ll * 1000000000ll) / (us ? us : 1));   // fps x1000
}

void spec_run_frame(void) {
    /*
     * The whole main loop of the machine, sliced.
     *
     * An emulator spends a frame stepping a CPU and counting contention.
     * This spends it letting a Z84C00 be a Z80, stopping 882 times to
     * take a sample. The only thing the ULA owes the CPU is an interrupt
     * every 70908 T-states, and the only thing that has to be right is
     * the count.
     */
    uint32_t budget = SPEC_TSTATES_PER_FRAME - SPEC_INT_TSTATES;

    for (uint32_t i = 0; i < AUDIO_SAMPLES_PER_FRAME; i++) {
        uint32_t slice = SPEC_TSTATES_PER_SAMPLE;
        slice_acc += SPEC_TSTATE_REMAINDER;
        if (slice_acc >= AUDIO_SAMPLES_PER_FRAME) {
            slice_acc -= AUDIO_SAMPLES_PER_FRAME;
            slice++;
        }

        if (slice > budget) slice = budget;
        budget -= slice;
        run_slice(slice);
        if (!budget) break;
    }

    if (budget) run_slice(budget);

    z80_set_int(true);
    run_slice(SPEC_INT_TSTATES);
    z80_set_int(false);

    const uint32_t f = spec_frames + 1;
    spec_frames = f;

    // The ULA inverts FLASH cells every 16 frames, so the full cycle is
    // 32 and works out at about two-thirds of a second.
    spec_flash = (f & 0x10) != 0;

    spec_key_tick();
}
