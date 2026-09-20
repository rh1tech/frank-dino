/*
 * frank-dino — an RP2350B acting as the whole machine around a real Z80
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-dino
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "screen.h"

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

// The header is fixed and the rest scrolls. LOG_TOP is the first line
// that belongs to the report.
#define LOG_TOP 6
#define LOG_ROWS (VGA_ROWS - LOG_TOP)

static int log_y = LOG_TOP;

static inline uint8_t *cell(const int x, const int y) {
    return &vga_text[(y * VGA_COLS + x) * 2];
}

void scr_clear(const uint8_t attr) {
    for (int i = 0; i < VGA_COLS * VGA_ROWS; i++) {
        vga_text[i * 2]     = ' ';
        vga_text[i * 2 + 1] = attr;
    }
    log_y = LOG_TOP;
}

void scr_put(const int x, const int y, const char *s, const uint8_t attr) {
    if (y < 0 || y >= VGA_ROWS) return;
    uint8_t *c = cell(x, y);
    for (int i = x; *s && i < VGA_COLS; i++) {
        *c++ = (uint8_t)*s++;
        *c++ = attr;
    }
}

void scr_printf(const int x, const int y, const uint8_t attr, const char *fmt, ...) {
    recursive_mutex_enter_blocking(&log_mutex);
    char buf[VGA_COLS + 1];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    scr_put(x, y, buf, attr);
    recursive_mutex_exit(&log_mutex);
}

void scr_box(const int x, const int y, const int w, const int h,
             const char *title, const uint8_t attr) {
    // CP437 single-line box drawing: the font is the one xt8086 uses, so
    // these are the codes it has glyphs for.
    for (int i = 1; i < w - 1; i++) {
        cell(x + i, y)[0]         = 0xC4; cell(x + i, y)[1]         = attr;
        cell(x + i, y + h - 1)[0] = 0xC4; cell(x + i, y + h - 1)[1] = attr;
    }
    for (int i = 1; i < h - 1; i++) {
        cell(x, y + i)[0]         = 0xB3; cell(x, y + i)[1]         = attr;
        cell(x + w - 1, y + i)[0] = 0xB3; cell(x + w - 1, y + i)[1] = attr;
    }
    cell(x, y)[0]                 = 0xDA; cell(x, y)[1]                 = attr;
    cell(x + w - 1, y)[0]         = 0xBF; cell(x + w - 1, y)[1]         = attr;
    cell(x, y + h - 1)[0]         = 0xC0; cell(x, y + h - 1)[1]         = attr;
    cell(x + w - 1, y + h - 1)[0] = 0xD9; cell(x + w - 1, y + h - 1)[1] = attr;

    if (title && *title) {
        const int len = (int)strlen(title);
        scr_printf(x + (w - len - 2) / 2, y, attr, " %s ", title);
    }
}

static void log_scroll(void) {
    memmove(cell(0, LOG_TOP), cell(0, LOG_TOP + 1),
            (size_t)(LOG_ROWS - 1) * VGA_COLS * 2);
    uint8_t *c = cell(0, VGA_ROWS - 1);
    for (int i = 0; i < VGA_COLS; i++) { *c++ = ' '; *c++ = ATTR(C_GREY, C_BLACK); }
}

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
static volatile uint32_t log_head;      // written by core 1
static volatile uint32_t log_tail;      // written by core 0

static void log_line(const uint8_t attr, const char *text) {
    if (log_y >= VGA_ROWS) {
        log_scroll();
        log_y = VGA_ROWS - 1;
    }
    scr_put(1, log_y++, text, attr);

    const uint32_t h = log_head;
    strncpy(log_queue[h % LOG_QUEUE], text, LOG_WIDTH - 1);
    log_queue[h % LOG_QUEUE][LOG_WIDTH - 1] = 0;
    __dmb();                            // text before the index that publishes it
    log_head = h + 1;
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
    log_line(ATTR(C_GREY, C_BLACK), "");
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
    log_line(ok ? ATTR(C_LGREEN, C_BLACK) : ATTR(C_LRED, C_BLACK), buf);
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
    scr_clear(ATTR(C_GREY, C_BLACK));
}
