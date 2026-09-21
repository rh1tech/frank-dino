/*
 * frank-dino — an RP2350B acting as the whole machine around a real Z80
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-dino
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * The settings page.
 *
 * One screen, one list, values on the right, left and right to change
 * them. pico-speccy's menu is a tree because it has twenty machines and
 * six sound cards to fit in it; this has one screenful, so a tree would
 * be three keystrokes where one would do.
 *
 * Modal, and called with the machine already paused -- it is opened from
 * the file browser, which pauses on the way in.
 */
#pragma once

#include <stdbool.h>

/*
 * Run the page until the user leaves it.
 *
 * Returns true if anything changed, which is what the browser uses to
 * decide whether to offer to save.
 */
bool options_run(void);
