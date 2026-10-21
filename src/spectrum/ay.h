/*
 * frank-dino — an RP2350B acting as the whole machine around a real Z80
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-dino
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * The AY-3-8912, twice.
 *
 * Three square-wave channels, one noise source, one envelope generator
 * and sixteen registers. The generator in ay.c is libayemu's, by way of
 * ESPectrum and pico-speccy -- its tables, its counter semantics, its
 * noise polynomial and its envelope rows. What is here is the shape
 * around it: a struct per chip, because TurboSound is two of these
 * sharing a pair of ports with writes of 0xFF and 0xFE to the select
 * port choosing which one is listening.
 *
 * The two things easiest to get subtly wrong, and the reason for using
 * somebody else's numbers: the amplitude table is not linear, and the
 * envelope is thirty-two levels rather than the sixteen the datasheet's
 * description suggests.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

// 1.7734 MHz: the 128K's CPU clock divided by two, which is how the AY is
// clocked on every Sinclair machine that has one. Getting this wrong
// transposes everything the chip plays.
#define AY_CLOCK_HZ 1773400u

/*
 * Every counter in the chip is clocked at the chip clock over eight.
 *
 * The datasheet quotes results rather than mechanism -- tone frequency
 * fclock/(16*TP), noise fclock/(16*NP), envelope fclock/(256*EP) -- and
 * it is easy to read those as the divisors. They are not. The counters
 * all run at fclock/8; a tone *toggles* its output when its counter
 * reaches TP, so a full cycle is 2*TP counts and the frequency works out
 * as fclock/(16*TP). The envelope's quoted figure is the rate of the
 * whole 32-step cycle, so its step rate is 32 times that, which is
 * fclock/(8*EP) -- a counter at fclock/8 expiring every EP.
 *
 * Taking 16 and 256 as the divisors puts every tone an octave low and
 * every envelope four times too slow. It is hard to hear and trivial to
 * measure, which is what ay_selftest() is for: it reported 125 Hz where
 * arithmetic said 260.18.
 */
#define AY_COUNTER_DIV 8u

/*
 * Where the three channels sit between the speakers.
 *
 * ABC is the Pentagon's and most Russian clones': A left, B in the
 * middle, C right. ACB swaps the last two, as Melodik and some Western
 * add-ons wired it, and a tune written for one sounds lopsided on the
 * other. Mono is all three in both, which is what a Sinclair 128 itself
 * did.
 */
typedef enum : uint8_t {
    AY_STEREO_MONO = 0,
    AY_STEREO_ABC,
    AY_STEREO_ACB,
    AY_STEREO_COUNT,
} ay_stereo_t;

typedef struct {
    uint8_t  reg[16];
    uint8_t  latched;              // which register the select port named

    // libayemu's state, one set per chip: three tone counters and their
    // output bits, the noise counter and its LFSR, and where the
    // envelope has got to along its 128-position row.
    uint16_t cnt[3];
    uint8_t  bit[3];
    uint16_t cnt_n;
    uint8_t  bit_n;
    uint32_t seed;
    uint16_t cnt_e;
    uint16_t env_pos;

    // The chip-clock remainder carried between calls, and the last value
    // returned. Per chip, not static: TurboSound has two of these, and
    // sharing either would make one chip's output depend on the other.
    uint32_t tone_rem;
    int32_t  last_l, last_r;
} ay_t;

void ay_reset(ay_t *ay);

/* Register select and data, as the ports deliver them. */
void    ay_select(ay_t *ay, uint8_t reg);
void    ay_write(ay_t *ay, uint8_t value);
uint8_t ay_read(const ay_t *ay);

/*
 * Advance by `ticks` chip clocks and write the left and right mixes of
 * the three channels, panned as `stereo` says, each roughly 0..12287
 * before the caller scales it.
 *
 * Called once per audio sample with the number of AY cycles that sample
 * covers, so the chip's clock and the Z80's stay locked to each other.
 * Internally it steps the counters at the chip's own rate and averages
 * the result across the sample, which is what stops a 4 kHz tone from
 * aliasing into something else at 44.1 kHz.
 *
 * A side's peak is the same in every mode -- a hard-panned channel
 * counts double on its own side -- so changing the panning never
 * changes the headroom.
 */
void ay_render_stereo(ay_t *ay, uint32_t ticks, ay_stereo_t stereo,
                      int32_t *left, int32_t *right);

/* The same, mono: one sample of all three channels in the middle. */
int32_t ay_render(ay_t *ay, uint32_t ticks);

/*
 * Check the model against arithmetic, with no audio hardware involved.
 *
 * Programs a known tone period, renders a tenth of a second, and counts
 * the transitions in what comes out. The expected frequency is just
 * clock/(16*period), so this either matches or it does not — and unlike
 * listening to the board, it cannot be confused by a keyclick, a mixer
 * setting or a BASIC editor that will not take an ENTER.
 *
 * Fills `detail` either way.
 */
bool ay_selftest(char *detail, unsigned detail_len);
