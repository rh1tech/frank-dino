/*
 * frank-dino — an RP2350B acting as the whole machine around a real Z80
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-dino
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * FDI, UDI and Teledisk images of TR-DOS disks, unwrapped into the .trd
 * layout the Beta Disk keeps. See diskconv.c.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

bool diskconv_fdi(const uint8_t *src, uint32_t len, uint8_t *trd, uint32_t trd_max, uint32_t *out_len);
bool diskconv_udi(const uint8_t *src, uint32_t len, uint8_t *trd, uint32_t trd_max, uint32_t *out_len);

/*
 * A .td0 straight from the file: Teledisk compresses the whole image
 * after its header ("td" rather than "TD"), so it is read through the
 * LZH decoder into `scratch` first. `f` is a FIL *, at the start.
 */
bool diskconv_td0(void *f, uint8_t *scratch, uint32_t scratch_max,
                  uint8_t *trd, uint32_t trd_max, uint32_t *out_len);

#ifdef __cplusplus
}
#endif
