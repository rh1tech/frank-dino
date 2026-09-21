/*
 * frank-dino — an RP2350B acting as the whole machine around a real Z80
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-dino
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * The file browser, ported from pico-speccy src/ui/UiBrowser.cpp.
 *
 * Their chrome and their row semantics over this firmware's directory
 * scan: the header with a position counter, the path bar, a list pane
 * with a scrollbar hairline, an info pane naming the focused file and
 * the verbs that apply to it, and a footer that becomes a Find prompt.
 *
 * What did not come across is the half of their file that is about
 * their storage rather than about browsing -- the on-card .idx cache
 * (SortedFiles.h), the per-type position memory, the ZIP and slot verbs,
 * the FTP and web-archive back ends. This board reads a directory with
 * storage_list() straight into an array and that is the whole model, so
 * their index machinery has nothing to index.
 */
#pragma once

#include <stdbool.h>

/*
 * Open the browser and stay in it until the user leaves.
 *
 * Returns true if something was loaded, so the caller knows the machine
 * it resumes is not the machine it paused. The machine must already be
 * paused: this reads the card and draws into the PSRAM.
 */
bool ui_browser_run(void);

/* Read the current directory again, after something changed it. */
void ui_browser_rescan(void);
