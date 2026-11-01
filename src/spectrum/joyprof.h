/*
 * frank-dino — an RP2350B acting as the whole machine around a real Z80
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-dino
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Named pad maps on the card, in /joystick.cfg. See joyprof.c.
 */
#pragma once

#include <stdbool.h>

#define JOYPROF_MAX  16
#define JOYPROF_NAME 24

/* How many profiles the file holds, and the name of one. Reads the file. */
int         joyprof_count(void);
const char *joyprof_name(int i);

/* Save the current map (and joystick type) under `name`, replacing one of
 * that name; load profile `i` into the settings. */
bool joyprof_save(const char *name);
bool joyprof_load(int i);
