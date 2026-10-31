/*
 * frank-dino — an RP2350B acting as the whole machine around a real Z80
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-dino
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Saving the running machine as a .z80. Core 0, with the machine paused.
 * See snapshot.c.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "launch.h"

/* The registers as the last dump found them, for the register page. */
extern z80_state_t snapshot_last;
extern bool        snapshot_last_valid;

bool snapshot_save(const char *path, char *detail, unsigned detail_len);

/* To /snapshots/SNAPnnnn.z80, the first free number. */
bool snapshot_save_new(char *detail, unsigned detail_len);

/* Quick-save slots 1-10, as /snapshots/slotN.z80. */
bool snapshot_quick_save(unsigned slot, char *detail, unsigned detail_len);
bool snapshot_quick_exists(unsigned slot);
void snapshot_quick_path(unsigned slot, char *out, unsigned n);

/* Dump the registers into snapshot_last, without saving anything. */
bool snapshot_peek_registers(char *detail, unsigned detail_len);
