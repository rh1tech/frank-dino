/*
 * frank-dino — an RP2350B acting as the whole machine around a real Z80
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Ported from pico-speccy src/ui/UiGfx.h and UiGfx.cpp (GPL-3.0-or-later).
 *
 * Their file carries two backends: a standard 8bpp indexed framebuffer
 * and the Profi DS80 packed-pair mode, where one byte holds two 4-bit
 * pixels and every write is a read-modify-write. This board has no DS80
 * and no packed mode, so only the first backend came across -- which
 * also fixes their `glyphScale` at 1 and lets the `^2` DMA byte swizzle
 * of their video driver go, this one having no such swizzle.
 *
 * Everything else is theirs: the colour roles, the clipping rules, the
 * glyph blitter, the widgets, and the corner table that makes the window
 * border curve.
 *
 * Coordinates are logical UI pixels, which here are framebuffer pixels:
 * 320x240, doubled onto the 640x480 wire by the video driver. Everything
 * clips.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "ui_font.h"
#include "vga.h"

/*
 * The 16 UI colours. Values are palette indices in vga.c's palette16,
 * which holds pico-speccy's kUiPaletteVga in this same order.
 *
 * Their budget note applies here unchanged: exactly 16, with duplicates
 * merged (one white serves text, header and selection) to free four
 * slots for the rainbow mark in the header.
 */
typedef enum {
    C_BG = 0,       /* outside the rounded window */
    C_PANEL,        /* window and pane background */
    C_PANEL_ALT,    /* header / sub-header / pane-title bands */
    C_SEP,          /* rules and the window border */
    C_TEXT,         /* primary text */
    C_TEXT_DIM,     /* secondary text, footer hints */
    C_WHITE,        /* emphasis, text on the selection bar */
    C_SEL_BG,       /* selection bar, focused pane */
    C_SEL_BAND,     /* subtle band for the focused row of the unfocused pane */
    C_ACCENT,       /* chosen radio dot */
    C_FOOT_BG,
    C_SHADOW,
    C_ICON_R,       /* rainbow mark */
    C_ICON_Y,
    C_ICON_C,
    C_DISABLED,     /* "<< Back" and non-selectable rows */
    C_COUNT
} ui_color_t;

/* Metrics in logical pixels. One fixed surface, so these are constants. */
#define UI_SURF_W VGA_UI_W
#define UI_SURF_H VGA_UI_H

static inline int ui_glyph_w(void) { return UI_FONT_W; }
static inline int ui_cols(void)    { return UI_SURF_W / UI_FONT_W; }
static inline int ui_rows(void)    { return UI_SURF_H / UI_FONT_H; }

/* Options > Menu corners, in their UI. Square when false. */
extern bool ui_rounded;

void ui_px(int x, int y, ui_color_t c);
void ui_hline(int x, int y, int w, ui_color_t c);
void ui_vline(int x, int y, int h, ui_color_t c);
void ui_fill(int x, int y, int w, int h, ui_color_t c);
void ui_frame(int x, int y, int w, int h, ui_color_t c);

/* Transparent glyph run. Returns the advance in logical pixels. */
int ui_text(int x, int y, const char *s, ui_color_t ink);
/* As above but stops before exceeding `maxw`, appending ".." when it did not fit. */
int ui_text_clip(int x, int y, int maxw, const char *s, ui_color_t ink);
int ui_text_width(const char *s);

/* Widgets. */
void ui_radio(int x, int y, bool on, ui_color_t ink, ui_color_t accent);
int  ui_radio_w(void);
void ui_chevron(int x, int y, ui_color_t c);    /* 4x7 "descend" arrow */
int  ui_chevron_w(void);
/* Rounded rectangle: 1-px border with the corners cut back by `r` pixels. */
void ui_round_rect(int x, int y, int w, int h, int r, ui_color_t border, ui_color_t fill_c);
/*
 * Re-draws ONLY the border and the outside-corner pixels of a round rect.
 * The header/footer bands are plain rectangles, so every band repaint
 * squares the window corners off -- call this after them to restore the
 * rounding.
 */
void ui_round_rect_border(int x, int y, int w, int h, int r,
                          ui_color_t border, ui_color_t outside);
/* The ZX rainbow mark of the header. */
void ui_rainbow(int x, int y);
int  ui_rainbow_w(void);
