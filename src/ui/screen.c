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
    for (int i = 0; i < VGA_COLS; i++) { *c++ = ' '; *c++ = ATTR(C_TEXT_DIM, C_PANEL); }
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
    scr_clear(ATTR(C_TEXT_DIM, C_PANEL));
}

// ---------------------------------------------------------------------------
// The furniture
// ---------------------------------------------------------------------------
/*
 * pico-speccy's layout, in character cells.
 *
 * Theirs is drawn pixel by pixel with its own font and a rounded
 * one-pixel border; this board's UI is an 80x30 grid of 8x16 cells and
 * cannot have the corners or the hairline. What it can have -- and what
 * actually carries the design -- is the structure: a coloured header
 * strip with the rainbow at its left, a sub-header for where you are, a
 * body on a panel, a selection bar rather than a highlighted word, and a
 * footer of key hints in dim text. That is the part worth copying.
 */

void scr_fill(const int x, const int y, const int w, const int h, const uint8_t bg) {
    const uint8_t attr = ATTR(C_TEXT, bg);
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) {
            uint8_t *c = cell(x + i, y + j);
            c[0] = ' '; c[1] = attr;
        }
}

void scr_band(const int y, const uint8_t bg) { scr_fill(0, y, VGA_COLS, 1, bg); }

/*
 * The four-bar mark, which is the one piece of colour in the header.
 *
 * Theirs is a small sprite of diagonal bars; four solid cells is what
 * this grid can say, and it says the same thing.
 */
void scr_rainbow(const int x, const int y) {
    static const uint8_t bars[] = { C_ICON_R, C_ICON_Y, C_ACCENT, C_ICON_C };
    for (unsigned i = 0; i < sizeof bars; i++) {
        uint8_t *c = cell(x + (int)i, y);
        c[0] = 0xDB;                            // a full block
        c[1] = ATTR(bars[i], C_PANEL_ALT);
    }
}

void scr_page(const char *title, const char *right, const char *breadcrumb) {
    scr_fill(0, 0, VGA_COLS, VGA_ROWS, C_PANEL);

    // Header: the mark, the name, and whatever belongs at the far end.
    scr_band(0, C_PANEL_ALT);
    scr_rainbow(1, 0);
    scr_printf(6, 0, ATTR(C_WHITE, C_PANEL_ALT), "%s", title ? title : "");
    if (right && *right) {
        const int len = (int)strlen(right);
        scr_printf(VGA_COLS - len - 2, 0, ATTR(C_TEXT_DIM, C_PANEL_ALT), "%s", right);
    }

    // Sub-header: where you are.
    scr_band(1, C_PANEL);
    if (breadcrumb && *breadcrumb)
        scr_printf(2, 1, ATTR(C_TEXT_DIM, C_PANEL), "%.*s", VGA_COLS - 4, breadcrumb);

    for (int x = 0; x < VGA_COLS; x++) {
        uint8_t *c = cell(x, 2);
        c[0] = 0xC4; c[1] = ATTR(C_SEP, C_PANEL);
    }
}

void scr_footer(const char *hints) {
    scr_band(VGA_ROWS - 1, C_PANEL_ALT);
    if (hints && *hints)
        scr_printf(2, VGA_ROWS - 1, ATTR(C_TEXT_DIM, C_PANEL_ALT), "%.*s", VGA_COLS - 4, hints);
}
