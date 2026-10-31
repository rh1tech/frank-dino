/*
 * frank-dino — an RP2350B acting as the whole machine around a real Z80
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-dino
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Block semantics after the TZX 1.20, PZX 1.0 and CSW 2 specifications.
 * Standard and Rodolfo Guerra timings from pico-speccy
 * (https://github.com/drewpo28/pico-speccy, GPL-3.0) -- drew.po28, after
 * ESPectrum by Víctor Iborra [Eremus] and David Crespo [dcrespo3d].
 */

#include "pulse.h"

#include <stdio.h>
#include <string.h>

#ifndef PULSE_HOST_TEST
#define PSRAM_BSS __attribute__((section(".psram")))
#else
#define PSRAM_BSS
#endif

// ---------------------------------------------------------------------------
// Timings, in T-states at 3.5 MHz
// ---------------------------------------------------------------------------
typedef struct {
    uint16_t pilot, sync1, sync2, bit0, bit1;
    uint16_t pilot_hdr, pilot_data;     // pilot pulses before a header / data
    uint32_t pause;                     // between blocks of a .tap
} std_timing_t;

static const std_timing_t TIMING_STD = { 2168, 667, 735, 855, 1710, 8063, 3223, 3500000u };
static const std_timing_t TIMING_RG  = { 1408, 397, 317, 325,  649, 4835, 1930, 1113000u };

#define T_PER_MS 3500u

// ---------------------------------------------------------------------------
// The tape
// ---------------------------------------------------------------------------
static const uint8_t *img;
static uint32_t       img_len;
static pulse_fmt_t    fmt;
static pulse_state_t  state;
static bool           level;
static bool           is_48k;
static const std_timing_t *tm = &TIMING_STD;

/*
 * Where every block starts, found once at open.
 *
 * A TZX jump is relative in blocks and a loop goes back to a block, so
 * the generator has to be able to name block n -- and the browser lists
 * them. Two thousand is more than any tape has; the PSRAM has the room.
 */
#define MAX_BLOCKS 2048u
static uint32_t blk_off[MAX_BLOCKS] PSRAM_BSS;
static uint32_t blk_count;
static uint32_t blk_no;             // the block being played
static bool     blk_started;        // its stages have been set up

static uint32_t loop_start, loop_left;      // TZX 0x24/0x25
static uint32_t call_blk, call_n, call_i;   // TZX 0x26/0x27
static bool     in_call;

// ---------------------------------------------------------------------------
// Little-endian reads, bounds-checked against the image
// ---------------------------------------------------------------------------
static inline uint32_t rd8(const uint32_t p)  { return p < img_len ? img[p] : 0u; }
static inline uint32_t rd16(const uint32_t p) { return rd8(p) | (rd8(p + 1) << 8); }
static inline uint32_t rd24(const uint32_t p) { return rd16(p) | (rd8(p + 2) << 16); }
static inline uint32_t rd32(const uint32_t p) { return rd16(p) | (rd16(p + 2) << 16); }

// ---------------------------------------------------------------------------
// A block is a short program of stages
// ---------------------------------------------------------------------------
typedef enum : uint8_t {
    S_TONE,         // a pulses of b T-states
    S_SEQ,          // a pulses, lengths as u16 at p
    S_DATA,         // a bits at p, MSB first: two pulses of b (0) or c (1)
    S_PAUSE,        // a ms of silence, then the level is low
    S_DIRECT,       // a samples at p, one bit each, b T-states a sample
    S_CSW,          // RLE edges at p for a bytes, at b Hz
    S_GDB,          // TZX 0x19, from p
    S_PZX_PULS,     // the pulse list at p, a bytes
    S_PZX_DATA,     // the data block at p
    S_LEVEL,        // the level becomes a
    S_STOP,         // the tape stops here
} stage_kind_t;

typedef struct {
    stage_kind_t kind;
    uint32_t     a, b, c;
    uint32_t     p;             // an offset into the image
} stage_t;

#define MAX_STAGES 6
static stage_t  st[MAX_STAGES];
static uint32_t st_n, st_i;

/* Progress through the current stage. */
static uint32_t ctr;            // pulses, bits or bytes done
static uint32_t sub;            // within a bit or a symbol
static uint32_t cur;            // the stage's own scratch

static void stage(const stage_kind_t k, const uint32_t a, const uint32_t b,
                  const uint32_t c, const uint32_t p) {
    if (st_n < MAX_STAGES) st[st_n++] = (stage_t){ k, a, b, c, p };
}

/*
 * A standard-shaped data block: pilot, two sync pulses, the data, and a
 * pause. 0x10, 0x11, 0x14 and every block of a .tap are this.
 */
static void plan_data(const uint32_t pilot, const uint32_t pilot_n, const uint32_t s1,
                      const uint32_t s2, const uint32_t b0, const uint32_t b1,
                      const uint32_t last_bits, const uint32_t pause_ms,
                      const uint32_t data, const uint32_t len) {
    if (pilot_n) stage(S_TONE, pilot_n, pilot, 0, 0);
    if (s1)      stage(S_TONE, 1, s1, 0, 0);
    if (s2)      stage(S_TONE, 1, s2, 0, 0);
    if (len) {
        const uint32_t bits = (len - 1u) * 8u + (last_bits ? last_bits : 8u);
        stage(S_DATA, bits, b0, b1, data);
    }
    if (pause_ms) stage(S_PAUSE, pause_ms, 0, 0, 0);
}

// ---------------------------------------------------------------------------
// TZX
// ---------------------------------------------------------------------------
/* How long the record at p (its ID byte) is, ID included. */
static uint32_t tzx_record_len(const uint32_t p) {
    const uint32_t b = p + 1;
    switch (rd8(p)) {
        case 0x10: return 1 + 4 + rd16(b + 2);
        case 0x11: return 1 + 0x12 + rd24(b + 0x0F);
        case 0x12: return 1 + 4;
        case 0x13: return 1 + 1 + 2u * rd8(b);
        case 0x14: return 1 + 0x0A + rd24(b + 7);
        case 0x15: return 1 + 8 + rd24(b + 5);
        case 0x18: return 1 + 4 + rd32(b);
        case 0x19: return 1 + 4 + rd32(b);
        case 0x20: return 1 + 2;
        case 0x21: return 1 + 1 + rd8(b);
        case 0x22: return 1;
        case 0x23: return 1 + 2;
        case 0x24: return 1 + 2;
        case 0x25: return 1;
        case 0x26: return 1 + 2 + 2u * rd16(b);
        case 0x27: return 1;
        case 0x28: return 1 + 2 + rd16(b);
        case 0x2A: return 1 + 4 + rd32(b);
        case 0x2B: return 1 + 4 + rd32(b);
        case 0x30: return 1 + 1 + rd8(b);
        case 0x31: return 1 + 2 + rd8(b + 1);
        case 0x32: return 1 + 2 + rd16(b);
        case 0x33: return 1 + 1 + 3u * rd8(b);
        case 0x35: return 1 + 0x14 + rd32(b + 0x10);
        case 0x5A: return 1 + 9;
        default:   return 1 + 4 + rd32(b);      // the spec's rule for the unknown
    }
}

static void index_tzx(void) {
    uint32_t p = 10;
    while (p < img_len && blk_count < MAX_BLOCKS) {
        blk_off[blk_count++] = p;
        const uint32_t n = tzx_record_len(p);
        if (n == 0 || p + n > img_len) break;
        p += n;
    }
}

static void index_tap(void) {
    uint32_t p = 0;
    while (p + 2 <= img_len && blk_count < MAX_BLOCKS) {
        const uint32_t n = rd16(p);
        if (n == 0 || p + 2 + n > img_len) break;
        blk_off[blk_count++] = p;
        p += 2 + n;
    }
}

#define PZX_TAG(a, b, c, d) ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))

static void index_pzx(void) {
    uint32_t p = 0;
    while (p + 8 <= img_len && blk_count < MAX_BLOCKS) {
        blk_off[blk_count++] = p;
        p += 8 + rd32(p + 4);
    }
}

/*
 * Set the current block up as stages. Returns false for a block that
 * produces nothing and has already done whatever it does (a jump, a loop
 * marker, a comment), so the caller moves straight on.
 */
static bool plan_tzx(const uint32_t p) {
    const uint32_t b = p + 1;
    switch (rd8(p)) {
        case 0x10: {
            const uint32_t len  = rd16(b + 2);
            const uint32_t flag = rd8(b + 4);
            plan_data(tm->pilot, flag < 0x80 ? tm->pilot_hdr : tm->pilot_data,
                      tm->sync1, tm->sync2, tm->bit0, tm->bit1, 8, rd16(b), b + 4, len);
            return true;
        }
        case 0x11:
            plan_data(rd16(b), rd16(b + 0x0A), rd16(b + 2), rd16(b + 4), rd16(b + 6),
                      rd16(b + 8), rd8(b + 0x0C), rd16(b + 0x0D), b + 0x12, rd24(b + 0x0F));
            return true;
        case 0x12:
            stage(S_TONE, rd16(b + 2), rd16(b), 0, 0);
            return true;
        case 0x13:
            stage(S_SEQ, rd8(b), 0, 0, b + 1);
            return true;
        case 0x14:
            plan_data(0, 0, 0, 0, rd16(b), rd16(b + 2), rd8(b + 4), rd16(b + 5), b + 0x0A, rd24(b + 7));
            return true;
        case 0x15: {
            const uint32_t len  = rd24(b + 5);
            const uint32_t last = rd8(b + 4);
            if (len) stage(S_DIRECT, (len - 1u) * 8u + (last ? last : 8u), rd16(b), 0, b + 8);
            if (rd16(b + 2)) stage(S_PAUSE, rd16(b + 2), 0, 0, 0);
            return true;
        }
        case 0x18: {
            // RLE only. Z-RLE inside a TZX is rare enough to be a pause.
            const uint32_t len = rd32(b);
            if (rd8(b + 9) == 1 && len > 10) stage(S_CSW, len - 10u, rd24(b + 6), 0, b + 14);
            if (rd16(b + 4)) stage(S_PAUSE, rd16(b + 4), 0, 0, 0);
            return true;
        }
        case 0x19:
            stage(S_GDB, 0, 0, 0, b);
            if (rd16(b + 4)) stage(S_PAUSE, rd16(b + 4), 0, 0, 0);
            return true;
        case 0x20:
            if (rd16(b)) stage(S_PAUSE, rd16(b), 0, 0, 0);
            else         stage(S_STOP, 0, 0, 0, 0);
            return true;
        case 0x23: {
            const int16_t rel = (int16_t)rd16(b);
            const int32_t to  = (int32_t)blk_no + rel;
            blk_no = (to < 0) ? 0 : (uint32_t)to;
            blk_no--;                   // the caller's increment lands on it
            return false;
        }
        case 0x24:
            loop_start = blk_no + 1;
            loop_left  = rd16(b);
            return false;
        case 0x25:
            if (loop_left > 1) { loop_left--; blk_no = loop_start - 1; }
            else loop_left = 0;
            return false;
        case 0x26:
            call_blk = p;
            call_n   = rd16(b);
            call_i   = 0;
            if (call_n) {
                in_call = true;
                const int32_t to = (int32_t)blk_no + (int16_t)rd16(b + 2);
                blk_no = (uint32_t)(to < 0 ? 0 : to) - 1;
            }
            return false;
        case 0x27:
            if (in_call) {
                // Back to the block after the call, or on to the next target.
                uint32_t caller = 0;
                for (uint32_t i = 0; i < blk_count; i++) if (blk_off[i] == call_blk) caller = i;
                if (++call_i < call_n) {
                    const int32_t to = (int32_t)caller + (int16_t)rd16(call_blk + 1 + 2 + 2u * call_i);
                    blk_no = (uint32_t)(to < 0 ? 0 : to) - 1;
                } else {
                    in_call = false;
                    blk_no  = caller;
                }
            }
            return false;
        case 0x2A:
            if (is_48k) stage(S_STOP, 0, 0, 0, 0);
            return st_n != 0;
        case 0x2B:
            stage(S_LEVEL, rd8(b + 4) ? 1u : 0u, 0, 0, 0);
            return true;
        default:
            return false;                // text, archive info, groups, glue...
    }
}

// ---------------------------------------------------------------------------
// PZX
// ---------------------------------------------------------------------------
static bool plan_pzx(const uint32_t p) {
    const uint32_t tag = rd32(p), len = rd32(p + 4), body = p + 8;
    if (tag == PZX_TAG('P', 'U', 'L', 'S')) { stage(S_PZX_PULS, len, 0, 0, body); return true; }
    if (tag == PZX_TAG('D', 'A', 'T', 'A')) { stage(S_PZX_DATA, len, 0, 0, body); return true; }
    if (tag == PZX_TAG('P', 'A', 'U', 'S')) {
        const uint32_t v = rd32(body);
        stage(S_LEVEL, (v >> 31) & 1u, 0, 0, 0);
        stage(S_TONE, 1, v & PULSE_LEN_MASK, 1, 0);     // c=1: hold, no edge
        return true;
    }
    if (tag == PZX_TAG('S', 'T', 'O', 'P')) {
        if (!(rd16(body) & 1u) || is_48k) { stage(S_STOP, 0, 0, 0, 0); return true; }
    }
    return false;
}

// ---------------------------------------------------------------------------
// Opening and moving about
// ---------------------------------------------------------------------------
static void reset_play(void) {
    st_n = st_i = 0;
    ctr = sub = cur = 0;
    blk_started = false;
    loop_left = 0;
    in_call = false;
    level = false;
    state = blk_count ? PULSE_RUNNING : PULSE_END;
}

bool pulse_open(const uint8_t *image, const uint32_t len, const pulse_fmt_t f) {
    img = image;
    img_len = len;
    fmt = f;
    blk_count = 0;
    blk_no = 0;

    switch (f) {
        case PULSE_TAP: index_tap(); break;
        case PULSE_TZX:
            if (len < 10 || memcmp(image, "ZXTape!\x1A", 8) != 0) return false;
            index_tzx();
            break;
        case PULSE_PZX:
            if (len < 8 || rd32(0) != PZX_TAG('P', 'Z', 'X', 'T')) return false;
            index_pzx();
            break;
        case PULSE_CSW:
            if (len < 0x20 || memcmp(image, "Compressed Square Wave\x1A", 23) != 0) return false;
            blk_off[0] = 0;
            blk_count = 1;
            break;
    }
    reset_play();
    return blk_count != 0;
}

void pulse_rewind(void) {
    blk_no = 0;
    reset_play();
}

bool pulse_seek_block(const uint32_t n) {
    if (n >= blk_count) return false;
    reset_play();
    blk_no = n;
    return true;
}

pulse_state_t pulse_state(void) { return state; }

void pulse_resume(void) {
    if (state == PULSE_STOPPED) state = PULSE_RUNNING;
}

void pulse_set_is_48k(const bool v) { is_48k = v; }
void pulse_set_rg_timings(const bool on) { tm = on ? &TIMING_RG : &TIMING_STD; }

uint32_t pulse_block_index(void) { return blk_no; }
uint32_t pulse_block_count(void) { return blk_count; }

/*
 * The CSW file is one block: a header, then RLE. v1 has a 0x20-byte
 * header and a 16-bit rate; v2 says how long its header is.
 */
static void plan_csw_file(void) {
    const uint32_t major = rd8(0x17);
    uint32_t rate, data, comp, flags;
    if (major == 1) {
        rate = rd16(0x19); comp = rd8(0x1B); flags = rd8(0x1C); data = 0x20;
    } else {
        rate = rd32(0x19); comp = rd8(0x21); flags = rd8(0x22);
        data = 0x34 + rd8(0x23);
    }
    level = (flags & 1u) != 0;
    // Z-RLE is inflated by the loader before it gets here, so by now
    // every CSW is RLE.
    (void)comp;
    if (data < img_len) stage(S_CSW, img_len - data, rate ? rate : 44100u, 0, data);
}

// ---------------------------------------------------------------------------
// Making pulses
// ---------------------------------------------------------------------------
static uint32_t *out_p;
static uint32_t  out_left;

/* An edge, then a pulse at the new level. */
__attribute__((always_inline)) static inline void edge(const uint32_t t) {
    level = !level;
    *out_p++ = (level ? PULSE_LEVEL : 0u) | (t & PULSE_LEN_MASK);
    out_left--;
}

/* The level held, no edge. */
__attribute__((always_inline)) static inline void hold(const uint32_t t) {
    *out_p++ = (level ? PULSE_LEVEL : 0u) | (t & PULSE_LEN_MASK);
    out_left--;
}

/*
 * GDB, TZX 0x19: symbols defined as pulse lists, a pilot as runs of
 * symbols and the data as a bit-packed symbol stream. The general case
 * of everything else in the format, and what most modern conversions of
 * custom loaders use.
 *
 * Run as a state machine across calls, because a block is easily longer
 * than one fill. `cur` is where we are in the stream, `ctr` how many
 * symbols are done, `sub` the pulse within the symbol.
 */
static struct {
    uint32_t totp, npp, asp, totd, npd, asd;
    uint32_t pdef, prle, ddef, dstream;
    uint32_t nb;                // bits per data symbol
    uint32_t sym, reps;         // the pilot run in progress
    bool     data_phase;
} g;

static void gdb_setup(const uint32_t b) {
    g.totp = rd32(b + 6);
    g.npp  = rd8(b + 10);
    g.asp  = rd8(b + 11) ? rd8(b + 11) : 256u;
    g.totd = rd32(b + 12);
    g.npd  = rd8(b + 16);
    g.asd  = rd8(b + 17) ? rd8(b + 17) : 256u;
    g.pdef = b + 18;
    g.prle = g.pdef + (g.totp ? g.asp * (1u + 2u * g.npp) : 0u);
    g.ddef = g.prle + 3u * g.totp;
    g.dstream = g.ddef + (g.totd ? g.asd * (1u + 2u * g.npd) : 0u);
    g.nb = 0;
    while ((1u << g.nb) < g.asd) g.nb++;
    if (!g.nb) g.nb = 1;
    g.data_phase = g.totp == 0;
    g.reps = 0;
    ctr = sub = 0;
}

/* Start a symbol: its flags decide the first edge. */
static void gdb_symbol_start(const uint32_t def) {
    switch (rd8(def) & 3u) {
        case 0: level = !level; break;      // an edge, as usual
        case 1: break;                      // carry on at this level
        case 2: level = false; break;
        case 3: level = true;  break;
    }
}

/* Play pulses of the symbol at def from pulse `sub`; true when it is done. */
static bool gdb_symbol_play(const uint32_t def, const uint32_t maxp) {
    while (sub < maxp && out_left) {
        const uint32_t t = rd16(def + 1 + 2u * sub);
        if (t == 0) { sub = maxp; break; }
        if (sub) level = !level;
        hold(t);
        sub++;
    }
    return sub >= maxp;
}

static bool run_gdb(void) {
    while (out_left) {
        if (!g.data_phase) {
            if (g.reps == 0) {
                if (ctr >= g.totp) { g.data_phase = true; ctr = 0; continue; }
                g.sym  = rd8(g.prle + 3u * ctr);
                g.reps = rd16(g.prle + 3u * ctr + 1);
                ctr++;
                sub = 0;
                if (!g.reps) continue;
                gdb_symbol_start(g.pdef + g.sym * (1u + 2u * g.npp));
            }
            const uint32_t def = g.pdef + g.sym * (1u + 2u * g.npp);
            if (!gdb_symbol_play(def, g.npp)) return false;
            sub = 0;
            if (--g.reps) gdb_symbol_start(def);
        } else {
            if (ctr >= g.totd) return true;
            if (sub == 0) {
                const uint32_t bit = ctr * g.nb;
                uint32_t sym = 0;
                for (uint32_t i = 0; i < g.nb; i++) {
                    const uint32_t at = bit + i;
                    sym = (sym << 1) | ((rd8(g.dstream + at / 8u) >> (7u - at % 8u)) & 1u);
                }
                g.sym = sym;
                gdb_symbol_start(g.ddef + sym * (1u + 2u * g.npd));
            }
            const uint32_t def = g.ddef + g.sym * (1u + 2u * g.npd);
            if (!gdb_symbol_play(def, g.npd)) return false;
            sub = 0;
            ctr++;
        }
    }
    return false;
}

/* One stage, as far as the output allows. True when it is finished. */
static bool run_stage(stage_t *s) {
    switch (s->kind) {
        case S_TONE:
            while (ctr < s->a && out_left) {
                if (s->c) hold(s->b); else edge(s->b);
                ctr++;
            }
            return ctr >= s->a;

        case S_SEQ:
            while (ctr < s->a && out_left) { edge(rd16(s->p + 2u * ctr)); ctr++; }
            return ctr >= s->a;

        case S_DATA:
            while (ctr < s->a && out_left) {
                const uint32_t bit = (rd8(s->p + ctr / 8u) >> (7u - ctr % 8u)) & 1u;
                edge(bit ? s->c : s->b);
                if (++sub == 2) { sub = 0; ctr++; }
            }
            return ctr >= s->a;

        case S_PAUSE: {
            if (!out_left) return false;
            uint32_t t = s->a * T_PER_MS;
            // A pause ends low. If the level is high, it falls a
            // millisecond in, the way every TZX player does it.
            if (level && sub == 0) {
                hold(T_PER_MS);
                level = false;
                sub = 1;
                if (!out_left) return false;
            }
            if (sub == 1) t = t > T_PER_MS ? t - T_PER_MS : 1u;
            hold(t);
            return true;
        }

        case S_DIRECT:
            while (ctr < s->a && out_left) {
                const bool bit = ((rd8(s->p + ctr / 8u) >> (7u - ctr % 8u)) & 1u) != 0;
                // Run-length the samples: a direct recording is mostly runs.
                uint32_t run = 0;
                while (ctr < s->a &&
                       (((rd8(s->p + ctr / 8u) >> (7u - ctr % 8u)) & 1u) != 0) == bit) {
                    run++;
                    ctr++;
                }
                level = bit;
                hold(run * s->b);
            }
            return ctr >= s->a;

        case S_CSW:
            while (ctr < s->a && out_left) {
                uint32_t n = rd8(s->p + ctr);
                ctr++;
                if (n == 0) { n = rd32(s->p + ctr); ctr += 4; }
                const uint32_t t = (uint32_t)(((uint64_t)n * 3500000u + s->b / 2u) / s->b);
                edge(t ? t : 1u);
            }
            return ctr >= s->a;

        case S_GDB:
            if (!cur) { gdb_setup(s->p); cur = 1; }
            return run_gdb();

        case S_PZX_PULS:
            // Starts low; each entry is a count and a duration, and every
            // pulse toggles -- except the very first, which is the level
            // the block starts at.
            if (!cur) { level = false; cur = 1; sub = 0; }
            while (ctr < s->a && out_left) {
                uint32_t count = 1, d = rd16(s->p + ctr);
                ctr += 2;
                if (d > 0x8000u) { count = d & 0x7FFFu; d = rd16(s->p + ctr); ctr += 2; }
                if (d >= 0x8000u) { d = ((d & 0x7FFFu) << 16) | rd16(s->p + ctr); ctr += 2; }
                // A run longer than the output has room for is put back.
                while (count && out_left) {
                    if (sub) level = !level;
                    sub = 1;
                    if (d) hold(d);
                    count--;
                }
                if (count) {
                    // Rewind to replay the rest of this entry next time.
                    // Cheap and rare: only a very long run hits it.
                    ctr -= 2;
                    if (d > 0x7FFFu) ctr -= 2;
                    ctr -= 2;
                    return false;
                }
            }
            return ctr >= s->a;

        case S_PZX_DATA: {
            const uint32_t bits = rd32(s->p) & 0x7FFFFFFFu;
            const uint32_t tail = rd16(s->p + 4);
            const uint32_t p0 = rd8(s->p + 6), p1 = rd8(s->p + 7);
            const uint32_t s0 = s->p + 8, s1 = s0 + 2u * p0, data = s1 + 2u * p1;
            if (!cur) { level = (rd32(s->p) >> 31) != 0; cur = 1; sub = 0; ctr = 0; }
            while (ctr < bits && out_left) {
                const uint32_t bit = (rd8(data + ctr / 8u) >> (7u - ctr % 8u)) & 1u;
                const uint32_t np = bit ? p1 : p0, sp = bit ? s1 : s0;
                hold(rd16(sp + 2u * sub));
                level = !level;
                if (++sub >= np) { sub = 0; ctr++; }
            }
            if (ctr >= bits && out_left) {
                if (tail) hold(tail);
                return true;
            }
            return false;
        }

        case S_LEVEL:
            level = s->a != 0;
            return true;

        case S_STOP:
            state = PULSE_STOPPED;
            return true;
    }
    return true;
}

/* Set up the next block that has anything to play. */
static bool next_block(void) {
    while (blk_no < blk_count) {
        st_n = st_i = 0;
        ctr = sub = cur = 0;
        const uint32_t p = blk_off[blk_no];
        bool plays;
        switch (fmt) {
            case PULSE_TAP: {
                const uint32_t len = rd16(p);
                plan_data(tm->pilot, rd8(p + 2) < 0x80 ? tm->pilot_hdr : tm->pilot_data,
                          tm->sync1, tm->sync2, tm->bit0, tm->bit1, 8,
                          tm->pause / T_PER_MS, p + 2, len);
                plays = true;
                break;
            }
            case PULSE_TZX: plays = plan_tzx(p); break;
            case PULSE_PZX: plays = plan_pzx(p); break;
            case PULSE_CSW: plan_csw_file(); plays = true; break;
            default:        plays = false; break;
        }
        if (plays && st_n) { blk_started = true; return true; }
        blk_no++;
    }
    return false;
}

uint32_t pulse_fill(uint32_t *out, const uint32_t max) {
    out_p = out;
    out_left = max;

    while (out_left && state == PULSE_RUNNING) {
        if (!blk_started && !next_block()) { state = PULSE_END; break; }

        if (run_stage(&st[st_i])) {
            ctr = sub = cur = 0;
            if (++st_i >= st_n) {
                blk_started = false;
                blk_no++;
            }
        }
    }
    return max - out_left;
}

// ---------------------------------------------------------------------------
// Describing the tape
// ---------------------------------------------------------------------------
/* A standard header's name, if this data is one. */
static void header_name(const uint32_t data, const uint32_t len, char *out, const unsigned n) {
    static const char *const kinds[4] = { "Program", "Numbers", "Chars", "Bytes" };
    if (len == 19 && rd8(data) == 0x00 && rd8(data + 1) < 4) {
        char name[11];
        for (int i = 0; i < 10; i++) {
            const uint8_t c = (uint8_t)rd8(data + 2 + (uint32_t)i);
            name[i] = (c >= 32 && c < 127) ? (char)c : '?';
        }
        name[10] = 0;
        snprintf(out, n, "%s: %s", kinds[rd8(data + 1)], name);
    } else {
        snprintf(out, n, "%s, %lu bytes", rd8(data) == 0x00 ? "Header" : "Data",
                 (unsigned long)(len >= 2 ? len - 2u : len));
    }
}

bool pulse_block_describe(const uint32_t n, char *out, const unsigned out_len) {
    if (n >= blk_count) return false;
    const uint32_t p = blk_off[n];
    switch (fmt) {
        case PULSE_TAP: header_name(p + 2, rd16(p), out, out_len); return true;
        case PULSE_CSW: snprintf(out, out_len, "CSW recording"); return true;
        case PULSE_PZX: {
            const uint32_t tag = rd32(p);
            char t[5] = { (char)(tag & 0xFF), (char)((tag >> 8) & 0xFF),
                          (char)((tag >> 16) & 0xFF), (char)(tag >> 24), 0 };
            snprintf(out, out_len, "PZX %s, %lu bytes", t, (unsigned long)rd32(p + 4));
            return true;
        }
        case PULSE_TZX: {
            const uint32_t id = rd8(p), b = p + 1;
            switch (id) {
                case 0x10: header_name(b + 4, rd16(b + 2), out, out_len); return true;
                case 0x11: snprintf(out, out_len, "Turbo data, %lu bytes", (unsigned long)rd24(b + 0x0F)); return true;
                case 0x12: snprintf(out, out_len, "Tone"); return true;
                case 0x13: snprintf(out, out_len, "Pulses"); return true;
                case 0x14: snprintf(out, out_len, "Pure data, %lu bytes", (unsigned long)rd24(b + 7)); return true;
                case 0x15: snprintf(out, out_len, "Direct recording"); return true;
                case 0x18: snprintf(out, out_len, "CSW recording"); return true;
                case 0x19: snprintf(out, out_len, "Generalized data"); return true;
                case 0x20: snprintf(out, out_len, rd16(b) ? "Pause %lu ms" : "Stop the tape",
                                    (unsigned long)rd16(b)); return true;
                case 0x21: {
                    char name[32];
                    const uint32_t l = rd8(b) < sizeof name - 1 ? rd8(b) : (uint32_t)sizeof name - 1;
                    for (uint32_t i = 0; i < l; i++) name[i] = (char)rd8(b + 1 + i);
                    name[l] = 0;
                    snprintf(out, out_len, "Group: %s", name);
                    return true;
                }
                case 0x2A: snprintf(out, out_len, "Stop if 48K"); return true;
                case 0x30: {
                    char txt[32];
                    const uint32_t l = rd8(b) < sizeof txt - 1 ? rd8(b) : (uint32_t)sizeof txt - 1;
                    for (uint32_t i = 0; i < l; i++) txt[i] = (char)rd8(b + 1 + i);
                    txt[l] = 0;
                    snprintf(out, out_len, "%s", txt);
                    return true;
                }
                default: snprintf(out, out_len, "Record %02lX", (unsigned long)id); return true;
            }
        }
    }
    return false;
}

bool pulse_all_rom_loadable(void) {
    for (uint32_t i = 0; i < blk_count; i++) {
        const uint32_t p = blk_off[i];
        switch (fmt) {
            case PULSE_TAP: break;
            case PULSE_CSW: return false;
            case PULSE_PZX: {
                const uint32_t tag = rd32(p);
                if (tag == PZX_TAG('P', 'U', 'L', 'S')) {
                    // A standard pilot is fine; the trap skips it anyway.
                    break;
                }
                if (tag == PZX_TAG('D', 'A', 'T', 'A')) {
                    const uint32_t q = p + 8;
                    if (rd8(q + 6) != 2 || rd8(q + 7) != 2) return false;
                    if (rd16(q + 8) != 855 || rd16(q + 12) != 1710) return false;
                }
                break;
            }
            case PULSE_TZX:
                switch (rd8(p)) {
                    case 0x10: break;
                    case 0x11: case 0x12: case 0x13: case 0x14: case 0x15:
                    case 0x18: case 0x19: return false;
                    default: break;
                }
                break;
        }
    }
    return true;
}
