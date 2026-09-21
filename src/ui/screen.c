/*
 * frank-dino — an RP2350B acting as the whole machine around a real Z80
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-dino
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "screen.h"

#include "ui_report.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <hardware/sync.h>
#include <pico/mutex.h>
#include <pico/stdlib.h>

#include "vga.h"

/*
 * One lock over everything in this file that formats or prints.
 *
 * Core 1 runs the tests and formats their results; core 0 drains the
 * queue to stdio. Both go through the C library's printf machinery, and
 * that machinery is shared mutable state — two cores inside it at once
 * corrupt it.
 *
 * The symptom was not a crash in printf. It was core 1 wedging somewhere
 * in the GPIO wiring test, hundreds of lines away, at a point that moved
 * every time anything was added or removed — which is what a data race
 * looks like from the outside, and what sent this chasing a hardware
 * fault that was not there.
 *
 * Recursive, because scr_pass() formats a line and then calls log_line(),
 * which formats again.
 */
static recursive_mutex_t log_mutex;

/*
 * The queue between core 1 (which logs) and core 0 (which prints).
 *
 * Deliberately lossy: if core 1 outruns core 0 by more than LOG_QUEUE
 * lines the oldest are overwritten, and that is the right trade. The
 * screen already has every line, the console is the secondary copy, and
 * the alternative — making core 1 wait for a USB endpoint to drain — is
 * the thing this exists to avoid.
 */
#define LOG_QUEUE 32
#define LOG_WIDTH 112

static char              log_queue[LOG_QUEUE][LOG_WIDTH];
/*
 * The colour each retained line was logged in.
 *
 * The console gets text and the screen gets text plus an ink, and the
 * ring is the one copy of the report that outlives the call -- so the
 * ink belongs beside it rather than in a second ring that could fall out
 * of step with it. Thirty-two bytes.
 */
static uint8_t           log_queue_ink[LOG_QUEUE];
/*
 * Whether the line is a verdict, so the screen can lay it out as one.
 *
 * The console wants the flat "name PASS detail" that verdict() builds;
 * fifty-one columns wants the name on one row with the verdict against
 * the right edge and the detail wrapped underneath. Rather than keep the
 * three pieces twice, the ring keeps the flat line and this says where
 * to cut it -- the offsets are verdict()'s own format string, ten lines
 * below, and SCR_VERDICT_AT / SCR_DETAIL_AT are how the two stay in step.
 */
static uint8_t           log_queue_kind[LOG_QUEUE];   /* scr_verdict_t */
static volatile uint32_t log_head;      // written by core 1
static volatile uint32_t log_tail;      // written by core 0

/* Verdicts so far, for the report's footer. */
static uint16_t tally_pass, tally_fail;

/* Set by verdict() for the line it is about to log, cleared by log_line(). */
static uint8_t next_kind = SCR_PLAIN;

static void log_line(const uint8_t attr, const char *text) {
    const uint32_t h = log_head;
    strncpy(log_queue[h % LOG_QUEUE], text, LOG_WIDTH - 1);
    log_queue[h % LOG_QUEUE][LOG_WIDTH - 1] = 0;
    log_queue_ink[h % LOG_QUEUE]  = (uint8_t)(attr & 0x0F);
    log_queue_kind[h % LOG_QUEUE] = next_kind;
    next_kind = SCR_PLAIN;
    __dmb();                            // text before the index that publishes it
    log_head = h + 1;

    /*
     * ...and on the screen, which redraws itself from this ring.
     *
     * The ring is the whole model now. There is no character buffer
     * underneath it any more -- the report, the browser and the menus
     * are all drawn on the one 320x240 surface.
     */
    ui_report_line_added();
}

unsigned scr_report_lines(void) {
    const uint32_t h = log_head;
    return (unsigned)(h > LOG_QUEUE ? LOG_QUEUE : h);
}

const char *scr_report_line(const unsigned i, uint8_t *ink, uint8_t *kind) {
    const uint32_t h = log_head;
    const uint32_t first = h > LOG_QUEUE ? h - LOG_QUEUE : 0;
    if (first + i >= h) return NULL;
    const uint32_t k = (first + i) % LOG_QUEUE;
    if (ink)  *ink  = log_queue_ink[k];
    if (kind) *kind = log_queue_kind[k];
    return log_queue[k];
}

void scr_report_tally(unsigned *pass, unsigned *fail) {
    if (pass) *pass = tally_pass;
    if (fail) *fail = tally_fail;
}

void scr_replay(void) {
    const uint32_t h = log_head;
    const uint32_t first = h > LOG_QUEUE ? h - LOG_QUEUE : 0;
    puts("--- report ---");
    for (uint32_t i = first; i < h; i++) puts(log_queue[i % LOG_QUEUE]);
    puts("--- end ---");
}

void scr_pump(void) {
    if (!recursive_mutex_try_enter(&log_mutex, NULL)) return;   // core 1 is busy
    while (log_tail != log_head) {
        const uint32_t t = log_tail;
        // Skip anything core 1 has already lapped rather than printing a
        // line that is being overwritten as it is read.
        if (log_head - t > LOG_QUEUE) { log_tail = log_head - LOG_QUEUE; continue; }
        puts(log_queue[t % LOG_QUEUE]);
        log_tail = t + 1;
    }

    recursive_mutex_exit(&log_mutex);
}

void scr_blank(void) {
    recursive_mutex_enter_blocking(&log_mutex);
    log_line(ATTR(C_TEXT_DIM, C_PANEL), "");
    recursive_mutex_exit(&log_mutex);
}

void scr_log(const uint8_t attr, const char *fmt, ...) {
    recursive_mutex_enter_blocking(&log_mutex);
    char buf[VGA_COLS];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    log_line(attr, buf);
    recursive_mutex_exit(&log_mutex);
}

/*
 * A verdict line, laid out so the column of PASS/FAIL is a column.
 *
 * The name is padded to a fixed width rather than separated by a space,
 * because the fastest way to read a screenshot of twenty results is to
 * find the one word that is not the same colour as the others, and that
 * only works if they line up.
 */
static void verdict(const bool ok, const char *name, const char *detail) {
    // Wide enough for the worst case rather than for the screen: the
    // console gets the whole line and scr_put() is what clips it to
    // eighty columns. Sizing this to VGA_COLS would have truncated the
    // detail on the wire too, where there is room for it.
    char buf[VGA_COLS + 48];
    snprintf(buf, sizeof buf, "%-28s %-4s %s", name, ok ? "PASS" : "FAIL", detail);
    if (ok) tally_pass++; else tally_fail++;
    next_kind = ok ? SCR_PASS : SCR_FAIL;
    log_line(ok ? ATTR(C_ACCENT, C_PANEL) : ATTR(C_ICON_R, C_PANEL), buf);
}

void scr_pass(const char *name, const char *fmt, ...) {
    recursive_mutex_enter_blocking(&log_mutex);
    char d[VGA_COLS];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(d, sizeof d, fmt, ap);
    va_end(ap);
    verdict(true, name, d);
    recursive_mutex_exit(&log_mutex);
}

void scr_fail(const char *name, const char *fmt, ...) {
    recursive_mutex_enter_blocking(&log_mutex);
    char d[VGA_COLS];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(d, sizeof d, fmt, ap);
    va_end(ap);
    verdict(false, name, d);
    recursive_mutex_exit(&log_mutex);
}

void scr_init(void) {
    recursive_mutex_init(&log_mutex);
}

