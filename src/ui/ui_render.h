/*
 * frank-dino — an RP2350B acting as the whole machine around a real Z80
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Ported from pico-speccy src/ui/UiRender.h (GPL-3.0-or-later).
 *
 * Layout is computed in LOGICAL UI PIXELS, not in character cells, so
 * panes, rules, insets and the selection bar are positioned freely. On
 * this board the surface is always 320x240 with a 6-pixel glyph, which
 * is the first of the four their file lists.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "ui_gfx.h"
#include "ui_model.h"

/*
 * Private font glyphs for the arrows / Enter / chevron (the UI font
 * carries them at 0x80..0x86). Shared so the nav can label hints too.
 */
#define SYM_UP    "\x80"
#define SYM_DOWN  "\x81"
#define SYM_RIGHT "\x82"
#define SYM_LEFT  "\x83"
#define SYM_ENTER "\x84"
#define SYM_CHEV  "\x85"

typedef struct {
    int w, h;               /* logical surface extent */
    int margin;             /* gap around the rounded window */
    int ix, iy, iw, ih;     /* window content rect (inside the border) */
    int hdr_h, sub_h, foot_h;
    int body_y, body_h;     /* vertical extent of the two panes */
    int lx, lw;             /* left pane */
    int sep_x;              /* separator rule */
    int rx, rw;             /* right pane */
    int row_h;              /* one list row */
    int body_rows;          /* rows that fit in a pane */
    int pad;                /* inner padding */
} ui_layout_t;

extern ui_layout_t LY;

void ui_compute_layout(void);
bool ui_layout_fits(void);

/*
 * Dirty regions. Drawing lands straight in the framebuffer the video DMA
 * is scanning out -- no back buffer, no vsync hook -- so a cursor move
 * must never repaint everything.
 */
enum {
    D_HEADER = 0x01, D_SUB = 0x02, D_PTITLE = 0x04,
    D_LEFT   = 0x08, D_RIGHT = 0x10, D_FOOT = 0x20,
    D_ALL    = 0x3F
};
void ui_mark_dirty(uint8_t bits);
void ui_mark_left_row(int vis_row);
void ui_mark_right_row(int vis_row);
void ui_flush_dirty(void);

void ui_draw_frame_once(void);  /* static chrome: background, window, separator */

/*
 * The header band: rainbow mark, name, version.
 *
 * Shared with the boot report, which is the same window with a different
 * body -- so the one thing that identifies the firmware is drawn by one
 * function and cannot come out differently in the two places.
 */
void ui_draw_header(void);

/* The footer's status line: what the last action had to say for itself. */
void ui_status(const char *s);
