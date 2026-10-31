/*
 * frank-dino — an RP2350B acting as the whole machine around a real Z80
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-dino
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The block formats follow the TZX 1.20, PZX 1.0 and CSW 2 specifications;
 * the standard and Rodolfo Guerra timings are pico-speccy's
 * (https://github.com/drewpo28/pico-speccy, GPL-3.0), which has them from
 * ESPectrum by Víctor Iborra and David Crespo.
 */

/*
 * A tape, as the signal it would have put on the EAR line.
 *
 * The ROM trap in tape.c answers a call to LD-BYTES with the bytes and
 * skips the signal altogether, which is instant and is all a tape of
 * standard blocks needs. Everything else -- turbo loaders, Speedlock,
 * pulse trains, direct recordings, a WAV of a real cassette -- reads the
 * EAR bit itself, at its own timing, and has to be given the signal.
 *
 * This is the half that makes it. It walks a .tap, .tzx, .pzx or .csw
 * held whole in the PSRAM and turns it into pulses: a level and how many
 * T-states it lasts. It runs on core 0, between frames' worth of work,
 * and fills a ring that core 1 drains at each IN from the ULA -- so an IN
 * costs a compare, and nothing on the clock path ever parses a file.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum : uint8_t {
    PULSE_TAP = 0,
    PULSE_TZX,
    PULSE_PZX,
    PULSE_CSW,
} pulse_fmt_t;

/*
 * One pulse: bit 31 the EAR level, bits 0-30 the length in T-states at
 * 3.5 MHz. A pause is a pulse too, at whatever level it holds.
 */
#define PULSE_LEVEL     0x80000000u
#define PULSE_LEN_MASK  0x7FFFFFFFu

/* Where the generator stopped, when it stops. */
typedef enum : uint8_t {
    PULSE_RUNNING = 0,
    PULSE_STOPPED,          // a "stop the tape" record: the motor goes off
    PULSE_END,              // nothing more on the tape
} pulse_state_t;

/*
 * Start a tape. The image must stay where it is for as long as it is
 * playing; the generator reads it in place.
 */
bool pulse_open(const uint8_t *image, uint32_t len, pulse_fmt_t fmt);

/* Back to the start, or to block n. */
void pulse_rewind(void);
bool pulse_seek_block(uint32_t n);

/*
 * Make up to `max` pulses into out[]. Returns how many. Fewer than asked
 * means the generator stopped -- see pulse_state() for why.
 */
uint32_t pulse_fill(uint32_t *out, uint32_t max);

pulse_state_t pulse_state(void);

/* Let a stopped tape go again, from where it stopped. */
void pulse_resume(void);

/* The 48K's "stop the tape here" record is honoured only on a 48K. */
void pulse_set_is_48k(bool is_48k);

/* Rodolfo Guerra's ROMs load at about 2.6 times the standard speed. */
void pulse_set_rg_timings(bool on);

/* Which block is playing, and how many there are. */
uint32_t pulse_block_index(void);
uint32_t pulse_block_count(void);

/*
 * A line about block n for the tape browser: its type and, for a
 * standard header, the name in it. Returns false past the end.
 */
bool pulse_block_describe(uint32_t n, char *out, unsigned out_len);

/*
 * Whether every block is one the ROM trap could serve instantly: standard
 * data at the ROM's own timings, and nothing that only makes sense as a
 * signal. Turbo-speed data does not count -- a block recorded faster than
 * the ROM reads is there because the tape's own loader reads it, and that
 * loader never calls LD-BYTES.
 */
bool pulse_all_rom_loadable(void);
