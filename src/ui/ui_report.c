/*
 * frank-dino — an RP2350B acting as the whole machine around a real Z80
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-dino
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "ui_report.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include <hardware/clocks.h>

#include "psram.h"
#include "screen.h"
#include "ui_gfx.h"
#include "ui_render.h"
#include "vga.h"

static bool active;

bool ui_report_active(void) { return active; }

/* ── word wrap ───────────────────────────────────────────────────────── */

/*
 * One row of `*p`, advancing it past what was taken.
 *
 * Breaks on the last space that fits, and on the character boundary when
 * a single word is longer than the row -- which the report produces on
 * its own ("11000000", a filename). `indent` is applied to every row
 * after the first, so a wrapped detail cannot be mistaken for the next
 * line of the report.
 *
 * Returns false once the string is exhausted.
 */
static bool wrap_next(const char **p, int cols, const int indent, const bool first,
                      char *out, const size_t outsz) {
    const char *s = *p;
    if (!*s) return false;

    const int lead = first ? 0 : indent;
    int room = cols - lead;
    if (room < 1) room = 1;
    if ((size_t)(room + lead) + 1 > outsz) return false;

    int take = 0;
    while (s[take] && take < room) take++;

    if (s[take]) {                              /* more to come: break on a space */
        int brk = take;
        while (brk > 0 && s[brk] != ' ') brk--;
        if (brk > 0) take = brk;
    }

    int n = 0;
    for (; n < lead; n++) out[n] = ' ';
    memcpy(out + n, s, (size_t)take);
    out[n + take] = 0;

    s += take;
    while (*s == ' ') s++;                      /* the break itself */
    *p = s;
    return true;
}

static int wrap_rows(const char *s, const int cols, const int indent) {
    char buf[80];
    int n = 0;
    while (wrap_next(&s, cols, indent, n == 0, buf, sizeof buf)) n++;
    return n ? n : 1;                           /* a blank line is still a row */
}

/* As above, with the first row indented too. */
static int wrap_rows_cont(const char *s, const int cols, const int indent) {
    char buf[80];
    int n = 0;
    while (wrap_next(&s, cols, indent, false, buf, sizeof buf)) n++;
    return n;
}

/*
 * How a retained line is laid out, and how many rows it takes.
 *
 * A verdict is its name on one row with PASS or FAIL against the right
 * edge, and its detail wrapped and indented underneath -- the eighty
 * column form verdict() builds for the console does not fit here and
 * wraps into something unreadable. Anything else is just wrapped.
 */
#define DETAIL_INDENT 2

static const char *verdict_detail(const char *line) {
    const size_t n = strlen(line);
    return n > SCR_DETAIL_AT ? line + SCR_DETAIL_AT : "";
}

static void verdict_name(const char *line, char *out, const size_t outsz) {
    size_t n = 0;
    while (n < SCR_VERDICT_AT && n + 1 < outsz && line[n]) { out[n] = line[n]; n++; }
    while (n && out[n - 1] == ' ') n--;         /* verdict() pads it to 28 */
    out[n] = 0;
}

/*
 * A verdict whose detail fits beside its name stays on one row.
 *
 * "USB keyboard ... enumerated on the USB-A port PASS" is 45 of the 51
 * columns and reads as one fact; splitting it over two rows to match the
 * ones that genuinely need two costs six rows across the report, which
 * is six lines of it pushed off the bottom.
 */
/* Two spaces between the three fields, which is the gap drawn below. */
#define VERDICT_GAP 2

static bool verdict_fits(const char *name, const char *detail, const int cols) {
    if (!*detail) return true;
    return (int)(strlen(name) + VERDICT_GAP + strlen(detail) + VERDICT_GAP + 4) <= cols;
}

static int line_rows(const char *line, const uint8_t kind, const int cols) {
    if (kind == SCR_PLAIN) return wrap_rows(line, cols, DETAIL_INDENT);
    char name[SCR_VERDICT_AT + 1];
    verdict_name(line, name, sizeof name);
    const char *d = verdict_detail(line);
    if (verdict_fits(name, d, cols)) return 1;
    /* Every row of the detail is indented under the name, the first
     * included -- it is subordinate to it, not a line of its own. */
    return 1 + wrap_rows_cont(d, cols, DETAIL_INDENT);
}

/* ── chrome ──────────────────────────────────────────────────────────── */

/*
 * The sub-header: what the board is, and how fast it is being run.
 *
 * The same band the menu puts its breadcrumb in, carrying what the old
 * report's two header lines said between them.
 */
static void draw_sub_header(void) {
    const int y = LY.iy + LY.hdr_h;
    ui_fill(LY.ix, y, LY.iw, LY.sub_h, C_PANEL_ALT);

    ui_text(LY.ix + LY.pad, y + 2, "dino_z80  Z84C00 + RP2350B", C_WHITE);

    char clk[16];
    snprintf(clk, sizeof clk, "%u MHz", (unsigned)(clock_get_hz(clk_sys) / MHZ));
    ui_text(LY.ix + LY.iw - ui_text_width(clk) - LY.pad, y + 2, clk, C_TEXT_DIM);

    ui_hline(LY.ix, y + LY.sub_h - 1, LY.iw, C_SEP);
}

/*
 * The footer: the wiring on the left, the verdict on the right.
 *
 * The tally is the part that matters. The body scrolls once the report
 * outgrows it, so on a board that failed its first test and passed the
 * rest the evidence would be gone by the time anyone looked -- this is
 * the line that keeps it, and it is the line a photograph of the bench
 * is taken for.
 */
static void draw_footer(void) {
    const int y = LY.iy + LY.ih - LY.foot_h;
    ui_fill(LY.ix, y, LY.iw, LY.foot_h, C_FOOT_BG);
    ui_hline(LY.ix, y, LY.iw, C_SEP);

    ui_text(LY.ix + LY.pad, y + 3, "640x480 HDMI   console 115200 on J1", C_TEXT_DIM);

    unsigned pass, fail;
    scr_report_tally(&pass, &fail);
    char t[24];
    if (fail) snprintf(t, sizeof t, "%u failed", fail);
    else      snprintf(t, sizeof t, "%u passed", pass);
    ui_text(LY.ix + LY.iw - ui_text_width(t) - LY.pad, y + 3, t,
            fail ? C_ICON_R : C_ACCENT);
}

/* ── the body ────────────────────────────────────────────────────────── */

/* Full width: the report is one column, not the menu's two. */
static int body_cols(void) { return (LY.iw - 2 * LY.pad) / ui_glyph_w(); }

/*
 * The report's rows are tighter than the menu's.
 *
 * A menu row is UI_FONT_H + 2 because a selection bar needs air around
 * the text inside it. Nothing here is ever highlighted, and the report
 * is long enough that two pixels a row is two rows of it -- so the pitch
 * is the glyph plus one, which is as close as the descenders allow.
 */
#define REP_ROW_H (UI_FONT_H + 1)

static int body_rows(void) { return LY.body_h / REP_ROW_H; }

/*
 * Redraw the report, ending on the newest line.
 *
 * Walked backwards first to find where to start -- counting wrapped rows
 * from the newest line until the body is full -- then forwards to draw.
 * That is two passes over the ring instead of one, and it is what avoids
 * a buffer of wrapped rows: there is no room in the SRAM for one and no
 * reason to have one.
 */
static void draw_body(void) {
    const int cols = body_cols();
    const int rows = body_rows();
    const unsigned n = scr_report_lines();

    /* Walk back from the newest line until the body is full, then draw
     * forwards from there. Two passes over the ring instead of one, and
     * it is what avoids a buffer of wrapped rows -- there is no room in
     * the SRAM for one and no reason to have one. */
    unsigned start = n;
    int skip = 0, total = 0;
    while (start > 0) {
        uint8_t kind = SCR_PLAIN;
        const char *s = scr_report_line(start - 1, NULL, &kind);
        const int r = line_rows(s, kind, cols);
        if (total + r > rows) { skip = total + r - rows; start--; break; }
        total += r;
        start--;
    }

    ui_fill(LY.ix, LY.body_y, LY.iw, LY.body_h, C_PANEL);

    const int ymax = LY.body_y + LY.body_h;
    int y = LY.body_y;
    char buf[80];

    for (unsigned i = start; i < n && y + REP_ROW_H <= ymax; i++) {
        uint8_t ink = C_TEXT, kind = SCR_PLAIN;
        const char *line = scr_report_line(i, &ink, &kind);
        if (!line) break;
        int row = 0;

        if (kind != SCR_PLAIN) {
            const char *passfail = (kind == SCR_PASS) ? "PASS" : "FAIL";
            const int vw = ui_text_width(passfail);
            char name[SCR_VERDICT_AT + 1];
            verdict_name(line, name, sizeof name);
            const char *d = verdict_detail(line);
            const bool oneline = verdict_fits(name, d, cols);

            if (i != start || row++ >= skip) {
                int x = LY.ix + LY.pad;
                x += ui_text_clip(x, y + 1, LY.iw - 3 * LY.pad - vw, name, C_TEXT);
                if (oneline && *d) {
                    x += VERDICT_GAP * ui_glyph_w();
                    ui_text_clip(x, y + 1,
                                 LY.ix + LY.iw - LY.pad - vw
                                     - VERDICT_GAP * ui_glyph_w() - x, d, C_TEXT_DIM);
                }
                ui_text(LY.ix + LY.iw - LY.pad - vw, y + 1, passfail,
                        (ui_color_t)(ink & 15));
                y += REP_ROW_H;
            }
            if (!oneline) {
                while (wrap_next(&d, cols, DETAIL_INDENT, false, buf, sizeof buf)) {
                    if (i == start && row++ < skip) continue;
                    if (y + REP_ROW_H > ymax) break;
                    ui_text(LY.ix + LY.pad, y + 1, buf, C_TEXT_DIM);
                    y += REP_ROW_H;
                }
            }
            continue;
        }

        if (!*line) {                           /* a deliberate blank */
            if (i != start || row >= skip) y += REP_ROW_H;
            continue;
        }

        const char *s = line;
        int k = 0;
        while (wrap_next(&s, cols, DETAIL_INDENT, k++ == 0, buf, sizeof buf)) {
            if (i == start && row++ < skip) continue;
            if (y + REP_ROW_H > ymax) break;
            ui_text(LY.ix + LY.pad, y + 1, buf, (ui_color_t)(ink & 15));
            y += REP_ROW_H;
        }
    }
}

/* ── entry points ────────────────────────────────────────────────────── */

/*
 * Restore the window's corner curve.
 *
 * The header and footer are square fills that run the full width of the
 * window, so every band repaint squares off the two corners it touches.
 * The menu and the browser both re-pass the border afterwards for the
 * same reason; this one did not, which is why the report's corners were
 * the only square ones in the firmware.
 */
static void frame_fix(void) {
    ui_round_rect_border(LY.margin, LY.iy - 1, LY.w - 2 * LY.margin, LY.ih + 2, 4,
                         C_SEP, C_BG);
}

void ui_report_begin(void) {
    ui_compute_layout();
    if (!ui_layout_fits()) return;

    ui_draw_frame_once();
    /* No separator: the report is one column. Paint over the one the
     * shared chrome drew for the menu's two panes. */
    ui_vline(LY.sep_x, LY.body_y, LY.body_h, C_PANEL);

    ui_draw_header();
    draw_sub_header();
    draw_body();
    draw_footer();
    frame_fix();

    active = true;
    vga_set_mode(VGA_MODE_UI);
}

void ui_report_line_added(void) {
    if (!active) return;
    draw_body();
    draw_footer();
    frame_fix();
}
