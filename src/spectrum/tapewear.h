/*
 * frank-dino — an RP2350B acting as the whole machine around a real Z80
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-dino
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Ported from pico-speccy (https://github.com/drewpo28/pico-speccy, GPL-3.0),
 * src/speccy/devices/tape/TapeWear.h -- the C++ namespace made C.
 */

/*
 * A worn tape: the recorder chewing it.
 *
 * Three things, on a seeded generator so a run can be repeated:
 *   wow    the speed wanders, a bounded random walk stepped every ~1 ms
 *   lurch  a stretch of pulses comes out 1.5-3x long, the tape snagging
 *   drop   the head lifts off and the line freezes where it was
 *
 * Lengths are T-states; a pulse longer than 50 ms is a pause, not signal,
 * and is left alone. Faults land only while there is signal: a gap
 * between blocks ages nothing.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define TW_WOW_STEP_T   3500u
#define TW_MAX_PULSE_T  175000u
#define TW_CLEAN_MIN_T  1400000000u
#define TW_CLEAN_SPAN_T 2800000000u

enum { TW_NONE = 0, TW_DROP, TW_LURCH };

typedef struct {
    int32_t  wow_max, wow_step;
    uint32_t evt_min, evt_span, dur_min, dur_span;
    uint8_t  clean_pct;
} tw_level_t;

static const tw_level_t kTwLevels[3] = {
    { 10, 1, 280000000u, 2240000000u,  1750u,  7000u,  0 },   // light:  +-4 %, ~400 s apart, 0.5-2.5 ms
    { 20, 2,  42000000u,  336000000u,  7000u, 28000u, 40 },   // medium: +-8 %, ~60 s,  2-10 ms
    { 40, 3,   4200000u,   33600000u, 17500u, 87500u, 10 },   // heavy: +-16 %, ~6 s,   5-30 ms
};

typedef struct {
    uint8_t  level;         // 0 off, 1-3
    uint32_t rng;
    int32_t  wow, wow_next;
    int64_t  evt_next;
    uint32_t evt_left;
    uint8_t  evt_kind;
    uint16_t evt_mul;       // a lurch's stretch, 1/256ths
} tapewear_t;

static inline uint32_t tw_rand(tapewear_t *s) {     // xorshift32
    s->rng ^= s->rng << 13;
    s->rng ^= s->rng >> 17;
    s->rng ^= s->rng << 5;
    return s->rng;
}

static inline int64_t tw_interval(tapewear_t *s) {
    const tw_level_t *L = &kTwLevels[s->level - 1];
    if (L->clean_pct && (tw_rand(s) % 100u) < L->clean_pct)
        return (int64_t)TW_CLEAN_MIN_T + (int64_t)(tw_rand(s) % TW_CLEAN_SPAN_T);
    return (int64_t)L->evt_min + (int64_t)(tw_rand(s) % L->evt_span);
}

static inline void tw_reset(tapewear_t *s, const uint8_t level, const uint32_t seed) {
    s->level = level > 3 ? 3 : level;
    s->rng = seed | 1u;
    s->wow = 0;
    s->wow_next = (int32_t)TW_WOW_STEP_T;
    s->evt_left = 0;
    s->evt_kind = TW_NONE;
    s->evt_mul = 256;
    s->evt_next = s->level ? tw_interval(s) : 0;
}

static inline void tw_advance(tapewear_t *s, const uint32_t t, const bool signal) {
    if (!s->level) return;
    if (s->evt_kind) {
        s->evt_left = s->evt_left > t ? s->evt_left - t : 0;
        if (!s->evt_left) s->evt_kind = TW_NONE;
        return;
    }
    if (!signal) return;
    s->evt_next -= (int64_t)t;
    if (s->evt_next > 0) return;
    const tw_level_t *L = &kTwLevels[s->level - 1];
    s->evt_kind = (tw_rand(s) & 1u) ? TW_DROP : TW_LURCH;
    s->evt_left = L->dur_min + tw_rand(s) % L->dur_span;
    s->evt_mul  = (uint16_t)(384u + tw_rand(s) % 385u);
    s->evt_next = tw_interval(s);
}

/* One pulse of `next` T-states through the worn tape: its new length, and
 * whether the line is frozen (a drop) for it. */
static inline uint32_t tw_pulse(tapewear_t *s, const uint32_t next, bool *freeze) {
    *freeze = false;
    if (!s->level || next == 0 || next > TW_MAX_PULSE_T) return next;
    const tw_level_t *L = &kTwLevels[s->level - 1];
    s->wow_next -= (int32_t)next;
    while (s->wow_next <= 0) {
        s->wow += (int32_t)(tw_rand(s) % (uint32_t)(2 * L->wow_step + 1)) - L->wow_step;
        if (s->wow >  L->wow_max) s->wow =  L->wow_max;
        if (s->wow < -L->wow_max) s->wow = -L->wow_max;
        s->wow_next += (int32_t)TW_WOW_STEP_T;
    }
    int32_t scaled = (int32_t)next + (int32_t)next * s->wow / 256;
    if (scaled < 1) scaled = 1;
    uint32_t out = (uint32_t)scaled;
    if (out > TW_MAX_PULSE_T) out = TW_MAX_PULSE_T;
    tw_advance(s, out, true);
    if (s->evt_kind == TW_LURCH) {
        out = (uint32_t)(((uint64_t)out * s->evt_mul) >> 8);
        if (out > TW_MAX_PULSE_T) out = TW_MAX_PULSE_T;
    } else if (s->evt_kind == TW_DROP) {
        *freeze = true;
    }
    return out ? out : 1;
}
