/*
 * frank-dino — an RP2350B acting as the whole machine around a real Z80
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * pico-speccy's fullscreen menu layout and drawing, from its
 * src/ui/UiRender.cpp (GPL-3.0-or-later).
 *
 *  +-----------------------------------------------------+  rounded window, 1-px border
 *  | [rainbow] frank-dino                        v1.0.0  |  header
 *  |-----------------------------------------------------|
 *  | Settings > Storage            Machine: Pentagon 128 |  sub-header
 *  |--------------------+--------------------------------|
 *  | << Back            | Beta Disk (TR-DOS)             |  dim back row / pane title
 *  | Tape             > |--------------------------------|
 *  | Beta Disk       On | Off                          o |
 *  | Auto-start      On | On                           * |  selection = blue bar
 *  |--------------------+--------------------------------|
 *  | ^v Move   > Select   Esc / << Back                  |  footer
 *  +-----------------------------------------------------+
 *
 * Everything goes through ui_gfx: own font, own 16-colour palette, own
 * clipped blitter. No character grid.
 *
 * Three things are gone, all of them ends that lead nowhere on this
 * board: the staging overlay, so the header never turns red and the
 * footer counts nothing pending (settings.h applies edits live and says
 * why); the RTC clock, there being no RTC; and the marquee, which scrolls
 * over-long filenames and can come back with the browser.
 *
 * The footer's right-hand slot is not left empty though. Where theirs
 * counts unapplied edits, this one carries what the last action had to
 * say -- the frame rate a re-tune measured, what a PSRAM test found --
 * because on a machine whose CPU is a physical part that sentence is
 * often the only evidence anything happened at all.
 */

#include "ui_render.h"

#include <stdio.h>
#include <string.h>

#include "ui_nav.h"

ui_layout_t LY;

/* ── layout ──────────────────────────────────────────────────────────── */

void ui_compute_layout(void) {
    LY.w = UI_SURF_W;
    LY.h = UI_SURF_H;
    LY.pad = 2;                             /* inner text padding */

    /* The window: a margin of the screen's grey, as the 128K leaves the
     * border around its menu, a 1-px black frame, content inset one more. */
    LY.margin = 6;
    LY.ix = LY.margin + 1;
    LY.iy = LY.margin + 1;
    LY.iw = LY.w - 2 * LY.ix;
    LY.ih = LY.h - 2 * LY.iy;

    LY.row_h  = UI_FONT_H + 2;              /* 10 px */
    LY.hdr_h  = UI_FONT_H + 4;              /* the black title bar */
    LY.sub_h  = UI_FONT_H + 4;
    LY.foot_h = UI_FONT_H + 4;

    LY.body_y = LY.iy + LY.hdr_h + LY.sub_h;
    LY.body_h = LY.iy + LY.ih - LY.foot_h - LY.body_y;

    /*
     * Left pane: a fixed character budget so labels stay readable. Wide
     * enough for a long label AND its value column ("Kempston port  #1F"),
     * with what remains still holding the right pane's longest content.
     */
    const int lchars = 25;
    LY.lx = LY.ix;
    LY.lw = lchars * ui_glyph_w() + 2 * LY.pad;
    LY.sep_x = LY.lx + LY.lw;
    LY.rx = LY.sep_x + 1;
    LY.rw = LY.ix + LY.iw - LY.rx;

    /* Both panes spend their first row on a title ("<< Back" / the focused item). */
    LY.body_rows = LY.body_h / LY.row_h - 1;
    if (LY.body_rows < 1) LY.body_rows = 1;
}

bool ui_layout_fits(void) {
    return ui_cols() >= 40 && LY.body_rows >= 6;
}

/* ── dirty tracking ──────────────────────────────────────────────────── */

static uint8_t s_dirty;
static int8_t  s_lrow[2] = { -1, -1 };
static int8_t  s_rrow[2] = { -1, -1 };

void ui_mark_dirty(const uint8_t bits) { s_dirty |= bits; }

static void push_row(int8_t *slots, const int row, const uint8_t whole) {
    if (row < 0) return;
    for (int i = 0; i < 2; i++) {
        if (slots[i] == row) return;
        if (slots[i] < 0) { slots[i] = (int8_t)row; return; }
    }
    slots[0] = slots[1] = -1;            /* more than two queued: repaint the pane */
    s_dirty |= whole;
}

void ui_mark_left_row(const int vis_row)  { push_row(s_lrow, vis_row, D_LEFT); }
void ui_mark_right_row(const int vis_row) { push_row(s_rrow, vis_row, D_RIGHT); }

/* ── the status line ─────────────────────────────────────────────────── */

static char s_status[64];

void ui_status(const char *s) {
    snprintf(s_status, sizeof s_status, "%s", s ? s : "");
    ui_mark_dirty(D_FOOT);
}

/* ── static chrome ───────────────────────────────────────────────────── */

void ui_draw_frame_once(void) {
    ui_fill(0, 0, LY.w, LY.h, C_BG);
    ui_round_rect(LY.margin, LY.iy - 1, LY.w - 2 * LY.margin, LY.ih + 2, 4,
                  C_SEP, C_PANEL);
    ui_vline(LY.sep_x, LY.body_y, LY.body_h, C_SEP);
}

/* ── bands ───────────────────────────────────────────────────────────── */

void ui_draw_header(void) {
    ui_title_bar(LY.ix, LY.iy, LY.iw, LY.hdr_h, "frank-dino", "v" FIRMWARE_VERSION);
}

static void draw_sub_header(void) {
    const int y = LY.iy + LY.hdr_h;
    ui_fill(LY.ix, y, LY.iw, LY.sub_h, C_PANEL_ALT);

    /*
     * "Settings > Storage > ..." -- chevrons dim, deepest level bright, so
     * the eye lands on where it actually is.
     */
    int x = LY.ix + LY.pad;
    x += ui_text(x, y + 2, "Settings", ui_S.depth ? C_TEXT_DIM : C_WHITE);
    for (uint8_t d = 1; d <= ui_S.depth; d++) {
        const ui_node_t *p = ui_S.lv[d].parent;
        if (!p) break;
        x += ui_text(x, y + 2, " " SYM_CHEV " ", C_DISABLED);
        x += ui_text(x, y + 2, p->label, d == ui_S.depth ? C_WHITE : C_TEXT_DIM);
    }

    const char *mach = ui_machine_line();
    const int mw = ui_text_width(mach);
    if (x + mw + 2 * LY.pad < LY.ix + LY.iw)
        ui_text(LY.ix + LY.iw - mw - LY.pad, y + 2, mach, C_TEXT_DIM);
    ui_hline(LY.ix, y + LY.sub_h - 1, LY.iw, C_SEP);
}

/* ── rows ────────────────────────────────────────────────────────────── */

static inline int row_y(const int vis_row) {
    return LY.body_y + LY.row_h * (vis_row + 1);
}

static void draw_pane_titles(void) {
    const int y = LY.body_y;
    ui_fill(LY.lx, y, LY.lw, LY.row_h, C_PANEL);
    /*
     * Only where Left actually has a level to back out to -- at the root it
     * does nothing (closing is Esc/F12 only), so the hint would be a lie.
     */
    if (ui_S.depth > 0)
        ui_text(LY.lx + LY.pad, y + 1, SYM_LEFT SYM_LEFT " Back", C_DISABLED);

    ui_fill(LY.rx, y, LY.rw, LY.row_h, C_PANEL);
    const ui_node_t *dyn_node = ui_dyn_owner();
    const char *title = dyn_node ? dyn_node->label : ui_right_title(ui_cur_node());
    while (*title == ' ') title++;          /* indented rows: the indent is left-pane only */
    ui_text_clip(LY.rx + LY.pad, y + 1, LY.rw - 2 * LY.pad, title, C_WHITE);
    ui_hline(LY.rx, y + LY.row_h - 1, LY.rw, C_SEP);
}

static void draw_left_row(const int vis_row) {
    if (vis_row < 0 || vis_row >= LY.body_rows) return;
    ui_level_t *L = ui_cur_level();
    const int y = row_y(vis_row);
    const int idx = L->top + vis_row;

    const bool sel = (idx < L->nvis) && (idx == L->sel);
    ui_fill(LY.lx, y, LY.lw, LY.row_h,
            sel ? (ui_S.focus == FOCUS_LEFT ? C_SEL_BG : C_SEL_BAND) : C_PANEL);
    if (idx >= L->nvis) return;

    if (L->dyn) {
        /*
         * Pool row: label left, current contents right. A dimmed row is
         * informational and cannot be selected.
         */
        const uint8_t r = L->vis[idx];
        const bool dim = ui_S.dyn->dim[r];
        const ui_color_t ink = dim ? C_DISABLED : (sel ? C_WHITE : C_TEXT);
        const char *val = ui_S.dyn->value[r];
        /* The value column never gets more than half the pane, so an
         * over-long filename cannot push the label out of the row. */
        int rw = (val && *val) ? ui_text_width(val) : 0;
        const int maxv = (LY.lw - 2 * LY.pad) / 2;
        if (rw > maxv) rw = maxv;
        int availw = LY.lw - 2 * LY.pad - rw - (rw ? LY.pad : 0);
        if (availw < 0) availw = 0;
        ui_text_clip(LY.lx + LY.pad, y + 1, availw, ui_S.dyn->label[r], ink);
        if (rw)
            ui_text_clip(LY.lx + LY.lw - LY.pad - rw, y + 1, rw, val,
                         dim ? C_DISABLED : (sel ? C_WHITE : C_TEXT_DIM));
        return;
    }

    const ui_node_t *n = &L->nodes[L->vis[idx]];
    const bool en = ui_node_enabled(n);
    const ui_color_t ink = !en ? C_DISABLED : (sel ? C_WHITE : C_TEXT);

    const bool is_sub = (n->kind == K_SUB || n->kind == K_DYNAMIC);
    const char *val   = is_sub ? NULL : ui_node_value_label(n);
    const int right_w = is_sub ? ui_chevron_w() : ((val && *val) ? ui_text_width(val) : 0);

    ui_text_clip(LY.lx + LY.pad, y + 1,
                 LY.lw - 2 * LY.pad - right_w - (right_w ? LY.pad : 0), n->label, ink);
    if (is_sub)
        ui_chevron(LY.lx + LY.lw - LY.pad - ui_chevron_w(), y + LY.row_h / 2 - 3,
                   !en ? C_DISABLED : (sel ? C_WHITE : C_DISABLED));
    else if (val && *val)
        ui_text(LY.lx + LY.lw - LY.pad - right_w, y + 1, val,
                !en ? C_DISABLED : (sel ? C_WHITE : C_TEXT_DIM));
}

static void draw_right_row(const int vis_row) {
    if (vis_row < 0 || vis_row >= LY.body_rows) return;
    const int y = row_y(vis_row);
    const int idx = ui_S.rtop + vis_row;

    if (ui_cur_level()->dyn) {
        ui_fill(LY.rx, y, LY.rw, LY.row_h, C_PANEL);
        const char *h = ui_dyn_hint(idx);
        if (h) ui_text_clip(LY.rx + LY.pad, y + 1, LY.rw - 2 * LY.pad, h, C_TEXT_DIM);
        return;
    }

    const ui_node_t *n = ui_cur_node();

    const bool pick = n && (n->kind == K_RADIO || n->kind == K_BOOL || n->kind == K_INT);
    const bool sel  = pick && idx == ui_S.rsel && idx < ui_S.rcount;
    ui_fill(LY.rx, y, LY.rw, LY.row_h,
            sel ? (ui_S.focus == FOCUS_RIGHT ? C_SEL_BG : C_SEL_BAND) : C_PANEL);
    if (!n || idx >= ui_S.rcount) return;

    switch (n->kind) {
        case K_RADIO:
        case K_BOOL: {
            uint8_t cnt;
            const ui_option_t *os = ui_node_options(n, &cnt);
            if (idx >= cnt) break;
            const ui_option_t *o = &os[idx];
            const bool on = (ui_node_value(n) == o->value);
            /* Marker on the right: hollow ring, filled when chosen. */
            ui_radio(LY.rx + LY.rw - LY.pad - ui_radio_w(), y + (LY.row_h - 5) / 2, on,
                     sel ? C_WHITE : C_DISABLED, C_ACCENT);
            ui_text_clip(LY.rx + LY.pad, y + 1, LY.rw - 3 * LY.pad - ui_radio_w(),
                         o->label, sel ? C_WHITE : (on ? C_TEXT : C_TEXT_DIM));
            break;
        }
        case K_SUB: {
            const ui_node_t *c = ui_sub_visible_child(n, idx);
            if (c) ui_text_clip(LY.rx + LY.pad, y + 1, LY.rw - 2 * LY.pad,
                                c->label, C_TEXT_DIM);
            break;
        }
        case K_INT: {
            /* One slider row: [====----] 13/16. Vertical keys adjust while focused. */
            const int32_t v = ui_node_value(n);
            char buf[16];
            snprintf(buf, sizeof buf, "%ld/%ld", (long)(v - n->lo), (long)(n->hi - n->lo));
            const int lw = ui_text_width(buf);
            const int bx = LY.rx + LY.pad;
            const int bw = LY.rw - 3 * LY.pad - lw;
            const int bh = LY.row_h - 4;
            if (bw > 8 && n->hi > n->lo) {
                ui_frame(bx, y + 2, bw, bh, sel ? C_WHITE : C_SEP);
                const int fillw = (int)((int64_t)(bw - 2) * (v - n->lo) / (n->hi - n->lo));
                if (fillw > 0) ui_fill(bx + 1, y + 3, fillw, bh - 2, C_ACCENT);
            }
            ui_text(LY.rx + LY.rw - LY.pad - lw, y + 1, buf, sel ? C_WHITE : C_TEXT_DIM);
            break;
        }
        default:
            /* Only where Enter actually does something. A row that exists
             * to report live state has no fn(), and offering to open it
             * is the same lie the "<< Back" hint would be at the root. */
            if (idx == 0 && n->fn)
                ui_text_clip(LY.rx + LY.pad, y + 1, LY.rw - 2 * LY.pad,
                             SYM_ENTER " to open", C_TEXT_DIM);
            break;
    }
}

/* ── footer ──────────────────────────────────────────────────────────── */

static void draw_footer(void) {
    const int y = LY.iy + LY.ih - LY.foot_h;
    ui_fill(LY.ix, y, LY.iw, LY.foot_h, C_FOOT_BG);
    ui_hline(LY.ix, y, LY.iw, C_SEP);
    const ui_node_t *fn_ = ui_cur_level()->dyn ? NULL : ui_cur_node();
    const bool int_pane = (ui_S.focus == FOCUS_RIGHT) && fn_ && fn_->kind == K_INT;
    const bool at_home  = (ui_S.depth == 0);    /* no level below: Left is a no-op */
    const char *hint = (ui_S.focus == FOCUS_LEFT)
        ? (at_home ? SYM_UP SYM_DOWN " Move   " SYM_RIGHT " Select   Esc Close"
                   : SYM_UP SYM_DOWN " Move   " SYM_RIGHT " Select   Esc / " SYM_LEFT SYM_LEFT " Back")
        : int_pane
        ? SYM_UP SYM_DOWN " Adjust   " SYM_ENTER " / " SYM_LEFT " Back"
        : SYM_UP SYM_DOWN " Move   " SYM_ENTER " Change   " SYM_LEFT " Back";
    const int hx = ui_text(LY.ix + LY.pad, y + 3, hint, C_TEXT);

    /*
     * Where theirs counts staged edits, this says what the last action
     * found. Clipped to whatever the hint left, and dropped entirely when
     * that is nothing -- the hint is the part the user needs.
     */
    if (s_status[0]) {
        const int avail = LY.iw - LY.pad - (hx + 2 * LY.pad) - LY.pad;
        const int w = ui_text_width(s_status);
        if (avail >= 6 * ui_glyph_w())
            ui_text_clip(LY.ix + LY.iw - LY.pad - (w < avail ? w : avail), y + 3,
                         w < avail ? w : avail, s_status, C_ACCENT);
    }
}

/* ── flush ───────────────────────────────────────────────────────────── */

void ui_flush_dirty(void) {
    if (s_dirty & D_HEADER) ui_draw_header();
    if (s_dirty & D_SUB)    draw_sub_header();
    if (s_dirty & D_PTITLE) draw_pane_titles();

    if (s_dirty & D_LEFT)  for (int r = 0; r < LY.body_rows; r++) draw_left_row(r);
    else for (int i = 0; i < 2; i++) if (s_lrow[i] >= 0) draw_left_row(s_lrow[i]);

    if (s_dirty & D_RIGHT) for (int r = 0; r < LY.body_rows; r++) draw_right_row(r);
    else for (int i = 0; i < 2; i++) if (s_rrow[i] >= 0) draw_right_row(s_rrow[i]);

    if (s_dirty & D_FOOT) draw_footer();

    /* The header/footer fills are square rectangles -- restore the curve. */
    if (s_dirty & (D_HEADER | D_FOOT))
        ui_round_rect_border(LY.margin, LY.iy - 1, LY.w - 2 * LY.margin, LY.ih + 2, 4,
                             C_SEP, C_BG);

    s_dirty = 0;
    s_lrow[0] = s_lrow[1] = -1;
    s_rrow[0] = s_rrow[1] = -1;
}
