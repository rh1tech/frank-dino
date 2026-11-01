/*
 * frank-dino — an RP2350B acting as the whole machine around a real Z80
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * pico-speccy's OSD rasteriser, from its src/ui/UiGfx.cpp, with the
 * Profi DS80 packed-pair backend removed and the C++ taken off. See
 * ui_gfx.h for what that cost and why nothing else changed.
 */

#include "ui_gfx.h"

#include <string.h>

bool ui_rounded = false;     /* the Spectrum's boxes are square */

/* ── pixels ──────────────────────────────────────────────────────────── */

static inline uint8_t *fb_row(const int y) { return &vga_ui_fb[y * VGA_UI_W]; }

void ui_px(const int x, const int y, const ui_color_t c) {
    if (x < 0 || y < 0 || x >= UI_SURF_W || y >= UI_SURF_H) return;
    fb_row(y)[x] = (uint8_t)c;
}

void ui_hline(int x, const int y, int w, const ui_color_t c) {
    if (y < 0 || y >= UI_SURF_H || w <= 0) return;
    if (x < 0) { w += x; x = 0; }
    if (x + w > UI_SURF_W) w = UI_SURF_W - x;
    if (w <= 0) return;
    memset(&fb_row(y)[x], (uint8_t)c, (size_t)w);
}

void ui_vline(const int x, int y, int h, const ui_color_t c) {
    if (y < 0) { h += y; y = 0; }
    if (y + h > UI_SURF_H) h = UI_SURF_H - y;
    for (int i = 0; i < h; i++) ui_px(x, y + i, c);
}

void ui_fill(const int x, int y, const int w, int h, const ui_color_t c) {
    if (y < 0) { h += y; y = 0; }
    if (y + h > UI_SURF_H) h = UI_SURF_H - y;
    for (int i = 0; i < h; i++) ui_hline(x, y + i, w, c);
}

void ui_frame(const int x, const int y, const int w, const int h, const ui_color_t c) {
    if (w <= 0 || h <= 0) return;
    ui_hline(x, y, w, c);
    ui_hline(x, y + h - 1, w, c);
    ui_vline(x, y, h, c);
    ui_vline(x + w - 1, y, h, c);
}

/* ── text ────────────────────────────────────────────────────────────── */

static void glyph(const int x, const int y, const char ch, const ui_color_t ink) {
    const uint8_t *g = ui_glyph(ch);
    for (int r = 0; r < UI_FONT_H; r++) {
        const uint8_t bits = g[r];
        if (!bits) continue;
        for (int b = 0; b < UI_FONT_W; b++) {
            if (!(bits & (0x40 >> b))) continue;
            ui_px(x + b, y + r, ink);
        }
    }
}

int ui_text_width(const char *s) {
    if (!s) return 0;
    return (int)strlen(s) * ui_glyph_w();
}

int ui_text(const int x, const int y, const char *s, const ui_color_t ink) {
    if (!s) return 0;
    const int adv = ui_glyph_w();
    int cx = x;
    for (; *s; ++s) {
        if (cx >= UI_SURF_W) break;
        if (*s != ' ') glyph(cx, y, *s, ink);   /* space costs nothing to draw */
        cx += adv;
    }
    return cx - x;
}

int ui_text_clip(const int x, const int y, const int maxw, const char *s,
                 const ui_color_t ink) {
    if (!s || maxw <= 0) return 0;
    const int adv  = ui_glyph_w();
    const int fits = maxw / adv;
    const int len  = (int)strlen(s);
    if (len <= fits) return ui_text(x, y, s, ink);
    /* Truncate and mark it: two dots cost one cell each, so keep fits-2. */
    int keep = fits - 2;
    if (keep < 0) keep = 0;
    int cx = x;
    for (int i = 0; i < keep; i++, cx += adv)
        if (s[i] != ' ') glyph(cx, y, s[i], ink);
    for (int i = 0; i < 2 && cx < x + maxw; i++, cx += adv) glyph(cx, y, '.', ink);
    return cx - x;
}

/* ── widgets ─────────────────────────────────────────────────────────── */

void ui_radio(const int x, const int y, const bool on,
              const ui_color_t ink, const ui_color_t accent) {
    const int w = 5, h = 5;
    ui_hline(x + 1, y,         w - 2, ink);
    ui_hline(x + 1, y + h - 1, w - 2, ink);
    ui_fill (x,         y + 1, 1, h - 2, ink);
    ui_fill (x + w - 1, y + 1, 1, h - 2, ink);
    if (on) ui_fill(x + 1, y + 1, w - 2, h - 2, accent);
}

int ui_radio_w(void) { return 5; }

void ui_chevron(const int x, const int y, const ui_color_t c) {
    for (int i = 0; i < 4; i++) {
        ui_fill(x + i, y + i,     1, 1, c);
        ui_fill(x + i, y + 6 - i, 1, 1, c);
    }
}

int ui_chevron_w(void) { return 4; }

/*
 * Corner cut-back table for r<=4: how many pixels to inset on each of
 * the first r rows. Quarter-circle-ish -- the outermost row cuts deepest
 * so the curve actually reads at this size.
 */
static const uint8_t kCorner[5] = { 0, 1, 1, 2, 4 };

static inline int corner_cut(const int i, const int h, const int r) {
    if (i < r)      return kCorner[r - i];
    if (i >= h - r) return kCorner[r - (h - 1 - i)];
    return 0;
}

void ui_round_rect(const int x, const int y, const int w, const int h, int r,
                   const ui_color_t border, const ui_color_t fill_c) {
    if (w <= 0 || h <= 0) return;
    if (!ui_rounded) r = 0;
    if (r > 4) r = 4;
    for (int i = 0; i < h; i++) {
        const int cut = corner_cut(i, h, r);
        ui_hline(x + cut, y + i, w - 2 * cut, fill_c);
    }
    /*
     * Border. In the corner region each row's segment stretches to the
     * deeper cut of its neighbouring row, so the steps CONNECT into a
     * curve instead of leaving single pixels with gaps between them.
     */
    for (int i = 0; i < h; i++) {
        const int cut = corner_cut(i, h, r);
        if (i == 0 || i == h - 1) {
            ui_hline(x + cut, y + i, w - 2 * cut, border);
            continue;
        }
        const int above = corner_cut(i - 1, h, r);
        const int below = corner_cut(i + 1, h, r);
        const int adj   = above > below ? above : below;
        const int wseg  = (adj > cut) ? (adj - cut + 1) : 1;
        ui_fill(x + cut, y + i, wseg, 1, border);
        ui_fill(x + w - cut - wseg, y + i, wseg, 1, border);
    }
}

void ui_round_rect_border(const int x, const int y, const int w, const int h, int r,
                          const ui_color_t border, const ui_color_t outside) {
    if (w <= 0 || h <= 0) return;
    if (!ui_rounded) r = 0;
    if (r > 4) r = 4;
    for (int i = 0; i < h; i++) {
        const int cut = corner_cut(i, h, r);
        if (cut) {                              /* clear the outside of the curve */
            ui_fill(x, y + i, cut, 1, outside);
            ui_fill(x + w - cut, y + i, cut, 1, outside);
        }
        if (i == 0 || i == h - 1) {
            ui_hline(x + cut, y + i, w - 2 * cut, border);
            continue;
        }
        const int above = corner_cut(i - 1, h, r);
        const int below = corner_cut(i + 1, h, r);
        const int adj   = above > below ? above : below;
        const int wseg  = (adj > cut) ? (adj - cut + 1) : 1;
        ui_fill(x + cut, y + i, wseg, 1, border);
        ui_fill(x + w - cut - wseg, y + i, wseg, 1, border);
    }
}

/*
 * The 128K menu's flash: red, yellow, green and cyan, eight pixels each,
 * slanting up to the right over the height of the bar -- as wide as the
 * four stripes plus the slant.
 */
#define FLASH_STRIPE 8

void ui_rainbow_h(const int x, const int y, const int h) {
    static const ui_color_t c[4] = { C_FLASH_R, C_FLASH_Y, C_FLASH_G, C_FLASH_C };
    for (int r = 0; r < h; r++)
        for (int k = 0; k < 4; k++)
            ui_hline(x + (h - 1 - r) + k * FLASH_STRIPE, y + r, FLASH_STRIPE, c[k]);
}

int ui_rainbow_w_h(const int h) { return 4 * FLASH_STRIPE + h - 1; }

void ui_rainbow(const int x, const int y) { ui_rainbow_h(x, y, UI_FONT_H); }
int  ui_rainbow_w(void) { return ui_rainbow_w_h(UI_FONT_H); }

void ui_title_bar(const int x, const int y, const int w, const int h,
                  const char *title, const char *right) {
    ui_fill(x, y, w, h, C_TITLE_BG);
    const int ty = y + (h - UI_FONT_H) / 2;
    const int fw = ui_rainbow_w_h(h);
    const int fx = x + w - fw - FLASH_STRIPE;          /* a stripe's width of black after it */
    ui_rainbow_h(fx, y, h);
    int limit = fx - FLASH_STRIPE;
    if (right && *right) {
        const int rw = ui_text_width(right);
        ui_text(limit - rw, ty, right, C_TITLE_TEXT);
        limit -= rw + ui_glyph_w();
    }
    if (title) ui_text_clip(x + 3, ty, limit - (x + 3), title, C_TITLE_TEXT);
}
