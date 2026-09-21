/*
 * frank-dino — an RP2350B acting as the whole machine around a real Z80
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-dino
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * The on-screen menu, and the file browser that is most of it.
 *
 * Modal and synchronous: menu_run() pauses the machine, takes over the
 * screen, loops until the user leaves, and resumes. That is a great deal
 * simpler than an event-driven menu drawn over a running Spectrum, and it
 * is also the only shape that is safe here — the card may only be touched
 * while the Z80 is parked, and loading a file rewrites the RAM the CPU
 * would otherwise be executing from.
 *
 * Runs on core 0, which owns the screen, the card and the console.
 */
#pragma once

#include <stdbool.h>

/*
 * Open the menu and stay in it until the user leaves.
 *
 * Returns true if something was loaded, so the caller knows the machine
 * it resumes is not the machine it paused.
 */
bool menu_run(void);

/* Feed a console byte to the menu while it is open. */
void menu_key(char c);

/* Whether the menu currently has the screen and the keyboard. */
bool menu_is_open(void);
