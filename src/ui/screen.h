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
 * Everything a stage-0 test says goes to both: UART for a person at a
 * terminal, and the text buffer for the capture stick, because "did the
 * board pass" has to be a question a script can answer without a person
 * leaning over the bench. scr_log() is the one call that does both, so
 * the two can never drift apart.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

// CGA-order colour numbers, for attributes.
enum {
    C_BLACK = 0, C_BLUE, C_GREEN, C_CYAN, C_RED, C_MAGENTA, C_BROWN, C_GREY,
    C_DGREY, C_LBLUE, C_LGREEN, C_LCYAN, C_LRED, C_LMAGENTA, C_YELLOW, C_WHITE
};
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
void scr_clear(uint8_t attr);
void scr_put(int x, int y, const char *s, uint8_t attr);
void scr_printf(int x, int y, uint8_t attr, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
void scr_box(int x, int y, int w, int h, const char *title, uint8_t attr);

/*
 * Append a line to the scrolling report region and to the UART.
 *
 * The region is everything below the header; it scrolls, so a test that
 * prints more than fits still ends with its verdict on screen.
 */
void scr_log(uint8_t attr, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

/* One empty line. Its own call because scr_log(attr, "") is a printf with
 * a zero-length format, which is a warning in every build that has -Wall
 * and a reasonable thing for the compiler to complain about. */
void scr_blank(void);

/* Pass/fail lines, which is most of what stage 0 prints. */
void scr_pass(const char *name, const char *detail_fmt, ...) __attribute__((format(printf, 2, 3)));
void scr_fail(const char *name, const char *detail_fmt, ...) __attribute__((format(printf, 2, 3)));
