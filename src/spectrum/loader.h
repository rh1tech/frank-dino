/*
 * frank-dino — an RP2350B acting as the whole machine around a real Z80
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-dino
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Getting something off the card and into the machine.
 *
 * Snapshots are the easy half and the interesting half. Easy, because a
 * snapshot is just RAM plus registers and both can be written directly.
 * Interesting, because the registers cannot — there is no way to load a
 * Z84C00's from outside it, so every snapshot ends by handing a
 * z80_state_t to spec_launch(), which builds a short program that makes
 * the CPU load them itself. See launch.h.
 *
 * Everything here runs on core 0 with the machine paused, because it
 * rewrites the RAM the CPU would otherwise be executing from.
 */
#pragma once

#include <stdbool.h>

#include "storage.h"

/*
 * Load `path` and start it.
 *
 * `kind` comes from the browser so the extension is only parsed once.
 * Fills `detail` with a sentence for the status line either way — what
 * was loaded, or why it was not.
 */
bool loader_load(const char *path, file_kind_t kind, char *detail, unsigned detail_len);
