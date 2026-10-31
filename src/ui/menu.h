/*
 * frank-dino — an RP2350B acting as the whole machine around a real Z80
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-dino
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * The way in and out of the on-screen UI, and the keys that feed it.
 *
 * Modal and synchronous: menu_run() pauses the machine, hands the screen
 * to the ported browser or settings menu, and resumes when they are
 * done. That is a great deal simpler than an event-driven menu drawn
 * over a running Spectrum, and it is also the only shape that is safe
 * here -- the card may only be touched while the Z80 is parked, and
 * loading a file rewrites the RAM the CPU would otherwise be executing
 * from.
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

/*
 * Make the next menu_run() open the settings page at once.
 *
 * F12 means "settings" and F11 means "files", and the settings hang off
 * the browser -- so this is how the one becomes the other without a
 * second modal loop.
 */
void menu_open_settings(bool yes);

/* Feed a console byte to the menu while it is open. */
void menu_key(char c);

/* Whether the menu currently has the screen and the keyboard. */
bool menu_is_open(void);

/*
 * Block until a key arrives, from the console or the mailbox.
 *
 * Shared with the settings page, which is modal in the same way and for
 * the same reason -- and which must keep the audio DMA fed while it
 * waits, or the last block in the ring plays on a loop. See the
 * definition for why this reads stdin rather than waiting on the
 * mailbox alone.
 */
int menu_wait_key(void);

/* The same, giving up after `ms` and returning -1. */
int menu_wait_key_timeout(unsigned ms);

/* getchar_timeout_us(0), with the harness's memory commands taken out
 * on the way. See conmon.c. */
int console_getchar(void);

/*
 * Set by the settings page's "Re-read the card" action.
 *
 * The page knows nothing about directory listings; the browser does, and
 * picks this up when the page closes.
 */
extern bool menu_rescan_wanted;
