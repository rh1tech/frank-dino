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
 * The UI colours, as roles: palette indices into vga.c's kUiPalette,
 * which gives them the 128K menu's colours in this same order. The
 * surface is a byte a pixel and the video masks it to five bits, so
 * there is room for 32; roles that share a colour still keep their own
 * name, so a screen says what a thing is and the palette what it looks
 * like.
 */
typedef enum {
    C_BG = 0,       /* outside the rounded window */
    C_PANEL,        /* window and pane background */
    C_PANEL_ALT,    /* header / sub-header / pane-title bands */
    C_SEP,          /* rules and the window border */
    C_TEXT,         /* primary text */
    C_TEXT_DIM,     /* secondary text, footer hints */
    C_WHITE,        /* emphasis, text on the selection bar (black, here) */
    C_SEL_BG,       /* selection bar, focused pane */
    C_SEL_BAND,     /* subtle band for the focused row of the unfocused pane */
    C_ACCENT,       /* chosen radio dot */
    C_FOOT_BG,
    C_SHADOW,
    C_ICON_R,       /* ink: an error */
    C_ICON_Y,       /* ink: a warning */
    C_ICON_C,       /* ink: news */
    C_DISABLED,     /* "<< Back" and non-selectable rows */
    C_FLASH_R,      /* the 128K's flash, bright, in a title bar */
    C_FLASH_Y,
    C_FLASH_G,
    C_FLASH_C,
    C_TITLE_BG,     /* the black bar a window's title sits in */
    C_TITLE_TEXT,
    C_OK,           /* ink: a pass */
    C_COUNT
} ui_color_t;

/* Metrics in logical pixels. One fixed surface, so these are constants. */
#define UI_SURF_W VGA_UI_W
#define UI_SURF_H VGA_UI_H

static inline int ui_glyph_w(void) { return UI_FONT_W; }
static inline int ui_cols(void)    { return UI_SURF_W / UI_FONT_W; }
static inline int ui_rows(void)    { return UI_SURF_H / UI_FONT_H; }

/* Square unless set: the Spectrum's boxes are. */
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
/* The 128K menu's flash, a font-height tall, and at any height. */
void ui_rainbow(int x, int y);
int  ui_rainbow_w(void);
void ui_rainbow_h(int x, int y, int h);
int  ui_rainbow_w_h(int h);
/*
 * A window's title bar as the 128K draws it: black, the title in white
 * on the left, `right` (may be NULL) in white before the flash, and the
 * flash at the right-hand end.
 */
void ui_title_bar(int x, int y, int w, int h, const char *title, const char *right);
