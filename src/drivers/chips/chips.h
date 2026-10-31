/*
 * frank-dino — an RP2350B acting as the whole machine around a real Z80
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-dino
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * The FM and PSG chips beyond the AY and the SAA: TurboSound FM's two
 * YM2203s, the VGM card's YMF262, YM2413 and SN76489 pair. All rendered
 * on core 0 from sound.c, one sample at a time. See chips.cpp.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void chips_init(int rate_hz);
void chips_reset(void);

void chips_opn_write(int chip, bool data, uint8_t v);   // YM2203: register, then data
void chips_opl_write(int port, uint8_t v);              // YMF262: #C4-#C7 as ports 0-3
void chips_opll_write(bool data, uint8_t v);            // YM2413: #C0 register, #C1 data
void chips_sn_write(int chip, uint8_t v);               // SN76489 pair

void chips_sample(int32_t *l, int32_t *r);
bool chips_any(void);

#ifdef __cplusplus
}
#endif
