/*
 * frank-dino — an RP2350B acting as the whole machine around a real Z80
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-dino
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * A machine's ROM from the card instead of flash. See customrom.c.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "spectrum.h"

/* The file name a machine's own ROM is looked for under, in /roms, or NULL
 * for the machines whose ROM sets are too big or too many to swap. */
const char *customrom_tag(machine_t m, unsigned *banks);

/* Read /roms/<tag>.rom into `banks` 16 KB slots when it is exactly that
 * size; false (and nothing touched) otherwise. Core 0, machine paused. */
bool customrom_load(machine_t m, uint8_t *const *slot, unsigned banks);

/* /roms/trdos.rom, 16 KB, into `dst`. */
bool customrom_trdos(uint8_t *dst);

/* What the last reset found, for the menu. */
extern bool customrom_active, customrom_trdos_active;
