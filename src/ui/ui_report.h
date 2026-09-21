/*
 * frank-dino — an RP2350B acting as the whole machine around a real Z80
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-dino
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * The boot report, in the menu's window.
 *
 * Stage 0 has a great deal to say and eight or nine seconds in which to
 * say it, and until now it said it in eighty columns of text mode while
 * everything else in the firmware was drawn in pico-speccy's bands and
 * panes. This is the same report on the same surface as the menus: the
 * rounded window, the header with the rainbow mark, a sub-header naming
 * the board, the report itself, and a footer carrying the tally.
 *
 * Fifty-one columns rather than eighty, so the long lines are word
 * wrapped with a hanging indent instead of being cut off. Nothing is
 * lost that the text mode showed -- what does not fit scrolls, exactly
 * as it did there, and the footer keeps the verdict on screen whatever
 * has scrolled past it.
 *
 * The body is redrawn from screen.c's retained ring on every new line
 * rather than scrolled, because scrolling would mean moving fifty-seven
 * kilobytes of PSRAM and redrawing fifteen lines of text costs almost
 * nothing next to that.
 */
#pragma once

#include <stdbool.h>

/*
 * Take the screen: draw the chrome and switch the video mode.
 *
 * Call once, after vga_init() and psram_init() -- the surface is in the
 * PSRAM, so with no chip fitted this does nothing at all and the text
 * mode stays up, which is the one configuration where the old report is
 * still what gets shown.
 */
void ui_report_begin(void);

/* Redraw the body and footer. Called by screen.c for every logged line. */
void ui_report_line_added(void);

/* True while the report owns the screen. */
bool ui_report_active(void);
