/*
 * frank-dino — an RP2350B acting as the whole machine around a real Z80
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-dino
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Frame timings (first paper T-state, T-states a line) as Fuse and
 * pico-speccy (https://github.com/drewpo28/pico-speccy, GPL-3.0) have
 * them, moved to this firmware's frame origin -- T-state 0 is where
 * ~INT goes down.
 */

#include "ula_frame.h"

#include <string.h>

#include "hardware/sync.h"

#include "settings.h"
#include "spectrum.h"
#include "z80_bus.h"

volatile bool ula_beam;

static ula_frame_t uf[3];
static volatile int8_t ready = -1;      // the newest finished frame
static volatile int8_t front = -1;      // the one core 0 is showing
static int8_t          build;

/* Which cells have been frozen this frame, a bit per column per line. */
static uint32_t fz_b[192], fz_a[192];

static uint32_t paper_t0, line_t;

// ---------------------------------------------------------------------------
// When the ULA fetches what
// ---------------------------------------------------------------------------
/*
 * The ULA reads a cell's bitmap and attribute in the same four T-states,
 * two cells to an eight-T-state group, starting paper_t0 into the frame
 * and line_t further on for every line below the first.
 */
__force_inline static uint32_t fetch_t(const uint32_t y, const uint32_t c) {
    return paper_t0 + y * line_t + c * 4u;
}

static void timings(void) {
    const uint32_t num = 2u + settings.turbo;
    uint32_t t0, lt;
    if (spec_is_48k_class(spec_machine) || spec_machine == MACHINE_TIMEX
        || spec_kay_level(spec_machine)) {
        t0 = 14336u; lt = 224u;
    } else if (spec_is_pentagon_class(spec_machine) || spec_machine == MACHINE_SCORPEVO) {
        t0 = 17988u; lt = 224u;
    } else {
        t0 = 14362u; lt = 228u;
    }
    paper_t0 = t0 * num / 2u;
    line_t   = lt * num / 2u;
}

// ---------------------------------------------------------------------------
// Core 1
// ---------------------------------------------------------------------------
static void __not_in_flash_func(ula_wwatch)(const uint16_t a, const uint8_t v, const uint32_t t) {
    (void)v;
    const uint32_t o = a & 0x1FFFu;
    if (o >= 0x1B00u) return;
    const uint8_t old = z80_rpage[a >> 13][o];
    ula_frame_t *const f = &uf[build];
    const uint32_t c = o & 31u;
    const uint32_t bit = 1u << c;

    if (o < 0x1800u) {
        const uint32_t y = ((o >> 5) & 0xC0u) | ((o >> 2) & 0x38u) | ((o >> 8) & 0x07u);
        if (t < fetch_t(y, c) || (fz_b[y] & bit)) return;
        fz_b[y] |= bit;
        f->bmp[y][c] = old;
        return;
    }

    // An attribute is fetched once on each of its eight lines.
    const uint32_t row = (o - 0x1800u) >> 5;
    for (uint32_t y = row * 8u; y < row * 8u + 8u; y++) {
        if (t < fetch_t(y, c)) break;
        if (fz_a[y] & bit) continue;
        fz_a[y] |= bit;
        f->att[y][c] = old;
    }
}

void ula_rewatch(void) {
    if (!ula_beam) return;
    const uint8_t *const scr = spec_ram[spec_screen_bank];
    uint8_t m = 0;
    for (unsigned w = 0; w < Z80_PAGES; w++)
        if (z80_wpage[w] == scr) m |= (uint8_t)(1u << w);
    z80_wwatch_cb   = ula_wwatch;
    z80_wwatch_mask = m;
}

void ula_frame_begin(void) {
    if (!ula_beam) return;
    // Any buffer that is neither finished nor on the screen.
    int8_t b = 0;
    while (b == ready || b == front) b++;
    build = b;
    timings();
    ula_frame_t *const f = &uf[build];
    f->paper_t0 = paper_t0;
    f->line_t   = line_t;
    f->border_at_start = spec_border;
    f->nev = 0;
    memset(fz_b, 0, sizeof fz_b);
    memset(fz_a, 0, sizeof fz_a);
    ula_rewatch();
}

void __not_in_flash_func(ula_border)(const uint32_t t, const uint8_t colour) {
    ula_frame_t *const f = &uf[build];
    // Full: the last change wins. Coarser, never wrong at the frame's end.
    const uint32_t i = f->nev < ULA_EV_MAX ? f->nev++ : ULA_EV_MAX - 1u;
    f->ev[i] = ULA_EV(t, colour);
}

void ula_frame_end(void) {
    if (!ula_beam) return;
    ula_frame_t *const f = &uf[build];
    const uint8_t *const scr = spec_ram[spec_screen_bank];

    for (uint32_t y = 0; y < 192u; y++) {
        const uint32_t boff = ((y & 0xC0u) << 5) | ((y & 0x38u) << 2) | ((y & 0x07u) << 8);
        const uint32_t aoff = 0x1800u + ((y >> 3) << 5);
        const uint32_t zb = fz_b[y], za = fz_a[y];
        if (!zb) memcpy(f->bmp[y], scr + boff, 32);
        else for (uint32_t c = 0; c < 32u; c++) if (!(zb & (1u << c))) f->bmp[y][c] = scr[boff + c];
        if (!za) memcpy(f->att[y], scr + aoff, 32);
        else for (uint32_t c = 0; c < 32u; c++) if (!(za & (1u << c))) f->att[y][c] = scr[aoff + c];
    }
    __dmb();
    ready = build;
}

void ula_set_enabled(const bool on) {
    ula_beam = on;
    if (!on) {
        z80_wwatch_mask = 0;
        ready = front = -1;
    }
}

// ---------------------------------------------------------------------------
// Core 0
// ---------------------------------------------------------------------------
const ula_frame_t *__not_in_flash_func(ula_frame_latest)(void) {
    if (!ula_beam) return NULL;
    const int8_t r = ready;
    if (r >= 0) front = r;
    return front >= 0 ? &uf[front] : NULL;
}
