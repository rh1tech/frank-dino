/*
 * frank-dino — an RP2350B acting as the whole machine around a real Z80
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-dino
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * The report, on the screen and on the wire at the same time.
 *
 * Everything a stage-0 test says goes to both: the USB console for a
 * person at a terminal, and the text buffer for the capture stick, because "did the
 * board pass" has to be a question a script can answer without a person
 * leaning over the bench. scr_log() is the one call that does both, so
 * the two can never drift apart.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

/*
 * The colour roles are the menu's, so the names come from there.
 *
 * The browser and the report are drawn in the same sixteen roles as
 * everything else -- C_PANEL the body, C_PANEL_ALT the bands, C_SEL_BG
 * the selection bar, C_TEXT and C_TEXT_DIM the two weights of text --
 * and there is one definition of them, in ui_gfx.h, next to the
 * rasteriser that uses them. Two copies of an enumeration that has to
 * match a palette table is one copy too many.
 */
#include "ui_gfx.h"

#define ATTR(fg, bg) (uint8_t)(((bg) << 4) | (fg))

void scr_init(void);

/*
 * Hand any queued log lines to stdio. Call from core 0 only.
 *
 * scr_log() and friends are called from core 1, which drives the Z80's
 * clock and must never block. They write to the screen — which is just
 * memory — and queue the text here; this drains the queue.
 *
 * The reason is not politeness about latency. TinyUSB is not multicore
 * safe, and core 0 services it from a timer: a printf from core 1 races
 * tud_task() and corrupts it. That fault put core 1 into a state the
 * debugger reported as "unknown" and hung the board before the first
 * test result, intermittently enough to look like a hardware problem.
 */
void scr_pump(void);

/*
 * Print the whole retained report again.
 *
 * The tests finish in well under a second and a USB host takes one or two
 * to enumerate, so the live output is gone before anything is listening.
 * The screen always has it; this is how the console catches up. Bound to
 * a keypress by whoever owns the console at the time.
 */
void scr_replay(void);

/*
 * Append a line to the scrolling report region and to the console.
 *
 * The region is everything below the header; it scrolls, so a test that
 * prints more than fits still ends with its verdict on screen.
 */
void scr_log(uint8_t attr, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

/* One empty line. Its own call because scr_log(attr, "") is a printf with
 * a zero-length format, which is a warning in every build that has -Wall
 * and a reasonable thing for the compiler to complain about. */
void scr_blank(void);

/*
 * The retained report, oldest first, for whoever draws it.
 *
 * The ring is the only copy that outlives the call -- the text buffer
 * scrolls and the console queue is consumed -- so this is what the boot
 * screen redraws itself from. `ink` is the colour the line was logged
 * in, as a palette index.
 */
typedef enum { SCR_PLAIN = 0, SCR_PASS, SCR_FAIL } scr_verdict_t;

/*
 * Where a verdict line's three fields sit inside the flat string.
 *
 * verdict() formats "%-28s %-4s %s". The console reads the whole line;
 * the screen cuts it here so it can put the name on one row and the
 * detail, wrapped, on the next. Change the format and change these.
 */
#define SCR_VERDICT_AT 29
#define SCR_DETAIL_AT  34

unsigned    scr_report_lines(void);
const char *scr_report_line(unsigned i, uint8_t *ink, uint8_t *kind);
void        scr_report_tally(unsigned *pass, unsigned *fail);

/* Pass/fail lines, which is most of what stage 0 prints. */
void scr_pass(const char *name, const char *detail_fmt, ...) __attribute__((format(printf, 2, 3)));
void scr_fail(const char *name, const char *detail_fmt, ...) __attribute__((format(printf, 2, 3)));
