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
 * and sixteen registers. It is a small chip and this is a small model of
 * it — the parts that are easy to get subtly wrong are the amplitude
 * table, which is not linear, and the envelope shapes, which are four
 * bits describing a state machine rather than sixteen separate ramps.
 *
 * Two instances because TurboSound is two of these chips sharing a pair
 * of ports, with writes of 0xFF and 0xFE to the select port choosing
 * which one is listening.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

// 1.7734 MHz: the 128K's CPU clock divided by two, which is how the AY is
// clocked on every Sinclair machine that has one. Getting this wrong
// transposes everything the chip plays.
#define AY_CLOCK_HZ 1773400u

// The chip's own dividers. Tone and noise counters step at the chip clock
// over sixteen; the envelope steps at the chip clock over two hundred and
// fifty six. Both are properties of the silicon, not choices.
#define AY_TONE_DIV 16u
#define AY_ENV_DIV  256u

typedef struct {
    uint8_t  reg[16];
    uint8_t  latched;              // which register the select port named

    uint16_t tone_count[3];
    uint8_t  tone_flip[3];         // the square wave's current half

    uint16_t noise_count;
    uint32_t noise_shift;          // 17-bit LFSR
    uint8_t  noise_bit;

    uint16_t env_count;
    uint8_t  env_step;             // 0-31 within the current ramp
    bool     env_rising;           // which way this ramp runs; ALT flips it
    bool     env_holding;
    uint8_t  env_level;            // 0-15, what the channels actually use

    // Chip-clock remainders carried between calls. Per chip, not static:
    // TurboSound has two of these and sharing the remainder would make
    // each one's pitch depend on how busy the other was.
    uint32_t tone_rem, env_rem;
} ay_t;

void ay_reset(ay_t *ay);

/* Register select and data, as the ports deliver them. */
void    ay_select(ay_t *ay, uint8_t reg);
void    ay_write(ay_t *ay, uint8_t value);
uint8_t ay_read(const ay_t *ay);

/*
 * Advance by `ticks` chip clocks and return the mixed output of all three
 * channels, roughly 0..3*32767/3 before the caller scales it.
 *
 * Called once per audio sample with the number of AY cycles that sample
 * covers, so the chip's clock and the Z80's stay locked to each other.
 * Internally it steps the counters at the chip's own rate and averages
 * the result across the sample, which is what stops a 4 kHz tone from
 * aliasing into something else at 44.1 kHz.
 */
int32_t ay_render(ay_t *ay, uint32_t ticks);
