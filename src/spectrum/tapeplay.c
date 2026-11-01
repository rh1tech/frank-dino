/*
 * frank-dino — an RP2350B acting as the whole machine around a real Z80
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-dino
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "tapeplay.h"
#include "z80_bus.h"

#include "hardware/sync.h"

volatile bool tapeplay_active;

/*
 * The pulses on their way from core 0 to core 1.
 *
 * Eight kilobytes: half a second of the busiest signal a tape carries --
 * data at four thousand pulses a second -- which is a hundred times the
 * gap between two visits of core 0's main loop.
 */
#define RING 2048u
static uint32_t          ring[RING];
static volatile uint32_t r_head, r_tail;     // head: core 0's; tail: core 1's

static volatile bool          motor;
static volatile pulse_state_t gen_state;     // the generator, as core 0 last saw it
static volatile bool          resume_req;    // core 1 asks core 0 to run on past a stop
static volatile uint32_t      underruns;

// ---------------------------------------------------------------------------
// Core 1: the tape's clock, in the machine's T-states
// ---------------------------------------------------------------------------
static uint32_t base;           // the tape clock at T-state 0 of this frame
static uint32_t seg_end;        // ...when the current pulse ends
static bool     seg_level;
static uint32_t remain;         // what was left of the pulse when the motor stopped
static uint32_t scale = 2;      // the turbo multiplier, over two
static uint32_t reads;          // INs of #FE this frame
static uint32_t quiet;          // frames in a row with hardly any
static bool     stopped_by_tape;

/*
 * Pulses are lengths at 3.5 MHz, and a machine at twice the clock reads
 * twice as many T-states for the same length of tape.
 */
__force_inline static uint32_t scaled(const uint32_t e) {
    return ((e & PULSE_LEN_MASK) * scale) >> 1;
}

/* Bring the tape up to `now`. */
static void __not_in_flash_func(advance)(const uint32_t now) {
    while ((int32_t)(now - seg_end) >= 0) {
        const uint32_t tl = r_tail;
        if (tl == r_head) {
            if (gen_state != PULSE_RUNNING) {
                // The generator has nothing more: the tape is at a stop
                // record or at its end, and the motor goes off.
                motor = false;
                stopped_by_tape = gen_state == PULSE_STOPPED;
                remain = 0;
            } else {
                underruns++;
            }
            seg_end = now + 1u;
            return;
        }
        const uint32_t e = ring[tl & (RING - 1u)];
        __dmb();
        r_tail = tl + 1u;
        seg_level = (e & PULSE_LEVEL) != 0;
        seg_end  += scaled(e);
    }
}

bool __not_in_flash_func(tapeplay_ear)(const uint32_t t) {
    reads++;
    if (motor) advance(base + t);
    return seg_level;
}

void __not_in_flash_func(tapeplay_frame_end)(const uint32_t frame_tstates, const uint32_t turbo_num) {
    scale = turbo_num;
    const uint32_t end = base + frame_tstates;
    if (motor) advance(end);

    /*
     * The motor heuristic.
     *
     * A loader samples #FE every fifty-odd T-states while it waits for
     * an edge: well over a thousand times a frame. A program that is not
     * loading reads it for the keyboard, eight times a frame at most.
     * Three seconds of the latter stop the tape; one frame of the former
     * starts it.
     *
     * Except in the ROM's own loader, which counts for both whatever it
     * reads (pico-speccy decides by the PC too). Hunting for a pilot it
     * reads a handful of times and then sits a second in LD-WAIT, which
     * reads nothing -- over and over, under any read count. Judged by
     * reads alone the tape stopped under it, came back a frame after
     * LD-EDGE had already given up, and a real-time load died mid-block.
     */
    const uint16_t pc = z80_stats.last_opfetch_addr;
    const bool rom_loader = pc >= 0x0556u && pc < 0x0605u;
    if (motor) {
        quiet = (reads < 40u && !rom_loader) ? quiet + 1u : 0u;
        if (quiet >= 150u) {
            motor  = false;
            remain = (int32_t)(seg_end - end) > 0 ? seg_end - end : 0u;
            stopped_by_tape = false;
        }
    } else if ((reads >= 400u || rom_loader) && (gen_state == PULSE_RUNNING || stopped_by_tape
                                 || r_tail != r_head)) {
        if (stopped_by_tape) resume_req = true;
        stopped_by_tape = false;
        motor   = true;
        quiet   = 0;
        seg_end = end + remain;
    }

    base  = end;
    reads = 0;
}

// ---------------------------------------------------------------------------
// Core 0
// ---------------------------------------------------------------------------
static void ring_reset(void) {
    r_head = r_tail = 0;
    seg_end = base;
    seg_level = false;
    remain = 0;
}

bool tapeplay_mount(const uint8_t *image, const uint32_t len, const pulse_fmt_t fmt) {
    tapeplay_active = false;
    if (!pulse_open(image, len, fmt)) return false;
    base = 0;
    ring_reset();
    underruns = 0;
    quiet = 0;
    stopped_by_tape = false;
    gen_state = pulse_state();
    /*
     * The motor waits for a loader. Started here, it ran on while
     * LOAD "" was still being typed and took most of the header's pilot
     * with it; the heuristic below starts it on the first frame of a
     * loader, which is when a person would press PLAY.
     */
    motor = false;
    tapeplay_active = true;
    return true;
}

void tapeplay_eject(void) {
    tapeplay_active = false;
    motor = false;
}

void tapeplay_play(void) {
    if (pulse_state() == PULSE_STOPPED) pulse_resume();
    if (pulse_state() == PULSE_END && r_head == r_tail) { pulse_rewind(); ring_reset(); }
    gen_state = pulse_state();
    seg_end = base + remain;
    quiet = 0;
    motor = true;
}

void tapeplay_stop(void) { motor = false; }

void tapeplay_rewind(void) {
    pulse_rewind();
    ring_reset();
    gen_state = pulse_state();
}

bool tapeplay_seek(const uint32_t block) {
    if (!pulse_seek_block(block)) return false;
    ring_reset();
    gen_state = pulse_state();
    return true;
}

bool tapeplay_motor_on(void) { return motor; }

uint32_t tapeplay_underruns(void) { return underruns; }

void tapeplay_task(void) {
    if (!tapeplay_active) return;

    if (resume_req) {
        resume_req = false;
        pulse_resume();
    }

    for (int pass = 0; pass < 4; pass++) {
        const uint32_t h = r_head;
        const uint32_t room = RING - (h - r_tail);
        if (room < 64u || pulse_state() != PULSE_RUNNING) break;

        // Straight into the ring, as far as it goes before it wraps.
        const uint32_t at = h & (RING - 1u);
        uint32_t n = RING - at;
        if (n > room) n = room;
        const uint32_t got = pulse_fill(&ring[at], n);
        __dmb();
        r_head = h + got;
        if (got < n) break;
    }
    gen_state = pulse_state();
}
