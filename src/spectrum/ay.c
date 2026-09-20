/*
 * frank-dino — an RP2350B acting as the whole machine around a real Z80
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-dino
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "ay.h"

#include <string.h>

#include "dino.h"

/*
 * The amplitude table, and it is not a straight line.
 *
 * Each of the sixteen volume steps is about 1.5 dB below the one above,
 * so the bottom half of the range is nearly silent and the top two steps
 * are most of the output. A linear table sounds wrong in a specific and
 * recognisable way: quiet passages come up too loud and every envelope
 * decays like a ramp instead of a fade.
 *
 * These are the measured values that circulate as the V_Soft/Lion 17
 * table, which pico-speccy carries too.
 */
static const uint16_t ay_amp[16] = {
        0,   513,   828,  1239,  1923,  3238,  4926,  9110,
    10344, 17876, 24682, 30442, 38844, 47270, 56402, 65535,
};

void ay_reset(ay_t *ay) {
    memset(ay, 0, sizeof *ay);
    ay->noise_shift = 1;            // any non-zero seed; zero is a dead LFSR
    ay->reg[7]      = 0x3F;         // everything disabled, as after power-on
}

void ay_select(ay_t *ay, const uint8_t reg) { ay->latched = reg & 0x0F; }

uint8_t ay_read(const ay_t *ay) {
    static const uint8_t mask[16] = {
        0xFF, 0x0F, 0xFF, 0x0F, 0xFF, 0x0F, 0x1F, 0xFF,
        0x1F, 0x1F, 0x1F, 0xFF, 0xFF, 0x0F, 0xFF, 0xFF,
    };
    return ay->reg[ay->latched] & mask[ay->latched];
}

void ay_write(ay_t *ay, const uint8_t value) {
    const uint8_t r = ay->latched;
    ay->reg[r] = value;

    /*
     * Writing the shape register restarts the envelope, and writing the
     * same value again restarts it again. That is not an implementation
     * detail to skip: a great deal of AY music retriggers an envelope
     * every note by rewriting R13, and a model that only acts on a
     * *change* plays those notes as one long fade.
     */
    if (r == 13) {
        ay->env_step    = 0;
        ay->env_count   = 0;
        ay->env_holding = false;
        ay->env_rising  = (value & 4) != 0;      // ATT picks the first direction
        ay->env_level   = ay->env_rising ? 0 : 15;
    }
}

// ---------------------------------------------------------------------------
// The envelope
// ---------------------------------------------------------------------------
/*
 * Four bits, not sixteen shapes.
 *
 * CONT (bit 3), ATT (2), ALT (1), HOLD (0). The generator counts a
 * 32-step ramp; ATT says whether it counts up or down, and what happens
 * at the end of the ramp is decided by the other three:
 *
 *   CONT = 0   one ramp, then sit at silence for ever. (The eight shapes
 *              with CONT clear are all this, regardless of the rest.)
 *   HOLD = 1   one ramp, then hold — at the end it reached, unless ALT
 *              says to flip first.
 *   otherwise  repeat, flipping direction each time if ALT is set.
 */
__force_inline static void env_step(ay_t *ay) {
    if (ay->env_holding) return;

    const uint8_t shape = ay->reg[13] & 0x0F;
    const bool cont = shape & 8, alt = shape & 2, hold = shape & 1;

    if (ay->env_step < 31) {
        ay->env_step++;
    } else {
        if (!cont) {
            // CONT clear: one ramp, then silence for ever, whatever the
            // other three bits say. That is all eight of those shapes.
            ay->env_holding = true;
            ay->env_level   = 0;
            return;
        }
        if (alt)  ay->env_rising = !ay->env_rising;
        if (hold) ay->env_holding = true;
        else      ay->env_step = 0;
    }

    ay->env_level = ay->env_rising ? (uint8_t)(ay->env_step >> 1)
                                   : (uint8_t)(15 - (ay->env_step >> 1));
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

int32_t ay_render(ay_t *ay, const uint32_t ticks) {
    // The counters step at the chip clock over sixteen; work in those
    // units and keep the remainder in the chip, not in a static.
    const uint32_t tsum = ticks + ay->tone_rem;
    const uint32_t tone_ticks = tsum / AY_TONE_DIV;
    ay->tone_rem = tsum % AY_TONE_DIV;

    const uint32_t esum = ticks + ay->env_rem;
    uint32_t envt = esum / AY_ENV_DIV;
    ay->env_rem = esum % AY_ENV_DIV;

    while (envt--) {
        const uint16_t period = (uint16_t)(ay->reg[11] | (ay->reg[12] << 8));
        if (++ay->env_count >= (period ? period : 1)) {
            ay->env_count = 0;
            env_step(ay);
        }
    }

    const uint8_t mixer = ay->reg[7];

    /*
     * Average across the slice rather than sampling at the end of it.
     *
     * At 44.1 kHz a sample covers about two and a half tone ticks, and a
     * channel near the top of its range flips more often than that.
     * Taking the level at one instant turns those into an aliased tone
     * somewhere else entirely; averaging what the channel actually did
     * across the slice is a crude low-pass, but it is the difference
     * between a high note and a wrong note.
     */
    /*
     * Decode the registers once, not once per step.
     *
     * The CPU is stopped for the whole of this slice, so nothing here can
     * change while the loop runs — and the loop runs two or three times
     * per sample, 44100 times a second, twice over for TurboSound.
     * Unpacking twelve-bit periods out of register pairs inside it cost
     * about a tenth of the Z80's clock rate, which showed up as the
     * machine running at 3.1 MHz instead of 3.5.
     */
    uint16_t period[3];
    uint8_t  vol[3];
    bool     tone_on[3], noise_on[3];
    for (int c = 0; c < 3; c++) {
        const uint16_t p = (uint16_t)((ay->reg[c * 2] | (ay->reg[c * 2 + 1] << 8)) & 0x0FFF);
        // A period of 0 behaves as 1 on real silicon, not as a divide by
        // zero and not as silence.
        period[c]   = p ? p : 1;
        tone_on[c]  = !(mixer & (1u << c));
        noise_on[c] = !(mixer & (8u << c));
        const uint8_t amp = ay->reg[8 + c];
        vol[c] = (amp & 0x10) ? ay->env_level : (amp & 0x0F);
    }
    const uint8_t np = ay->reg[6] & 0x1F;
    const uint16_t nperiod = (uint16_t)((np ? np : 1) * 2u);

    int32_t acc = 0;
    const uint32_t steps = tone_ticks ? tone_ticks : 1;

    for (uint32_t t = 0; t < steps; t++) {
        if (tone_ticks) {
            for (int c = 0; c < 3; c++)
                if (++ay->tone_count[c] >= period[c]) {
                    ay->tone_count[c] = 0;
                    ay->tone_flip[c] ^= 1;
                }

            if (++ay->noise_count >= nperiod) {
                ay->noise_count = 0;
                /*
                 * A 17-bit LFSR tapped at bits 0 and 3. Not a random
                 * number generator standing in for one: the AY's noise
                 * has a character that comes from this exact polynomial,
                 * and anything else sounds like hiss rather than like an AY.
                 */
                const uint32_t bit = ((ay->noise_shift ^ (ay->noise_shift >> 3)) & 1u);
                ay->noise_shift = (ay->noise_shift >> 1) | (bit << 16);
                ay->noise_bit   = (uint8_t)(ay->noise_shift & 1u);
            }
        }

        for (int c = 0; c < 3; c++) {
            // Tone AND noise, with the mixer bits *disabling* rather than
            // enabling — a set bit is off. Both disabled is a steady
            // level, which is how envelope-only percussion works and why
            // it must not be silenced.
            const bool level = (tone_on[c]  ? ay->tone_flip[c] : 1) &&
                               (noise_on[c] ? ay->noise_bit    : 1);
            if (level) acc += ay_amp[vol[c]];
        }
    }

    // Three channels at full scale would be 3 * 65535; bring that back to
    // something the mixer can add the beeper to without clipping on the
    // first loud chord.
    return (int32_t)(acc / (int32_t)steps / 8);
}
