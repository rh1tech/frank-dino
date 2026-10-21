/*
 * frank-dino — an RP2350B acting as the whole machine around a real Z80
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Ported from pico-speccy's SAASound.cpp (GPL-3.0-or-later), which is
 * based on stripwax/SAASound by Dave Hooper, verified against a real
 * SAA1099P. https://github.com/stripwax/SAASound
 */

#include "saa.h"

#include <string.h>

#include "dino.h"

/* ---------------------------------------------------------------------
 * The shape table, from their cs_EnvData.
 *
 * [resolution 0 = 4-bit, 1 = 3-bit][phase][position]
 * ------------------------------------------------------------------ */
typedef struct {
    uint8_t num_phases;
    bool    looping;
    uint8_t levels[2][2][16];
} saa_shape_t;

static const saa_shape_t env_shapes[8] = {
    /* 0: zero */
    {1, false, {{{0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
                 {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}},
                {{0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
                 {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}}}},
    /* 1: maximum */
    {1, true,  {{{15,15,15,15,15,15,15,15,15,15,15,15,15,15,15,15},
                 {15,15,15,15,15,15,15,15,15,15,15,15,15,15,15,15}},
                {{14,14,14,14,14,14,14,14,14,14,14,14,14,14,14,14},
                 {14,14,14,14,14,14,14,14,14,14,14,14,14,14,14,14}}}},
    /* 2: single decay */
    {1, false, {{{15,14,13,12,11,10,9,8,7,6,5,4,3,2,1,0},
                 {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}},
                {{14,14,12,12,10,10,8,8,6,6,4,4,2,2,0,0},
                 {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}}}},
    /* 3: repetitive decay */
    {1, true,  {{{15,14,13,12,11,10,9,8,7,6,5,4,3,2,1,0},
                 {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}},
                {{14,14,12,12,10,10,8,8,6,6,4,4,2,2,0,0},
                 {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}}}},
    /* 4: single triangular */
    {2, false, {{{0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15},
                 {15,14,13,12,11,10,9,8,7,6,5,4,3,2,1,0}},
                {{0,0,2,2,4,4,6,6,8,8,10,10,12,12,14,14},
                 {14,14,12,12,10,10,8,8,6,6,4,4,2,2,0,0}}}},
    /* 5: repetitive triangular */
    {2, true,  {{{0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15},
                 {15,14,13,12,11,10,9,8,7,6,5,4,3,2,1,0}},
                {{0,0,2,2,4,4,6,6,8,8,10,10,12,12,14,14},
                 {14,14,12,12,10,10,8,8,6,6,4,4,2,2,0,0}}}},
    /* 6: single attack */
    {1, false, {{{0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15},
                 {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}},
                {{0,0,2,2,4,4,6,6,8,8,10,10,12,12,14,14},
                 {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}}}},
    /* 7: repetitive attack */
    {1, true,  {{{0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15},
                 {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}},
                {{0,0,2,2,4,4,6,6,8,8,10,10,12,12,14,14},
                 {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}}}},
};

/*
 * Their EffectiveAmplitude table, pre-multiplied by four.
 *
 * It models the analogue interaction of the amplitude and envelope
 * signals on the real chip, which is not a multiply: pdm_x4[amp/2][env].
 */
static const uint16_t pdm_x4[8][16] = {
    {  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0},
    {  0,  4,  4,  8,  8, 12, 12, 16, 16, 20, 20, 24, 24, 28, 28, 32},
    {  0,  4,  8, 12, 16, 20, 24, 28, 32, 36, 40, 44, 48, 52, 56, 60},
    {  0,  8, 12, 20, 24, 32, 36, 44, 48, 56, 60, 68, 72, 80, 84, 92},
    {  0,  8, 16, 24, 32, 40, 48, 56, 64, 72, 80, 88, 96,104,112,120},
    {  0, 12, 20, 32, 40, 52, 60, 72, 80, 92,100,112,120,132,140,152},
    {  0, 12, 24, 36, 48, 60, 72, 84, 96,108,120,132,144,156,168,180},
    {  0, 16, 28, 44, 56, 72, 84,100,112,128,140,156,168,184,196,212},
};

/* The 18-bit Galois LFSR, x^18 + x^11 + x^1. */
static inline void lfsr_step(saa_noise_t *n) {
    if (n->rand & 1u) n->rand = (n->rand >> 1) ^ 0x20400u;
    else              n->rand >>= 1;
}

static inline void set_period(saa_channel_t *c) {
    const int p = 511 - (int)c->freq_offset;
    c->period = (p < 1) ? 1u : (uint32_t)p;
}

/* ------------------------------------------------------------------ */
void saa_reset(saa_t *s) {
    memset(s, 0, sizeof *s);
    s->tick_q16      = 1u << 16;
    s->rem_scale_q16 = 1u << 16;

    for (int i = 0; i < 6; i++) {
        s->ch[i].period = 511;
        s->ch[i].level  = 1;            /* their INITIAL_LEVEL */
    }
    s->noise[0].rand = 0xFFFFFFFFu;     /* their seed */
    s->noise[1].rand = 0xFFFFFFFFu;

    for (int i = 0; i < 2; i++) {
        s->env[i].envelope_ended = true;
        s->env[i].resolution     = 1;
    }
}

void saa_set_clock(saa_t *s, const uint32_t clock_hz, const uint32_t sample_rate_hz) {
    /*
     * Their generator takes one tick per output sample, which is exact
     * only when the output rate is the chip clock over 256. Folding the
     * real output rate in here keeps the whole of the rest of the
     * generator theirs.
     */
    const uint64_t ticks_hz = (uint64_t)clock_hz;
    s->tick_q16 = sample_rate_hz
        ? (uint32_t)((ticks_hz << 16) / (256ull * sample_rate_hz))
        : (1u << 16);
    if (!s->tick_q16) s->tick_q16 = 1;
    s->rem_scale_q16 = (uint32_t)((65536ull << 16) / s->tick_q16);
}

/* ------------------------------------------------------------------ */
/* Envelope                                                            */
/* ------------------------------------------------------------------ */
static void env_set_levels(saa_t *s, const int e) {
    saa_env_t *en = &s->env[e];
    const saa_shape_t *sh = &env_shapes[en->shape];

    if (en->resolution == 1) {
        en->left_level = (en->envelope_ended && !en->looping)
            ? 0 : sh->levels[0][en->phase][en->phase_position];
        en->right_level = en->invert_right ? 15 - en->left_level : en->left_level;
    } else {
        en->left_level = (en->envelope_ended && !en->looping)
            ? 0 : sh->levels[1][en->phase][en->phase_position];
        en->right_level = en->invert_right ? 14 - en->left_level : en->left_level;
    }
}

static void env_set_new_data(saa_t *s, const int e, const uint8_t data) {
    saa_env_t *en = &s->env[e];

    en->phase            = 0;
    en->phase_position   = 0;
    en->shape            = (uint8_t)((data >> 1) & 0x07u);
    en->invert_right     = (data & 0x01u) != 0;
    en->clock_externally = (data & 0x20u) != 0;
    en->num_phases       = env_shapes[en->shape].num_phases;
    en->looping          = env_shapes[en->shape].looping;
    en->resolution       = (data & 0x10u) ? 2u : 1u;
    en->enabled          = (data & 0x80u) != 0;

    if (en->enabled) {
        en->envelope_ended = false;
    } else {
        en->envelope_ended = true;
        en->phase          = 0;
        en->phase_position = 0;
    }
    env_set_levels(s, e);
}

static void env_tick(saa_t *s, const int e) {
    saa_env_t *en = &s->env[e];

    if (!en->enabled) {
        en->envelope_ended = true;
        en->phase          = 0;
        en->phase_position = 0;
        return;
    }
    if (en->envelope_ended) return;

    en->phase_position = (uint8_t)(en->phase_position + en->resolution);

    bool process_new = false;
    if (en->phase_position >= 16) {
        en->phase++;
        if (en->phase == en->num_phases) {
            if (!en->looping) {
                /* Ends at the sustain level, which is zero. */
                en->envelope_ended = true;
                process_new = true;
            } else {
                en->envelope_ended = false;
                en->phase          = 0;
                en->phase_position = (uint8_t)(en->phase_position - 16);
                process_new = true;
            }
        } else {
            /* Between the phases of a triangular shape. */
            en->envelope_ended = false;
            en->phase_position = (uint8_t)(en->phase_position - 16);
        }
    } else {
        en->envelope_ended = false;
    }

    /* Buffered parameters land only on a natural boundary. */
    if (en->new_data && process_new) {
        en->new_data = false;
        env_set_new_data(s, e, en->next_data);
    } else {
        env_set_levels(s, e);
    }
}

static void env_set_control(saa_t *s, const int e, const uint8_t data) {
    saa_env_t *en = &s->env[e];
    const bool enable = (data & 0x80u) != 0;

    if (!enable && !en->enabled) return;

    en->enabled = enable;
    if (!en->enabled) { en->envelope_ended = true; return; }

    /* The resolution is immediate, and moves the position with it. */
    const uint8_t res = (data & 0x10u) ? 2u : 1u;
    if (en->resolution == 1 && res == 2)      en->phase_position &= 0x0Eu;
    else if (en->resolution == 2 && res == 1) en->phase_position |= 0x01u;
    en->resolution = res;

    if (en->envelope_ended) {
        env_set_new_data(s, e, data);
        en->new_data = false;
    } else {
        env_set_levels(s, e);       /* the resolution may have moved it */
        en->new_data  = true;
        en->next_data = data;
    }
}

/* ------------------------------------------------------------------ */
/* Registers                                                           */
/* ------------------------------------------------------------------ */
void saa_select(saa_t *s, const uint8_t reg) {
    s->selected = (uint8_t)(reg & 0x1Fu);
    /*
     * Selecting an envelope register is the external envelope clock.
     * That is the chip's own behaviour and programs use it as one.
     */
    if (s->selected == 24) {
        if (s->env[0].clock_externally && s->env[0].enabled) env_tick(s, 0);
    } else if (s->selected == 25) {
        if (s->env[1].clock_externally && s->env[1].enabled) env_tick(s, 1);
    }
}

uint8_t saa_read(const saa_t *s) { return s->reg[s->selected & 0x1Fu]; }

/* The tone generator's buffered octave and offset, applied on a
 * half-cycle. Their UpdateOctaveOffsetData. */
static void tone_update_data(saa_channel_t *c) {
    if (!c->new_data) return;

    c->octave = c->next_octave;                 /* the octave always lands */
    if (!c->ignore_offset) {
        c->freq_offset = c->next_offset;
        c->new_data    = false;
    }
    c->ignore_offset = false;
    set_period(c);
}

void saa_write(saa_t *s, const uint8_t data) {
    const uint8_t r = s->selected;
    if (r > 0x1Fu) return;
    s->reg[r] = data;

    switch (r) {
    case 0: case 1: case 2: case 3: case 4: case 5:
        s->ch[r].amp_left  = (uint8_t)(data & 0x0Fu);
        s->ch[r].amp_right = (uint8_t)((data >> 4) & 0x0Fu);
        break;

    case 8: case 9: case 10: case 11: case 12: case 13: {
        saa_channel_t *c = &s->ch[r - 8];
        if (!s->sync) {
            c->next_offset = data;
            c->new_data    = true;
            /* Philips' quirk: an offset with no octave change waits a
             * further half-cycle. */
            if (c->next_octave == c->octave) c->ignore_offset = true;
        } else {
            c->new_data      = false;
            c->ignore_offset = false;
            c->freq_offset   = data;
            c->next_offset   = data;
            c->octave        = c->next_octave;
            set_period(c);
        }
        break;
    }

    case 16: case 17: case 18: {
        const int base = (r - 16) * 2;
        const uint8_t oct[2] = { (uint8_t)(data & 0x07u),
                                 (uint8_t)((data >> 4) & 0x07u) };
        for (int i = 0; i < 2; i++) {
            saa_channel_t *c = &s->ch[base + i];
            if (!s->sync) {
                c->next_octave   = oct[i];
                c->new_data      = true;
                c->ignore_offset = false;
            } else {
                c->new_data      = false;
                c->ignore_offset = false;
                c->octave        = oct[i];
                c->next_octave   = oct[i];
                c->freq_offset   = c->next_offset;
                set_period(c);
            }
        }
        break;
    }

    case 20:            /* tone mixer */
        for (int i = 0; i < 6; i++) {
            if (data & (1u << i)) s->ch[i].mix_mode |= 1u;
            else                  s->ch[i].mix_mode &= (uint8_t)~1u;
        }
        break;

    case 21:            /* noise mixer */
        for (int i = 0; i < 6; i++) {
            if (data & (1u << i)) s->ch[i].mix_mode |= 2u;
            else                  s->ch[i].mix_mode &= (uint8_t)~2u;
        }
        break;

    case 22:            /* noise source */
        s->noise[0].source = (uint8_t)(data & 0x03u);
        s->noise[1].source = (uint8_t)((data >> 4) & 0x03u);
        break;

    case 24: env_set_control(s, 0, data); break;
    case 25: env_set_control(s, 1, data); break;

    case 28: {          /* global enable and sync */
        const bool new_sync = (data & 0x02u) != 0;
        if (new_sync != s->sync) {
            if (new_sync) {
                for (int i = 0; i < 6; i++) {
                    saa_channel_t *c = &s->ch[i];
                    c->counter       = 0;
                    c->level         = 1;
                    c->octave        = c->next_octave;
                    c->freq_offset   = c->next_offset;
                    set_period(c);
                    c->new_data      = false;
                    c->ignore_offset = false;
                }
                s->noise[0].counter = 0;
                s->noise[1].counter = 0;
                /* The envelopes are deliberately not synced, as theirs. */
            }
            s->sync = new_sync;
        }
        s->output_enabled = (data & 0x01u) != 0;
        break;
    }

    default:
        break;
    }
}

/* ------------------------------------------------------------------ */
/* One sample                                                          */
/* ------------------------------------------------------------------ */
void __not_in_flash_func(saa_render_stereo)(saa_t *s, int32_t *const left,
                                            int32_t *const right) {
    /*
     * Muted is the power-on state and the state a Profi in CP/M stays
     * in, so it is worth a fast path: the loop below would otherwise
     * tick two LFSRs and six counters to accumulate nothing.
     *
     * The generators freeze while muted rather than free-running. The
     * real chip keeps counting; the difference is the phase of silent
     * oscillators, which is the same trade the sync path makes.
     */
    if (!s->output_enabled || s->sync) { *left = *right = 0; return; }

    /* The noise generators that count for themselves. Source 3 is
     * clocked by a tone channel instead, below. */
    for (int ng = 0; ng < 2; ng++) {
        saa_noise_t *n = &s->noise[ng];
        if (n->source >= 3) continue;
        const uint32_t period = (n->source == 0) ? 1u : (n->source == 1) ? 2u : 4u;
        n->counter += s->tick_q16;
        while (n->counter >= (period << 16)) {
            n->counter -= (period << 16);
            lfsr_step(n);
        }
    }

    int32_t out_l = 0, out_r = 0;

    for (int i = 0; i < 6; i++) {
        saa_channel_t *c = &s->ch[i];
        const int ng = i / 3;

        /*
         * The square's TIME AVERAGE over this sample, not its level at
         * the sample point.
         *
         * Taking the level alone snaps every edge to the output grid,
         * and their measurements put the resulting broadband noise only
         * about 19 dB below a 500 Hz tone -- eight to ten dB dirtier
         * than the AY at every pitch. Averaging costs work per EDGE
         * rather than per sub-tick, and there is at most one edge in a
         * sample by construction, so in the common case this is the
         * plain level and the arithmetic is unchanged.
         */
        const uint8_t  oct = c->octave;
        const uint32_t inc = s->tick_q16 << oct;
        uint32_t lvl_acc = 0;       /* level * duration, Q16 of a sample */
        uint32_t t_prev  = 0;

        c->counter += inc;
        while (c->counter >= (c->period << 16)) {
            c->counter -= (c->period << 16);

            /* What is left in the counter is how much of the sample
             * falls AFTER this edge. rem_scale_q16 turns the divide by
             * the tick into a multiply. */
            const uint32_t rem = (uint32_t)
                (((uint64_t)(c->counter >> oct) * s->rem_scale_q16) >> 16);
            uint32_t t_flip = (rem >= 65536u) ? 0u : 65536u - rem;
            if (t_flip < t_prev) t_flip = t_prev;   /* keep it monotonic */
            lvl_acc += (uint32_t)c->level * (t_flip - t_prev);
            t_prev = t_flip;

            c->level ^= 1u;

            /* What a tone edge drives: channel 0 the first noise
             * generator's source 3, channel 3 the second's, and
             * channels 1 and 4 the two internal envelope clocks. */
            if (i == 0 && s->noise[0].source == 3) lfsr_step(&s->noise[0]);
            if (i == 3 && s->noise[1].source == 3) lfsr_step(&s->noise[1]);
            if (i == 1 && s->env[0].enabled && !s->env[0].clock_externally)
                env_tick(s, 0);
            if (i == 4 && s->env[1].enabled && !s->env[1].clock_externally)
                env_tick(s, 1);

            tone_update_data(c);
        }
        lvl_acc += (uint32_t)c->level * (65536u - t_prev);

        /*
         * The mixer. Everything is Q16, so 65536 is their 1 and the
         * intermediate runs 0..131072 where theirs ran 0..2.
         *
         * A noise source ticks at most once per output sample and was
         * advanced above, so it is constant across the sample and only
         * the tone needed averaging.
         */
        const uint32_t tone_level  = lvl_acc;
        const int      noise_level = (int)(s->noise[ng].rand & 1u);
        uint32_t intermediate;

        switch (c->mix_mode) {
        case 1:  intermediate = tone_level * 2u; break;
        case 2:  intermediate = (uint32_t)noise_level * (2u << 16); break;
        case 3:  intermediate = tone_level * (uint32_t)(2 - noise_level); break;
        default: intermediate = 0; break;
        }

        if ((i == 2 && s->env[0].enabled) || (i == 5 && s->env[1].enabled)) {
            /* An envelope channel goes through the PDM table rather
             * than a multiply. */
            const saa_env_t *en = &s->env[i / 3];
            const uint32_t inv = (2u << 16) - intermediate;
            out_l += (int32_t)((pdm_x4[c->amp_left  >> 1][en->left_level]  * inv) >> 16);
            out_r += (int32_t)((pdm_x4[c->amp_right >> 1][en->right_level] * inv) >> 16);
        } else {
            out_l += (int32_t)((c->amp_left  * 16u * intermediate) >> 16);
            out_r += (int32_t)((c->amp_right * 16u * intermediate) >> 16);
        }
    }

    /*
     * Scaled to sit where the AY sits.
     *
     * Six channels reach about 2880 a side. ay_render_stereo() gives a
     * chip at full scale about 12287 a side, and four times this puts
     * it at 11520 -- near enough that the SAA and the AY do not need
     * different volume settings to be heard together. It is a mix
     * decision, not a measurement.
     */
    *left  = out_l * 4;
    *right = out_r * 4;
}

int32_t saa_render(saa_t *s) {
    int32_t l, r;
    saa_render_stereo(s, &l, &r);
    return (l + r) / 2;
}
