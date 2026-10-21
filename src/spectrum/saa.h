/*
 * frank-dino — an RP2350B acting as the whole machine around a real Z80
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * The SAA1099, as the Karabas-Pro and the Profi's extended periphery
 * have it.
 *
 * Ported from pico-speccy's SAASound.cpp, which is itself stripwax's
 * SAASound by Dave Hooper verified against a real SAA1099P. Their
 * flattening of CSAAFreq/CSAANoise/CSAAEnv/CSAAAmp into one object is
 * kept, because the parts interlock -- a tone edge clocks a noise
 * generator and an envelope -- and separating them again would only
 * move the coupling into a header.
 *
 * Two things are different here, and both are forced by the board.
 *
 * Theirs fills a stereo pair of byte buffers a frame at a time. This
 * machine mixes one sample at a time in run_slice(), so
 * saa_render_stereo() returns one left and one right sample per call
 * and the mixer adds them into its own two sides. Nothing about the
 * generator changes; only where the samples go.
 *
 * And theirs runs one tick per output sample, which is exact only when
 * the output rate is the chip's own clock over 256 -- 31250 Hz at the
 * usual 8 MHz. This board runs at 44100, so the tick is scaled in Q16
 * by saa_set_clock() and a sample is 0.7 of a tick rather than one.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* The ZX/SAM convention. The Creative Music System's pair run at
 * 7.159090 MHz, which is why the clock is a parameter at all. */
#define SAA_CLOCK_HZ 8000000u

typedef struct {
    /* Tone generator, from their CSAAFreq. */
    uint8_t  freq_offset;
    uint8_t  octave;
    uint8_t  next_offset;
    uint8_t  next_octave;
    bool     new_data;
    /* Philips' documented quirk: an offset written without an octave
     * change is deferred by a further half-cycle. */
    bool     ignore_offset;
    uint32_t counter;
    uint32_t period;            /* max(511 - freq_offset, 1) */
    uint8_t  level;             /* the square, 0 or 1 */

    /* Amplifier and mixer, from their CSAAAmp. */
    uint8_t  amp_left;          /* 0-15 */
    uint8_t  amp_right;         /* 0-15 */
    uint8_t  mix_mode;          /* bit 0 tone, bit 1 noise */
} saa_channel_t;

typedef struct {
    uint32_t counter;
    uint8_t  source;            /* 0-3; 3 is clocked by a tone channel */
    uint32_t rand;              /* the 18-bit LFSR */
} saa_noise_t;

typedef struct {
    bool    enabled;
    bool    invert_right;
    bool    clock_externally;
    bool    envelope_ended;
    bool    looping;
    uint8_t num_phases;
    uint8_t phase;
    uint8_t phase_position;
    uint8_t resolution;         /* 1 = 4-bit, step by 1; 2 = 3-bit, by 2 */
    uint8_t shape;              /* 0-7 */
    bool    new_data;
    uint8_t next_data;
    int     left_level;
    int     right_level;
} saa_env_t;

typedef struct {
    uint32_t tick_q16;          /* chip ticks per output sample, Q16 */
    uint32_t rem_scale_q16;     /* 65536 / tick_q16, Q16 */
    uint8_t  reg[32];
    uint8_t  selected;
    bool     output_enabled;
    bool     sync;

    saa_channel_t ch[6];
    saa_noise_t   noise[2];
    saa_env_t     env[2];
} saa_t;

/* Power-on state. Also what the reset line does; the chip has no other. */
void saa_reset(saa_t *s);

/*
 * The master clock and the rate samples are wanted at.
 *
 * Call after saa_reset(): it is what turns the chip's own tick into a
 * per-output-sample increment, and a chip left at the default would run
 * 41% sharp at 44.1 kHz.
 */
void saa_set_clock(saa_t *s, uint32_t clock_hz, uint32_t sample_rate_hz);

/*
 * Register select. Not free of side effects: selecting an envelope
 * register is how a program clocks that envelope externally, so this
 * can advance one.
 */
void saa_select(saa_t *s, uint8_t reg);

void    saa_write(saa_t *s, uint8_t value);
uint8_t saa_read(const saa_t *s);

/* One output sample a side, each at about the AY's scale. */
void saa_render_stereo(saa_t *s, int32_t *left, int32_t *right);

/* One mono output sample: the two sides averaged. */
int32_t saa_render(saa_t *s);

/* Whether anything has switched the chip on, for the boot report. */
static inline bool saa_active(const saa_t *s) { return s->output_enabled; }
