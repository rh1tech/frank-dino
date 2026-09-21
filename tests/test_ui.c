/*
 * The UI rasteriser, drawn into the real 320x240 surface.
 *
 * The frames around panels are rounded, and they stopped being rounded
 * once already -- a band fill painted over the corners after the border
 * had been drawn, and nothing but a person looking at the screen
 * noticed. So the corners are asserted here, explicitly, on every
 * rounded frame this draws.
 *
 * The test also writes out what it drew, so a change of appearance can
 * be looked at rather than argued about: tests/out/ as PPM.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>

#include "ui_gfx.h"
#include "ui_font.h"
#include "vga.h"
#include "settings.h"
#include "tap.h"

/* ---- writing out what was drawn ------------------------------------ */
static const uint32_t PAL[16] = {
    0x000000, 0x101018, 0x1C1C28, 0x2A2A3A, 0x3A3A50, 0x505070, 0x7070A0, 0x9090C0,
    0xFFFFFF, 0xC8C8D8, 0x9090A0, 0x606070, 0x4CC2FF, 0x36D399, 0xFFB454, 0xFF5C5C,
};

static void write_surface(const char *name) {
    /* A PPM is enough to look at and needs no library. */
    char path[256]; snprintf(path, sizeof path, "tests/out/%s.ppm", name);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", UI_SURF_W, UI_SURF_H);
    for (int i = 0; i < UI_SURF_W * UI_SURF_H; i++) {
        const uint32_t c = PAL[vga_ui_fb[i] & 15];
        const uint8_t px[3] = { (uint8_t)(c >> 16), (uint8_t)(c >> 8), (uint8_t)c };
        fwrite(px, 1, 3, f);
    }
    fclose(f);
}

static uint8_t at(const int x, const int y) { return vga_ui_fb[y * UI_SURF_W + x]; }

/*
 * A rounded frame must leave its four corner pixels alone.
 *
 * `bg` is what was there before the frame was drawn; if the corner
 * still holds it, the corner was rounded off.
 */
static void check_corners(const char *what, const int x, const int y,
                          const int w, const int h, const uint8_t bg) {
    CHECK(at(x, y) == bg,                 "%s: top-left corner is rounded", what);
    CHECK(at(x + w - 1, y) == bg,         "%s: top-right corner is rounded", what);
    CHECK(at(x, y + h - 1) == bg,         "%s: bottom-left corner is rounded", what);
    CHECK(at(x + w - 1, y + h - 1) == bg, "%s: bottom-right corner is rounded", what);
}

int main(void) {
    mkdir("tests/out", 0777);
    memset(&settings, 0, sizeof settings);

    /* ---------------------------------------------------------------- */
    GROUP("primitives");
    memset(vga_ui_fb, C_BG, sizeof vga_ui_fb);
    ui_px(10, 10, C_TEXT);
    EQ(at(10, 10), C_TEXT, "a pixel lands where it was put");
    EQ(at(11, 10), C_BG,   "and nowhere else");

    ui_hline(20, 30, 40, C_ACCENT);
    EQ(at(20, 30), C_ACCENT, "hline starts at x");
    EQ(at(59, 30), C_ACCENT, "hline ends at x+w-1");
    EQ(at(60, 30), C_BG,     "hline stops there");

    ui_vline(70, 40, 20, C_ACCENT);
    EQ(at(70, 40), C_ACCENT, "vline starts at y");
    EQ(at(70, 59), C_ACCENT, "vline ends at y+h-1");
    EQ(at(70, 60), C_BG,     "vline stops there");

    ui_fill(100, 100, 30, 20, C_PANEL);
    EQ(at(100, 100), C_PANEL, "fill covers its corner");
    EQ(at(129, 119), C_PANEL, "fill covers its far corner");
    EQ(at(130, 120), C_BG,    "fill stops at its edge");

    /* Nothing may be drawn off the surface. */
    ui_px(-5, -5, C_TEXT); ui_px(UI_SURF_W + 9, UI_SURF_H + 9, C_TEXT);
    ui_hline(-40, 50, 20, C_TEXT); ui_vline(50, -40, 20, C_TEXT);
    ui_fill(-20, -20, 10, 10, C_TEXT);
    ui_fill(UI_SURF_W - 5, UI_SURF_H - 5, 40, 40, C_ICON_R);
    EQ(at(UI_SURF_W - 1, UI_SURF_H - 1), C_ICON_R, "a fill clips to the edge");
    tap_pass++;   /* survived the off-surface draws without corrupting memory */

    /* ---------------------------------------------------------------- */
    GROUP("rounded frames");
    ui_rounded = true;
    memset(vga_ui_fb, C_BG, sizeof vga_ui_fb);
    ui_round_rect(20, 20, 120, 60, 4, C_SEP, C_PANEL);
    check_corners("round_rect", 20, 20, 120, 60, C_BG);
    EQ(at(20 + 60, 20), C_SEP,        "the top edge is drawn between the corners");
    EQ(at(20 + 60, 20 + 59), C_SEP,   "and the bottom edge");
    EQ(at(20 + 60, 20 + 30), C_PANEL, "and the middle is filled");

    /* A border-only rounded frame must not fill. */
    memset(vga_ui_fb, C_BG, sizeof vga_ui_fb);
    ui_round_rect_border(30, 30, 100, 50, 4, C_ACCENT, C_BG);
    check_corners("round_rect_border", 30, 30, 100, 50, C_BG);
    EQ(at(30 + 50, 30 + 25), C_BG, "a border-only frame leaves the middle alone");

    /* Square frames stay square: ui_rounded off must mean off. */
    ui_rounded = false;
    memset(vga_ui_fb, C_BG, sizeof vga_ui_fb);
    ui_round_rect(20, 20, 120, 60, 4, C_SEP, C_PANEL);
    EQ(at(20, 20), C_SEP, "with rounding off the corner is drawn");
    ui_rounded = true;

    /* ---------------------------------------------------------------- */
    GROUP("text");
    memset(vga_ui_fb, C_BG, sizeof vga_ui_fb);
    const int w = ui_text(10, 10, "frank-dino", C_TEXT);
    CHECK(w > 0, "text reports a width");
    EQ(w, ui_text_width("frank-dino"), "the width matches ui_text_width()");
    unsigned lit = 0;
    for (int y = 8; y < 24; y++)
        for (int x = 8; x < 8 + w + 2; x++)
            if (at(x, y) == C_TEXT) lit++;
    CHECK(lit > 30, "the glyphs actually put pixels down (%u)", lit);
    /* Nothing outside the reported extent. */
    unsigned spill = 0;
    for (int y = 0; y < UI_SURF_H; y++)
        for (int x = 0; x < UI_SURF_W; x++)
            if (at(x, y) != C_BG && (x < 10 || x >= 10 + w || y < 10 || y >= 10 + 12))
                spill++;
    EQ(spill, 0, "text stays inside the box it reported");

    /* Every printable character must have a glyph. */
    memset(vga_ui_fb, C_BG, sizeof vga_ui_fb);
    unsigned blank = 0;
    for (int ch = 33; ch < 127; ch++) {
        memset(vga_ui_fb, C_BG, sizeof vga_ui_fb);
        const char s[2] = { (char)ch, 0 };
        ui_text(4, 4, s, C_TEXT);
        unsigned on = 0;
        for (int y = 0; y < 16; y++) for (int x = 0; x < 12; x++) if (at(x, y) == C_TEXT) on++;
        if (!on) { blank++; printf("        no glyph for '%c' (%d)\n", ch, ch); }
    }
    EQ(blank, 0, "every printable character has a glyph");

    /* Clipping must not run past the limit it was given. */
    memset(vga_ui_fb, C_BG, sizeof vga_ui_fb);
    ui_text_clip(10, 10, 30, "a very long line that will not fit at all", C_TEXT);
    unsigned past = 0;
    for (int y = 0; y < UI_SURF_H; y++)
        for (int x = 10 + 30; x < UI_SURF_W; x++)
            if (at(x, y) != C_BG) past++;
    EQ(past, 0, "clipped text stops at the limit");

    /* ---------------------------------------------------------------- */
    GROUP("surface");
    /* A composed screen, as a whole: nothing may be outside the palette. */
    memset(vga_ui_fb, C_BG, sizeof vga_ui_fb);
    ui_round_rect(4, 4, UI_SURF_W - 8, UI_SURF_H - 8, 6, C_SEP, C_PANEL);
    ui_rainbow(UI_SURF_W - 12 - ui_rainbow_w(), 10);
    ui_text(12, 10, "frank-dino", C_WHITE);
    ui_hline(8, 26, UI_SURF_W - 16, C_SEP);
    for (int i = 0; i < 6; i++) {
        const int y = 34 + i * 16;
        if (i == 2) ui_fill(8, y - 2, UI_SURF_W - 16, 14, C_SEL_BG);
        ui_text(16, y, "Machine", i == 2 ? C_WHITE : C_TEXT);
        ui_text(150, y, "128K", i == 2 ? C_WHITE : C_TEXT_DIM);
        ui_radio(280, y, i == 2, C_TEXT, C_ACCENT);
    }
    ui_chevron(UI_SURF_W - 20, 34, C_TEXT_DIM);
    check_corners("full screen frame", 4, 4, UI_SURF_W - 8, UI_SURF_H - 8, C_BG);
    unsigned oob = 0;
    for (int i = 0; i < UI_SURF_W * UI_SURF_H; i++) if (vga_ui_fb[i] > 15) oob++;
    EQ(oob, 0, "every pixel is a palette index");
    write_surface("menu");

    printf("  ....  wrote tests/out/menu.ppm\n");
    return tap_done("ui");
}
