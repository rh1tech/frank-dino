/*
 * frank-dino — an RP2350B acting as the whole machine around a real Z80
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-dino
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * ---------------------------------------------------------------------
 * The generator is libayemu's, by Sashnov Alexander and Roman Scherbakov,
 * as carried by ESPectrum and pico-speccy in src/AySound.cpp. The tables,
 * the counter semantics, the noise polynomial and the envelope model are
 * all theirs; what is this firmware's is the shape around them -- one
 * struct per chip so TurboSound can have two, and a render call that
 * takes the cycles a single audio sample covers.
 *
 * It replaces a version written from the datasheet, which was a mistake
 * and sounded like one. The datasheet describes a 16-step envelope and
 * the chip as it is actually modelled has 32 amplitude levels, so
 * envelope-driven bass -- which is most of what the AY is used for in
 * Russian music, and most of what anyone notices -- came out as a
 * sixteen-level staircase and a subtly wrong timbre. Copying the
 * numbers that everyone else's output is compared against is worth more
 * than deriving them again.
 * ---------------------------------------------------------------------
 */

#include "ay.h"

#include <stdio.h>
#include <string.h>

#include "audio.h"
#include "dino.h"

/*
 * Thirty-two amplitude levels, 1.5 dB apart, tied to silence at zero.
 *
 * libayemu generates these at start-up as max/2^((v^31)/6.36) with
 * table[0] subtracted so level zero really is zero; they are precomputed
 * here because this firmware has no reason to carry powf() and because a
 * table is easier to compare against theirs than a formula is.
 *
 * Normalised so the top step is 65535, which is the scale the mixer in
 * spectrum.c was built around.
 */
static const uint16_t ay_amp[32] = {
        0,   266,   563,   895,  1264,  1676,  2135,  2648,
     3219,  3856,  4566,  5359,  6242,  7227,  8326,  9551,
    10917, 12440, 14139, 16033, 18146, 20502, 23129, 26058,
    29325, 32968, 37031, 41561, 46613, 52247, 58529, 65535,
};

/*
 * The volume register is four bits and the table is five.
 *
 * Not simply doubled: level 0 is silence and the rest sit on the odd
 * steps, which is libayemu's Rampa_AY_table and is what makes a fixed
 * volume of n sit exactly between envelope steps 2n and 2n+2.
 */
static const uint8_t vol_to_amp[16] = { 0,3,5,7,9,11,13,15,17,19,21,23,25,27,29,31 };

/*
 * The sixteen envelope shapes, as positions rather than a state machine.
 *
 * Four bits of the shape register select a row; the envelope counter
 * walks along it and wraps from 127 back to 64, which is what makes the
 * eight repeating shapes repeat and the eight one-shot ones hold. A
 * state machine with CONT, ATT, ALT and HOLD flags describes the same
 * thing and is much easier to get wrong -- this is a table lookup and
 * cannot be.
 */
static const uint8_t envelope[16][128] = {
    {
      31,30,29,28,27,26,25,24,23,22,21,20,19,18,17,16,15,14,13,12,11,10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0,
       0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
       0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
       0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    {
      31,30,29,28,27,26,25,24,23,22,21,20,19,18,17,16,15,14,13,12,11,10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0,
       0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
       0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
       0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    {
      31,30,29,28,27,26,25,24,23,22,21,20,19,18,17,16,15,14,13,12,11,10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0,
       0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
       0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
       0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    {
      31,30,29,28,27,26,25,24,23,22,21,20,19,18,17,16,15,14,13,12,11,10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0,
       0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
       0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
       0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    {
       0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,
       0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
       0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
       0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    {
       0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,
       0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
       0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
       0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    {
       0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,
       0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
       0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
       0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    {
       0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,
       0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
       0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
       0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    {
      31,30,29,28,27,26,25,24,23,22,21,20,19,18,17,16,15,14,13,12,11,10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0,
      31,30,29,28,27,26,25,24,23,22,21,20,19,18,17,16,15,14,13,12,11,10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0,
      31,30,29,28,27,26,25,24,23,22,21,20,19,18,17,16,15,14,13,12,11,10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0,
      31,30,29,28,27,26,25,24,23,22,21,20,19,18,17,16,15,14,13,12,11,10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0 },
    {
      31,30,29,28,27,26,25,24,23,22,21,20,19,18,17,16,15,14,13,12,11,10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0,
       0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
       0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
       0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    {
      31,30,29,28,27,26,25,24,23,22,21,20,19,18,17,16,15,14,13,12,11,10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0,
       0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,
      31,30,29,28,27,26,25,24,23,22,21,20,19,18,17,16,15,14,13,12,11,10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0,
       0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31 },
    {
      31,30,29,28,27,26,25,24,23,22,21,20,19,18,17,16,15,14,13,12,11,10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0,
      31,31,31,31,31,31,31,31,31,31,31,31,31,31,31,31,31,31,31,31,31,31,31,31,31,31,31,31,31,31,31,31,
      31,31,31,31,31,31,31,31,31,31,31,31,31,31,31,31,31,31,31,31,31,31,31,31,31,31,31,31,31,31,31,31,
      31,31,31,31,31,31,31,31,31,31,31,31,31,31,31,31,31,31,31,31,31,31,31,31,31,31,31,31,31,31,31,31 },
    {
       0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,
       0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,
       0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,
       0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31 },
    {
       0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,
      31,31,31,31,31,31,31,31,31,31,31,31,31,31,31,31,31,31,31,31,31,31,31,31,31,31,31,31,31,31,31,31,
      31,31,31,31,31,31,31,31,31,31,31,31,31,31,31,31,31,31,31,31,31,31,31,31,31,31,31,31,31,31,31,31,
      31,31,31,31,31,31,31,31,31,31,31,31,31,31,31,31,31,31,31,31,31,31,31,31,31,31,31,31,31,31,31,31 },
    {
       0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,
      31,30,29,28,27,26,25,24,23,22,21,20,19,18,17,16,15,14,13,12,11,10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0,
       0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,
      31,30,29,28,27,26,25,24,23,22,21,20,19,18,17,16,15,14,13,12,11,10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0 },
    {
       0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,
       0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
       0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
       0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
};

void ay_reset(ay_t *ay) {
    memset(ay, 0, sizeof *ay);
    ay->seed   = 0xFFFFu;           // libayemu's; any non-zero seed will do
    ay->reg[7] = 0x3F;              // everything disabled, as after power-on
}

void ay_select(ay_t *ay, const uint8_t reg) { ay->latched = reg & 0x0F; }

void ay_write(ay_t *ay, const uint8_t value) {
    ay->reg[ay->latched] = value;

    // Writing the shape register restarts the envelope, whatever it was
    // doing. That is the whole of how a tune retriggers a note.
    if (ay->latched == 13) { ay->env_pos = 0; ay->cnt_e = 0; }
}

uint8_t ay_read(const ay_t *ay) { return ay->reg[ay->latched]; }

// ---------------------------------------------------------------------------

/*
 * How much of each channel goes to each side.
 *
 * Every side's weights add up to three, so a side at full scale is
 * always three channels' worth -- exactly what the mono mix was -- and
 * the scaling below holds for all of them. A centred channel is one in
 * each; a hard-panned one is two on its own side and none on the other.
 */
static const uint8_t pan[AY_STEREO_COUNT][2][3] = {
    [AY_STEREO_MONO] = { { 1, 1, 1 }, { 1, 1, 1 } },
    [AY_STEREO_ABC]  = { { 2, 1, 0 }, { 0, 1, 2 } },
    [AY_STEREO_ACB]  = { { 2, 0, 1 }, { 0, 2, 1 } },
    [AY_STEREO_BAC]  = { { 1, 2, 0 }, { 1, 0, 2 } },
    [AY_STEREO_CBA]  = { { 0, 1, 2 }, { 2, 1, 0 } },
};

void __not_in_flash_func(ay_render_stereo)(ay_t *ay, const uint32_t ticks,
                                           const ay_stereo_t stereo,
                                           int32_t *const left, int32_t *const right) {
    /*
     * Chip clocks to counter ticks, with the remainder kept per chip.
     *
     * Every counter in the AY runs at the chip clock over eight -- see
     * ay.h for why the datasheet's 16 and 256 are results and not
     * divisors. libayemu truncates this to a constant per sample; the
     * remainder is carried here instead, which is the one place this
     * differs from theirs and it only makes the pitch more exact.
     */
    const uint32_t tsum  = ticks + ay->tone_rem;
    const uint32_t tacts = tsum / AY_COUNTER_DIV;
    ay->tone_rem = tsum % AY_COUNTER_DIV;

    if (!tacts) { *left = ay->last_l; *right = ay->last_r; return; }

    /*
     * Everything the inner loop needs, decoded once.
     *
     * The CPU is stopped for the whole of this slice, so none of it can
     * change while the loop runs -- and the loop runs a few times per
     * sample, 44100 times a second, twice over for TurboSound. Unpacking
     * twelve-bit periods out of register pairs inside it once cost about
     * a tenth of the Z80's clock rate.
     */
    const uint8_t  *env_row = envelope[ay->reg[13] & 0x0F];
    const uint16_t  tone[3] = {
        (uint16_t)((ay->reg[0] | (ay->reg[1] << 8)) & 0x0FFF),
        (uint16_t)((ay->reg[2] | (ay->reg[3] << 8)) & 0x0FFF),
        (uint16_t)((ay->reg[4] | (ay->reg[5] << 8)) & 0x0FFF),
    };
    const uint16_t noise2   = (uint16_t)((ay->reg[6] & 0x1F) * 2u);
    const uint16_t env_freq = (uint16_t)(ay->reg[11] | (ay->reg[12] << 8));

    const uint8_t mixer = ay->reg[7];
    const bool t_on[3] = { !(mixer & 1), !(mixer & 2), !(mixer & 4) };
    const bool n_on[3] = { !(mixer & 8), !(mixer & 16), !(mixer & 32) };

    uint8_t vol[3], env_on[3];
    for (int c = 0; c < 3; c++) {
        vol[c]    = (uint8_t)(ay->reg[8 + c] & 0x0F);
        env_on[c] = (uint8_t)(ay->reg[8 + c] & 0x10);
    }

    uint16_t cnt[3] = { ay->cnt[0], ay->cnt[1], ay->cnt[2] };
    uint8_t  bit[3] = { ay->bit[0], ay->bit[1], ay->bit[2] };
    uint16_t cnt_n = ay->cnt_n, cnt_e = ay->cnt_e;
    uint8_t  bit_n = ay->bit_n;
    uint16_t env_pos = ay->env_pos;
    uint32_t seed = ay->seed;

    int32_t acc[3] = { 0, 0, 0 };

    for (uint32_t m = 0; m < tacts; m++) {
        // libayemu's counters: count up, and on reaching the period
        // reset and toggle. A period of zero toggles every tick, which
        // is what the silicon does and is not a special case.
        for (int c = 0; c < 3; c++)
            if (++cnt[c] >= tone[c]) { cnt[c] = 0; bit[c] ^= 1; }

        if (++cnt_n >= noise2) {
            cnt_n = 0;
            // A 17-bit LFSR with taps at 16 and 13. The AY's noise has a
            // character that comes from this exact polynomial; anything
            // else is hiss.
            seed = (seed * 2u + 1u) ^ (((seed >> 16) ^ (seed >> 13)) & 1u);
            bit_n = (uint8_t)((seed >> 16) & 1u);
        }

        if (++cnt_e >= env_freq) {
            cnt_e = 0;
            if (++env_pos > 127) env_pos = 64;
        }

        for (int c = 0; c < 3; c++) {
            // The mixer bits *disable*: a set bit is off. Both disabled
            // is a steady level, which is how envelope-only percussion
            // works and why it must not be silenced.
            if ((bit[c] | !t_on[c]) & (bit_n | !n_on[c]))
                acc[c] += ay_amp[env_on[c] ? env_row[env_pos] : vol_to_amp[vol[c]]];
        }
    }

    for (int c = 0; c < 3; c++) { ay->cnt[c] = cnt[c]; ay->bit[c] = bit[c]; }
    ay->cnt_n = cnt_n; ay->cnt_e = cnt_e; ay->bit_n = bit_n;
    ay->env_pos = env_pos; ay->seed = seed;

    /*
     * Averaged across the sample, then given an eighth of the range.
     *
     * At 44.1 kHz a sample covers about five counter ticks and a channel
     * near the top of its range flips more often than that, so taking
     * the level at one instant aliases a high note into a wrong one.
     * Averaging is a crude low-pass and is the difference between the
     * two.
     *
     * Three channels at full scale is 3 x 65535. Two chips of that plus
     * the beeper has to stay inside a signed 16-bit sample, so each chip
     * gets an eighth: 196605/16 is 12287, twice that is 24574, and the
     * beeper's +/-6000 on top reaches 30574 -- just under 32767.
     *
     * Per side, now: the pan table's weights add up to three on each
     * side, so a side reaches the same 12287 the mono mix did.
     */
    const uint8_t (*const p)[3] = pan[stereo < AY_STEREO_COUNT ? stereo : AY_STEREO_MONO];
    const int32_t l = acc[0] * p[0][0] + acc[1] * p[0][1] + acc[2] * p[0][2];
    const int32_t r = acc[0] * p[1][0] + acc[1] * p[1][1] + acc[2] * p[1][2];
    const int32_t div = (int32_t)tacts * 16;
    *left  = ay->last_l = l / div;
    *right = ay->last_r = r / div;
}

int32_t ay_render(ay_t *ay, const uint32_t ticks) {
    int32_t l, r;
    ay_render_stereo(ay, ticks, AY_STEREO_MONO, &l, &r);
    return l;
}

// ---------------------------------------------------------------------------
// Proving it
// ---------------------------------------------------------------------------
/*
 * Check the model against arithmetic, with no audio hardware involved.
 *
 * It caught a real fault once -- the counters were being run at the
 * datasheet's quoted divisors instead of at the chip clock over eight,
 * which put everything an octave low -- and it is the thing that says
 * whether a change to the generator has moved the pitch. Listening to
 * the board cannot: a third of a semitone is inaudible on its own and
 * obvious across a whole tune.
 */
bool ay_selftest(char *detail, const unsigned detail_len) {
    // 426 puts the tone at 1773400/(16*426) = 260.2 Hz, near middle C and
    // comfortably inside the range 44100 Hz can resolve without the
    // averaging in ay_render() smearing it.
    const uint16_t period = 426;
    const uint32_t expect_mhz = AY_CLOCK_HZ * 1000u / (16u * period);   // milli-Hz

    ay_t chip;
    ay_reset(&chip);
    ay_select(&chip, 0);  ay_write(&chip, (uint8_t)(period & 0xFF));
    ay_select(&chip, 1);  ay_write(&chip, (uint8_t)(period >> 8));
    ay_select(&chip, 7);  ay_write(&chip, 0x3E);   // tone A only; every bit is a disable
    ay_select(&chip, 8);  ay_write(&chip, 0x0F);   // channel A at full volume

    /*
     * Half a second, and the same fractional cycle count per sample that
     * the frame loop uses.
     *
     * Half rather than a tenth because the answer is counted in zero
     * crossings: at 260 Hz a tenth of a second is 52 of them, so one
     * miscounted crossing is two per cent and the test cannot tell a
     * tuning error from its own resolution. It reported 255 Hz against
     * an expected 260.18 for exactly that reason, and the tuning was
     * fine.
     */
    const uint32_t samples = AUDIO_RATE_HZ / 2u;
    const uint32_t step_q16 = (uint32_t)(((uint64_t)AY_CLOCK_HZ << 16) / AUDIO_RATE_HZ);
    uint32_t frac = 0;

    int32_t  prev = 0;
    uint32_t crossings = 0;
    bool     high = false;
    int32_t  peak = 0;

    for (uint32_t i = 0; i < samples; i++) {
        frac += step_q16;
        const uint32_t ticks = frac >> 16;
        frac &= 0xFFFFu;

        const int32_t v = ay_render(&chip, ticks);
        if (v > peak) peak = v;

        // Threshold at half the peak seen so far, which a square wave
        // crosses exactly twice a cycle and noise would not.
        const int32_t mid = peak / 2;
        const bool now = v > mid;
        if (i && now != high) crossings++;
        high = now;
        prev = v;
    }
    (void)prev;

    const uint32_t got_mhz = crossings * 1000u / 2u * 2u;   // /2 cycles, x2 for half a second

    const uint32_t lo = expect_mhz - expect_mhz / 100u, hi = expect_mhz + expect_mhz / 100u;
    if (got_mhz < lo || got_mhz > hi) {
        snprintf(detail, detail_len, "period %u gave %lu.%02lu Hz, wanted %lu.%02lu",
                 period, (unsigned long)(got_mhz / 1000), (unsigned long)((got_mhz % 1000) / 10),
                 (unsigned long)(expect_mhz / 1000), (unsigned long)((expect_mhz % 1000) / 10));
        return false;
    }

    snprintf(detail, detail_len, "tone %lu.%02lu Hz from period %u (wanted %lu.%02lu)",
             (unsigned long)(got_mhz / 1000), (unsigned long)((got_mhz % 1000) / 10), period,
             (unsigned long)(expect_mhz / 1000), (unsigned long)((expect_mhz % 1000) / 10));
    return true;
}
