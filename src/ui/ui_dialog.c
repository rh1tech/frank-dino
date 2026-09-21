/*
 * frank-dino — an RP2350B acting as the whole machine around a real Z80
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-dino
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * pico-speccy's modal primitives, from its src/ui/UiDialog.cpp: the box
 * geometry, the drop shadow, the button row and the key handling. Their
 * palette suspend/resume is gone with the DS80 mode it existed for, and
 * fabgl's VirtualKey table is this board's byte stream instead.
 */

#include "ui_dialog.h"

#include <stdio.h>
#include <string.h>

#include <pico/stdlib.h>

#include "menu.h"
#include "ui_gfx.h"
#include "ui_render.h"

typedef struct { int x, y, w, h; } box_t;

static int line_count(const char *s) {
    int n = 1;
    for (; *s; s++) if (*s == '\n') n++;
    return n;
}

static int max_line_width(const char *s) {
    int best = 0, cur = 0;
    for (; *s; s++) {
        if (*s == '\n') { if (cur > best) best = cur; cur = 0; }
        else cur++;
    }
    if (cur > best) best = cur;
    return best * ui_glyph_w();
}

/*
 * Draw the box chrome and the body text; returns the box so callers can
 * add buttons under it. `extra_h` reserves room below the text.
 */
static box_t draw_box(const char *text_body, const char *title, const int extra_h,
                      const ui_color_t border, const int min_w) {
    const int pad = 4;
    const int lh  = UI_FONT_H + 2;
    const int tl  = title ? lh + 2 : 0;

    int tw = max_line_width(text_body);
    if (title) { const int t2 = ui_text_width(title); if (t2 > tw) tw = t2; }

    box_t b;
    b.w = tw + 2 * pad;
    if (b.w < min_w) b.w = min_w;
    const int wmax = UI_SURF_W - 8;
    if (b.w > wmax) b.w = wmax;
    b.h = tl + line_count(text_body) * lh + extra_h + 2 * pad;
    if (b.h > UI_SURF_H - 8) b.h = UI_SURF_H - 8;
    b.x = (UI_SURF_W - b.w) / 2;
    b.y = (UI_SURF_H - b.h) / 2;

    /* Drop shadow, then the window: it is what separates a modal from
     * the page it is standing on. */
    ui_fill(b.x + 2, b.y + 2, b.w, b.h, C_SHADOW);
    ui_round_rect(b.x, b.y, b.w, b.h, 3, border, C_PANEL_ALT);

    int y = b.y + pad;
    if (title) {
        ui_text(b.x + pad, y, title, C_WHITE);
        ui_hline(b.x + pad, y + lh, b.w - 2 * pad, C_SEP);
        y += tl + 2;
    }

    const char *p = text_body;
    char line[64];
    while (*p) {
        size_t i = 0;
        while (*p && *p != '\n' && i < sizeof line - 1) line[i++] = *p++;
        line[i] = 0;
        if (*p == '\n') p++;
        ui_text_clip(b.x + pad, y, b.w - 2 * pad, line, C_TEXT);
        y += lh;
        if (y > b.y + b.h - pad) break;
    }
    return b;
}

static void draw_yes_no(const box_t *b, const bool yes) {
    const int pad = 4;
    const int lh  = UI_FONT_H + 2;
    const int by  = b->y + b->h - pad - lh;
    const int bw  = 5 * ui_glyph_w();          /* " Yes " / " No  " */
    const int gap = 2 * ui_glyph_w();
    int bx = b->x + (b->w - 2 * bw - gap) / 2;

    ui_fill(bx, by - 1, bw, lh, yes ? C_SEL_BG : C_PANEL_ALT);
    ui_text(bx + (bw - 3 * ui_glyph_w()) / 2, by, "Yes", yes ? C_WHITE : C_TEXT_DIM);
    bx += bw + gap;
    ui_fill(bx, by - 1, bw, lh, yes ? C_PANEL_ALT : C_SEL_BG);
    ui_text(bx + (bw - 2 * ui_glyph_w()) / 2, by, "No", yes ? C_TEXT_DIM : C_WHITE);
}

/* ── keys ────────────────────────────────────────────────────────────── */

typedef enum { DK_NONE = 0, DK_PREV, DK_NEXT, DK_ENTER, DK_ESC, DK_CHAR } dkey_t;

#define ESC_CONT_MS 20

static dkey_t dlg_key(char *ch, const int wait_ms) {
    const int c = (wait_ms < 0) ? menu_wait_key() : menu_wait_key_timeout((unsigned)wait_ms);
    if (c < 0) return DK_NONE;

    if (c == 0x1B) {
        const int b = menu_wait_key_timeout(ESC_CONT_MS);
        if (b != '[' && b != 'O') return DK_ESC;
        switch (menu_wait_key_timeout(ESC_CONT_MS)) {
            case 'A': case 'D': return DK_PREV;
            case 'B': case 'C': return DK_NEXT;
            default:  return DK_NONE;
        }
    }
    switch (c) {
        case '\r': case '\n': return DK_ENTER;
        case '\t':            return DK_NEXT;
        default:
            if (c >= ' ' && c < 0x7F) { *ch = (char)c; return DK_CHAR; }
            return DK_NONE;
    }
}

/* ── the dialogs ─────────────────────────────────────────────────────── */

bool ui_confirm(const char *text, const char *title, const bool default_yes) {
    const int lh = UI_FONT_H + 2;
    box_t b = draw_box(text, title, lh + 6, C_SEP, 0);

    /*
     * Enter must not destroy, so the default is No -- except where the
     * destruction is what the user just asked for rather than what the
     * question is about, and the caller says so.
     */
    bool yes = default_yes;
    draw_yes_no(&b, yes);

    for (;;) {
        char ch = 0;
        switch (dlg_key(&ch, -1)) {
            case DK_PREV:
            case DK_NEXT:  yes = !yes; draw_yes_no(&b, yes); break;
            case DK_ENTER: return yes;
            case DK_ESC:   return false;
            case DK_CHAR:
                if (ch == 'y' || ch == 'Y') return true;
                if (ch == 'n' || ch == 'N') return false;
                break;
            default: break;
        }
    }
}

int ui_choice(const char *text, const char *const *btns, const int n,
              const int initial, const int esc_result) {
    const int lh = UI_FONT_H + 2;

    /* The box must fit the button row too. */
    int bw_total = 0;
    for (int i = 0; i < n; i++) bw_total += ((int)strlen(btns[i]) + 2) * ui_glyph_w();
    bw_total += (n - 1) * 2 * ui_glyph_w();

    box_t b = draw_box(text, NULL, lh + 6, C_SEP, bw_total + 8);

    int sel = (initial >= 0 && initial < n) ? initial : 0;
    const int by = b.y + b.h - 4 - lh;

    for (;;) {
        int x = b.x + (b.w - bw_total) / 2;
        for (int i = 0; i < n; i++) {
            const int w = ((int)strlen(btns[i]) + 2) * ui_glyph_w();
            ui_fill(x, by - 1, w, lh, i == sel ? C_SEL_BG : C_PANEL_ALT);
            ui_text(x + ui_glyph_w(), by, btns[i], i == sel ? C_WHITE : C_TEXT_DIM);
            x += w + 2 * ui_glyph_w();
        }

        char ch = 0;
        switch (dlg_key(&ch, -1)) {
            case DK_PREV:  sel = (sel + n - 1) % n; break;
            case DK_NEXT:  sel = (sel + 1) % n; break;
            case DK_ENTER: return sel;
            case DK_ESC:   return esc_result;
            default: break;
        }
    }
}

void ui_toast(const char *msg, const bool warn, const int ms) {
    draw_box(msg, NULL, 0, warn ? C_ICON_R : C_ACCENT, 0);

    if (ms <= 0) { char ch; while (dlg_key(&ch, -1) == DK_NONE) { } return; }

    const absolute_time_t deadline = make_timeout_time_ms((uint32_t)ms);
    for (;;) {
        char ch = 0;
        if (dlg_key(&ch, 20) != DK_NONE) return;
        if (absolute_time_diff_us(get_absolute_time(), deadline) <= 0) return;
    }
}

/* ── the text page ───────────────────────────────────────────────────── */

/*
 * Their inline markup: a 0x02 byte and a letter switch ink for the rest
 * of the line. It exists so a page's content can be one string literal
 * instead of a list of {colour, text} pairs, which is what makes an
 * info page cheap enough to write.
 */
static ui_color_t ink_for(const char c) {
    switch (c) {
        case 'd': return C_TEXT_DIM;
        case 'w': return C_WHITE;
        case 'g': return C_ACCENT;
        case 'r': return C_ICON_R;
        case 'h': return C_ICON_Y;
        default:  return C_TEXT;
    }
}

static void markup_line(const int x, const int y, const int maxw,
                        const char *s, const int len) {
    char seg[96];
    ui_color_t ink = C_TEXT;
    int cx = x, si = 0;
    const int maxx = x + maxw;
    const char *p = s, *e = s + len;

    while (p < e && cx < maxx) {
        if (*p == 0x02 && p + 1 < e) {
            if (si) { seg[si] = 0; cx += ui_text_clip(cx, y, maxx - cx, seg, ink); si = 0; }
            ink = ink_for(p[1]);
            p += 2;
            continue;
        }
        if (si < (int)sizeof seg - 1) seg[si++] = *p;
        p++;
    }
    if (si) { seg[si] = 0; ui_text_clip(cx, y, maxx - cx, seg, ink); }
}

typedef enum { PG_NONE = 0, PG_UP, PG_DOWN, PG_PGUP, PG_PGDN,
               PG_HOME, PG_END, PG_CLOSE } pkey_t;

static pkey_t page_key(char *ch, const int rows) {
    (void)rows;
    const int c = menu_wait_key();
    if (c < 0) return PG_NONE;

    if (c == 0x1B) {
        const int b = menu_wait_key_timeout(ESC_CONT_MS);
        if (b != '[' && b != 'O') return PG_CLOSE;      /* a bare Esc */
        switch (menu_wait_key_timeout(ESC_CONT_MS)) {
            case 'A': return PG_UP;
            case 'B': return PG_DOWN;
            case 'D': return PG_CLOSE;                  /* Left leaves, as theirs does */
            case 'H': return PG_HOME;
            case 'F': return PG_END;
            case '5': menu_wait_key_timeout(ESC_CONT_MS); return PG_PGUP;
            case '6': menu_wait_key_timeout(ESC_CONT_MS); return PG_PGDN;
            default:  return PG_NONE;
        }
    }
    switch (c) {
        case '\r': case '\n': case '\t': case '\\': return PG_CLOSE;
        default:
            if (c >= ' ' && c < 0x7F) { *ch = (char)c; return PG_NONE; }
            return PG_NONE;
    }
}

#define PAGE_MAX_LINES 192

void ui_text_page(const char *title, const char *body, const char *hdr_right) {
    const int lh  = UI_FONT_H + 2;
    /* The same inset the menu and the browser use, so the title and the
     * footer hints line up across every screen. */
    const int pad = 2;
    const int margin = 2;
    const int ix = margin + 1, iy = 3;
    const int iw = UI_SURF_W - 2 * ix, ih = UI_SURF_H - iy - 3;
    const int hdr_h = UI_FONT_H + 6, foot_h = UI_FONT_H + 4;
    const int body_y = iy + hdr_h + 2;
    const int body_h = iy + ih - foot_h - body_y;
    const int rows = body_h / lh;

    const char *ls[PAGE_MAX_LINES];
    uint16_t    ll[PAGE_MAX_LINES];
    int nlines = 0;
    for (const char *p = body; *p && nlines < PAGE_MAX_LINES; ) {
        ls[nlines] = p;
        const char *e = p;
        while (*e && *e != '\n') e++;
        ll[nlines++] = (uint16_t)(e - p);
        p = *e ? e + 1 : e;
    }

    int top = 0;
    const int max_top = nlines > rows ? nlines - rows : 0;

    /* Static chrome. */
    ui_fill(0, 0, UI_SURF_W, UI_SURF_H, C_BG);
    ui_round_rect(margin, iy - 1, UI_SURF_W - 2 * margin, ih + 2, 4, C_SEP, C_PANEL);
    ui_fill(ix, iy, iw, hdr_h, C_PANEL);
    ui_rainbow(ix + pad, iy + 3);
    int title_w = iw - 4 * pad - ui_rainbow_w();
    if (hdr_right && *hdr_right) {
        const int rw = ui_text_width(hdr_right);
        ui_text(ix + iw - pad - rw, iy + 4, hdr_right, C_TEXT_DIM);
        title_w -= rw + 2 * pad;
    }
    ui_text_clip(ix + pad + ui_rainbow_w() + 2 * pad, iy + 4, title_w, title, C_WHITE);
    ui_hline(ix, iy + hdr_h - 1, iw, C_SEP);

    const int fy = iy + ih - foot_h;
    ui_fill(ix, fy, iw, foot_h, C_FOOT_BG);
    ui_hline(ix, fy, iw, C_SEP);
    ui_text(ix + pad, fy + 3, SYM_UP SYM_DOWN " Scroll   Esc Close", C_TEXT_DIM);
    /* Bands are square fills -- restore the window's corner curve over them. */
    ui_round_rect_border(margin, iy - 1, UI_SURF_W - 2 * margin, ih + 2, 4, C_SEP, C_BG);

    for (;;) {
        for (int r = 0; r < rows; r++) {
            const int y = body_y + r * lh;
            ui_fill(ix, y, iw, lh, C_PANEL);
            const int li = top + r;
            if (li >= nlines) continue;
            markup_line(ix + pad, y + 1, iw - 2 * pad, ls[li], ll[li]);
        }
        if (max_top) {
            char pos[20];
            snprintf(pos, sizeof pos, "%d-%d/%d", top + 1,
                     top + rows < nlines ? top + rows : nlines, nlines);
            const int pw = ui_text_width(pos);
            ui_fill(ix + iw - pw - 2 * pad, fy + 1, pw + 2 * pad, foot_h - 2, C_FOOT_BG);
            ui_text(ix + iw - pw - pad, fy + 3, pos, C_TEXT_DIM);
        }

        int nt = top;
        char ch = 0;
        switch (page_key(&ch, rows)) {
            case PG_UP:    nt = top - 1; break;
            case PG_DOWN:  nt = top + 1; break;
            case PG_PGUP:  nt = top - rows; break;
            case PG_PGDN:  nt = top + rows; break;
            case PG_HOME:  nt = 0; break;
            case PG_END:   nt = max_top; break;
            case PG_CLOSE: return;
            default: break;
        }
        if (nt < 0) nt = 0;
        if (nt > max_top) nt = max_top;
        top = nt;
    }
}

/* ── the pick list ───────────────────────────────────────────────────── */

int ui_pick_list(const char *title, const int n, const ui_row_cb cb,
                 const int initial, const int wchars, const char *footer) {
    if (n <= 0) return -1;
    const int pad = 4;
    const int lh  = UI_FONT_H + 2;
    if (footer && !*footer) footer = NULL;

    int tw = wchars * ui_glyph_w();
    { const int t2 = ui_text_width(title); if (t2 > tw) tw = t2; }
    if (footer) { const int t3 = ui_text_width(footer); if (t3 > tw) tw = t3; }

    int rows = n;
    const int max_rows = (UI_SURF_H - (footer ? 7 : 6) * lh) / lh;
    if (rows > max_rows) rows = max_rows;
    if (rows < 1) rows = 1;

    box_t b;
    b.w = tw + 3 * pad;
    const int wmax = UI_SURF_W - 8;
    if (b.w > wmax) b.w = wmax;
    b.h = (lh + 2) + rows * lh + 2 * pad + (footer ? lh + 1 : 0);
    b.x = (UI_SURF_W - b.w) / 2;
    b.y = (UI_SURF_H - b.h) / 2;

    int sel = (initial >= 0 && initial < n) ? initial : 0;
    int top = 0;
    if (sel >= rows) top = sel - rows + 1;

    char row[96];
    for (;;) {
        ui_fill(b.x + 2, b.y + 2, b.w, b.h, C_SHADOW);
        ui_round_rect(b.x, b.y, b.w, b.h, 3, C_SEP, C_PANEL_ALT);
        ui_text(b.x + pad, b.y + pad - 1, title, C_WHITE);
        if (n > rows) {
            char pos[28];
            snprintf(pos, sizeof pos, "%d/%d", sel + 1, n);
            ui_text(b.x + b.w - pad - ui_text_width(pos), b.y + pad - 1, pos, C_TEXT_DIM);
        }
        ui_hline(b.x + pad, b.y + pad + lh - 2, b.w - 2 * pad, C_SEP);

        const int ly0 = b.y + pad + lh + 1;
        for (int r = 0; r < rows; r++) {
            const int i = top + r;
            const int y = ly0 + r * lh;
            const bool s = (i == sel);
            ui_fill(b.x + 2, y - 1, b.w - 4, lh, s ? C_SEL_BG : C_PANEL_ALT);
            if (i < n) {
                row[0] = 0;
                cb(i, row, sizeof row);
                ui_text_clip(b.x + pad, y, b.w - 2 * pad, row, s ? C_WHITE : C_TEXT);
            }
        }
        /* The verb line: the list's keys exist nowhere else on screen, so
         * it is drawn like the menu's own footer -- a rule, then dim ink. */
        if (footer) {
            const int fy = ly0 + rows * lh + 1;
            ui_hline(b.x + pad, fy, b.w - 2 * pad, C_SEP);
            ui_text_clip(b.x + pad, fy + 2, b.w - 2 * pad, footer, C_TEXT_DIM);
        }

        int ns = sel;
        char ch = 0;
        switch (dlg_key(&ch, -1)) {
            case DK_PREV:  ns = sel - 1; break;
            case DK_NEXT:  ns = sel + 1; break;
            case DK_ENTER: return sel;
            case DK_ESC:   return -1;
            default: break;
        }
        if (ns < 0) ns = 0;
        if (ns > n - 1) ns = n - 1;
        sel = ns;
        if (sel < top) top = sel;
        if (sel >= top + rows) top = sel - rows + 1;
    }
}
