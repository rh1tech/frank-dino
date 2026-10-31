/*
 * frank-dino — an RP2350B acting as the whole machine around a real Z80
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-dino
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The chip cores beside this file are pico-speccy's
 * (https://github.com/drewpo28/pico-speccy, GPL-3.0, drew.po28), themselves
 * reductions of MAME's fm.cpp, ymf262.cpp and ym2413.cpp (Jarek Burczynski,
 * Tatsuyuki Satoh, GPL-2.0+) and of the SN76489 as documented by Maxim.
 * This file is only the C face sound.c talks to.
 */

#include "chips.h"

#include <new>
#include <string.h>

#include "OpnFm.h"
#include "OplFm.h"
#include "OpllFm.h"
#include "SnSound.h"

/*
 * Each chip is made on first use and kept: a machine that never touches
 * TurboSound FM never pays for two YM2203s. The objects come off the
 * heap, which is the SRAM the build leaves over -- core 0 renders them,
 * and a chip in the PSRAM would compete with core 1 for the XIP cache.
 */
static OpnFm  *opn[2];
static OplFm  *opl;
static OpllFm *opll;
static SnSound *sn;
static int     rate_hz = 44100;

/*
 * A chip is reset as it is made, not only when the machine is: a machine
 * reset resets the chips that exist, and one made afterwards started from
 * whatever its constructor left -- an OPLL made on its first write after
 * boot stayed silent until the next reset.
 */
static bool make(void) {
    for (int i = 0; i < 2; i++) {
        if (opn[i]) continue;
        opn[i] = new (std::nothrow) OpnFm();
        if (opn[i]) { opn[i]->setRates(TSFM_YM2203_CLOCK, rate_hz); opn[i]->reset(); }
    }
    return opn[0] && opn[1];
}

extern "C" {

void chips_init(const int rate) { rate_hz = rate; }

void chips_reset(void) {
    for (int i = 0; i < 2; i++) if (opn[i]) opn[i]->reset();
    if (opl)  opl->reset();
    if (opll) opll->reset();
    if (sn)   sn->reset();
}

void chips_opn_write(const int chip, const bool data, const uint8_t v) {
    if (!opn[chip & 1] && !make()) return;
    if (data) opn[chip & 1]->writeData(v); else opn[chip & 1]->writeAddr(v);
}

void chips_opl_write(const int port, const uint8_t v) {
    if (!opl) {
        opl = new (std::nothrow) OplFm();
        if (!opl) return;
        opl->setRates(OPL3_YMF262_CLOCK, rate_hz);
        opl->reset();
    }
    opl->write(port & 3, v);
}

void chips_opll_write(const bool data, const uint8_t v) {
    if (!opll) {
        opll = new (std::nothrow) OpllFm();
        if (!opll) return;
        opll->setRates(OPLL_YM2413_CLOCK, rate_hz);
        opll->reset();
    }
    if (data) opll->writeData(v); else opll->writeAddr(v);
}

void chips_sn_write(const int chip, const uint8_t v) {
    if (!sn) {
        sn = new (std::nothrow) SnSound();
        if (!sn) return;
        sn->setRates(SN76489_CLOCK, rate_hz);
        sn->reset();
    }
    sn->write(chip & 1, v);
}

/*
 * One sample of everything that exists, as a stereo pair at about the
 * AY's scale. The OPL3 is the only stereo chip; the rest go in the middle.
 */
void chips_sample(int32_t *l, int32_t *r) {
    int32_t L = 0, R = 0;
    if (opn[0] || opn[1]) {
        int16_t s = 0;
        if (opn[0]) opn[0]->gen(&s, 1, 0);
        if (opn[1]) opn[1]->gen(&s, 1, 0);
        // The YM2203 core speaks in +/-128 at full level where the OPL
        // cores reach +/-16k: pico-speccy mixes it at a half and theirs at
        // 1/128, so it is 64 here to sit against them -- and the AY -- the
        // way it does there. It was mixed raw, 36 dB down.
        L += s * 64; R += s * 64;
    }
    if (opl) {
        int16_t sl = 0, sr = 0;
        opl->gen(&sl, &sr, 1, 0);
        L += sl; R += sr;
    }
    if (opll) {
        int16_t s = 0;
        opll->gen(&s, 1, 0);
        L += s; R += s;
    }
    if (sn) {
        uint8_t s = 0;
        sn->gen(&s, 1, 0);
        const int32_t v = ((int32_t)s - 64) * 96;
        L += v; R += v;
    }
    *l = L;
    *r = R;
}

bool chips_any(void) { return opn[0] || opn[1] || opl || opll || sn; }

}
