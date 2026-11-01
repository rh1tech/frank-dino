/*
 * frank-dino — an RP2350B acting as the whole machine around a real Z80
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The menus' font: the Spectrum's own character set in a 7x8 cell. See
 * ui_font.c and tools/mkfont.py, which makes it from the 48K ROM.
 */
#pragma once

#include <stdint.h>

#define UI_FONT_W      7      /* cell width: the ROM's 8 less its unused right column */
#define UI_FONT_H      8      /* the ROM's own height, descenders included */
#define UI_FONT_FIRST 0x20
/* Past '~' the range is ours: 0x80..0x86 are the UI symbols (arrows, return,
 * chevrons) used by the footer and the breadcrumb. */
#define UI_FONT_LAST  0x86
#define UI_FONT_COUNT (UI_FONT_LAST - UI_FONT_FIRST + 1)

/* Row bitmaps, UI_FONT_H per glyph, bit 6 = leftmost pixel. */
extern const uint8_t ui_font[UI_FONT_COUNT * UI_FONT_H];

/* Rows of `ch`. Anything outside the table draws as a blank. */
static inline const uint8_t *ui_glyph(const char ch) {
    const uint8_t c = (uint8_t)ch;
    if (c >= UI_FONT_FIRST && c <= UI_FONT_LAST)
        return &ui_font[(c - UI_FONT_FIRST) * UI_FONT_H];
    return &ui_font[0];     /* 0x20, which is empty */
}
