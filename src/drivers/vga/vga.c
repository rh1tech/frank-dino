/*
 * frank-dino — an RP2350B acting as the whole machine around a real Z80
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-dino
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "vga.h"

#include <string.h>
#include <hardware/clocks.h>
#include <hardware/dma.h>
#include <hardware/irq.h>
#include <hardware/pio.h>
#include <hardware/structs/timer.h>

#include "dino.h"       // likely()/unlikely(), and VGA_BASE_PIN
#include "settings.h"
#include "spectrum.h"
#include "ula_frame.h"
#include "atm.h"

#ifndef VGA_BASE_PIN
#define VGA_BASE_PIN 30
#endif

#define PIO_VGA     pio1
#define VGA_DMA_IRQ DMA_IRQ_0
#define VGA_PINS    8

// ============================================================================
// The wire format
// ============================================================================
//
// One byte per pixel, straight out of a PIO `out pins, 8` onto GP30-GP37,
// which is the R2R ladder and the two sync lines:
//
//   bit 7  VSYNC   bit 6  HSYNC   bits 5-4  R   bits 3-2  G   bits 1-0  B
//
// Both syncs are active low at 640x480/60, so an active-video byte has
// both high — every colour below carries 0xC0 and the sync templates
// clear one or both bits.
#define C_BG_INDEX 0      /* ui_gfx.h C_BG, without the dependency */
#define PX(r, g, b) ((uint8_t)(0xC0u | ((r) << 4) | ((g) << 2) | (b)))

/*
 * Sixteen colours, two bits a channel.
 *
 * The low-intensity half uses 10 rather than 01 for a lit channel — two
 * thirds, not one third. frank-xt8086 learned this the expensive way:
 * with 01 the whole palette came off the capture stick uniformly dim, and
 * CGA blue measured at a quarter of full scale instead of two thirds.
 * A two-resistor ladder has no room to be subtle about it.
 */
/*
 * The menus' colours, in ui_gfx.h's role order: the 128K's own menu --
 * a grey screen, white panels, black rules and title bars, the cyan bar,
 * the four-colour flash. Every one of them is on the {00,55,AA,FF} grid
 * the ladder makes, so the dither below comes out solid.
 */
static const uint32_t kUiPalette[] = {
    0xAAAAAA,   // C_BG          the screen, PAPER 7
    0xFFFFFF,   // C_PANEL       BRIGHT 1; PAPER 7
    0xFFFFFF,   // C_PANEL_ALT   bands inside a panel: the path, a dialog
    0x000000,   // C_SEP         rules and frames
    0x000000,   // C_TEXT
    0x555555,   // C_TEXT_DIM
    0x000000,   // C_WHITE       emphasis -- black, on a white panel
    0x55FFFF,   // C_SEL_BG      the 128's cyan bar
    0xAAAAAA,   // C_SEL_BAND    the bar in the pane without the focus
    0x0000AA,   // C_ACCENT      INK 1
    0xAAAAAA,   // C_FOOT_BG     the footer sits on the screen's grey
    0x555555,   // C_SHADOW
    0xAA0000,   // C_ICON_R      ink on white: INK 2, an error
    0xAA5500,   // C_ICON_Y      a warning -- INK 6 is too pale on white
    0x00AAAA,   // C_ICON_C      INK 5
    0xAAAAAA,   // C_DISABLED
    0xFF0000,   // C_FLASH_R     the flash, BRIGHT 1
    0xFFFF00,   // C_FLASH_Y
    0x00FF00,   // C_FLASH_G
    0x00FFFF,   // C_FLASH_C
    0x000000,   // C_TITLE_BG
    0xFFFFFF,   // C_TITLE_TEXT
    0x00AA00,   // C_OK          INK 4, a pass
};
#define UI_PAL_N (sizeof kUiPalette / sizeof kUiPalette[0])

/*
 * Each UI colour as the four pixels of its 2x2 dither block.
 *
 * ui_dither[c][(x & 1) + 2 * (y & 1)], in OUTPUT pixel coordinates -- so
 * the block lands inside the single logical pixel that the doubler is
 * expanding, and the eye averages it there rather than across a 2x2 grid
 * of UI pixels.
 */
static uint8_t ui_dither[32][4];     // the surface's index, masked to five bits



/*
 * pico-speccy's Bayer 2x2 ordered dither, from drivers/vga-nextgen/vga.c.
 *
 *   Threshold matrix: [0 2]   fill order per sub-level:
 *                     [3 1]   (0,0), (1,1), (0,1), (1,0)
 *
 * A channel is quantised to thirteen levels; the top two bits pick the
 * grid colour below it and the bottom two say how many of the four
 * pixels step up to the next one. A colour already on the grid comes out
 * with sub = 0 for every channel, which makes all four pixels equal and
 * the dither disappear -- so the solid palette needs no separate path.
 *
 * Their result is a 6-bit RRGGBB byte, which is this board's PX() layout
 * without the two sync bits, so it is used as it comes.
 */
static void vga_bayer4(const uint32_t color888, uint8_t p[4]) {
    const uint8_t r_level = ((color888 >> 16) & 0xff) / 21;
    const uint8_t g_level = ((color888 >> 8)  & 0xff) / 21;
    const uint8_t b_level = ( color888        & 0xff) / 21;

    const uint8_t r_lo = r_level >> 2, r_sub = r_level & 3, r_hi = r_lo + (r_lo < 3 && r_sub);
    const uint8_t g_lo = g_level >> 2, g_sub = g_level & 3, g_hi = g_lo + (g_lo < 3 && g_sub);
    const uint8_t b_lo = b_level >> 2, b_sub = b_level & 3, b_hi = b_lo + (b_lo < 3 && b_sub);

    p[0] = ((r_sub >= 1 ? r_hi : r_lo) << 4) | ((g_sub >= 1 ? g_hi : g_lo) << 2) | (b_sub >= 1 ? b_hi : b_lo);
    p[1] = ((r_sub >= 3 ? r_hi : r_lo) << 4) | ((g_sub >= 3 ? g_hi : g_lo) << 2) | (b_sub >= 3 ? b_hi : b_lo);
    p[2] = (r_lo << 4) | (g_lo << 2) | b_lo;
    p[3] = ((r_sub >= 2 ? r_hi : r_lo) << 4) | ((g_sub >= 2 ? g_hi : g_lo) << 2) | (b_sub >= 2 ? b_hi : b_lo);
}

void vga_build_ui_palette(void) {
    for (unsigned c = 0; c < 32; c++) {
        vga_bayer4(c < UI_PAL_N ? kUiPalette[c] : 0, ui_dither[c]);
        for (int i = 0; i < 4; i++) ui_dither[c][i] |= 0xC0;   // both syncs idle high
    }
}

// ============================================================================
// Timing
// ============================================================================
//
// Industry-standard 640x480 at 59.94 Hz: 800 pixels and 525 lines at
// 25.175 MHz, both syncs negative. One output byte per pixel, so a line
// is 800 bytes whatever is in it.
#define PIXEL_CLOCK      25175000
#define LINE_BYTES       800
#define VISIBLE_BYTES    640
#define TOTAL_LINES      525
#define VISIBLE_LINES    480
#define VSYNC_START_LINE 490
#define VSYNC_END_LINE   491

/*
 * Where the picture starts inside the line buffer.
 *
 * The DMA starts each buffer at byte 0 and the line begins with the sync
 * pulse, so the visible 640 bytes sit at an offset: the front porch and
 * the sync and the back porch come first. 144 bytes is 16 front porch +
 * 96 sync + 32 of the 48-byte back porch — the remaining 16 are the shift
 * that puts the picture where a display expects it. Moving this moves the
 * image sideways and nothing else.
 */
#define PICTURE_OFFSET_BYTES 144
#define HSYNC_BYTES          96

uint8_t vga_ui_fb[VGA_UI_W * VGA_UI_H] __attribute__((section(".psram")));
volatile uint32_t vga_frames;
/* Lines whose interrupt came more than a line late (the frame they were
 * in went out a line short), and the longest a line took to render, in
 * microseconds. Read by tools/dino.py. */
volatile uint32_t vga_late_lines, vga_line_max_us;

static volatile vga_mode_t vga_mode = VGA_MODE_UI;

void vga_set_mode(const vga_mode_t mode) { vga_mode = mode; }
vga_mode_t vga_get_mode(void) { return vga_mode; }

// ============================================================================
// The Spectrum's picture inside a 640x480 one
// ============================================================================
//
// 256x192 doubled to 512x384, centred, and the 64 columns and 48 lines
// left over on each side are the border — which is what the border is on
// a real television, so the proportions come out close to right rather
// than being a frame drawn around a small picture.
#define SPEC_X0     64          // first visible picture byte in the line
#define SPEC_Y0     48          // first picture scanline
#define SPEC_H      384         // 192 doubled

/*
 * Sixteen colours, two bits a channel.
 *
 * Bright is full scale and normal is two thirds, which is the same
 * decision the text palette makes and for the same reason: on a
 * two-resistor ladder the one-third step is too dark to read. Black has
 * no bright variant on a Spectrum and none here.
 *
 * The attribute's colour bits are GRB, not RGB — bit 0 is blue, bit 1 is
 * red, bit 2 is green. Getting that backwards swaps red and blue on
 * every screen ever written, which looks deliberate enough to miss.
 */
static uint8_t spec_palette[16];

/*
 * The same fifteen colours, one step darker.
 *
 * Scanlines on a doubled picture cost nothing if the darker version is
 * precomputed: each Spectrum pixel row already occupies two output
 * lines, so the effect is a choice of palette on the second of them
 * rather than any per-pixel work. With two bits per channel there is
 * exactly one step to give, which is what makes it a toggle and not a
 * strength.
 */
static uint8_t spec_palette_dim[16];

/*
 * A doubled Spectrum pixel is two output bytes, so a colour is a 16-bit
 * pattern and not a byte.
 *
 * In the ordinary sixteen-colour mode both halves are the same byte and
 * this changes nothing. In ULA+ they are the two halves of that colour's
 * Bayer cell for this output line -- which is what lets sixty-four
 * arbitrary colours come out of four levels a channel. The doubling was
 * already there; the dither costs the table below and nothing at run
 * time.
 *
 * dmask2 masks at sixteen-bit granularity, so the inner loop does not
 * change at all: where it used to repeat a byte four times it now
 * repeats a pattern twice.
 */
/* [scanline-dimmed][Bayer row][colour]: the 16 ZX colours as each output
 * line draws them. A colour on the grid has the same pattern in both rows,
 * so the default palette is as solid as it ever was. */
static uint16_t spec_pal16x[2][2][16];

/* ULA+: [entry][Bayer row], and the dimmed twin for scanlines. */
static uint16_t ulaplus_pal16[64][2];
static uint16_t ulaplus_pal16_dim[64][2];
static uint16_t ulaplus_border16[2], ulaplus_border16_dim[2];

/*
 * G3R3B2 to RGB888, from pico-speccy's Video.cpp grb_to_rgb888().
 *
 * Bits 7:5 green, 4:2 red, 1:0 blue -- the order is the chip's, not a
 * mistake -- and each field is replicated up rather than shifted, so
 * full-scale in three bits is full-scale in eight.
 */
static uint32_t grb_to_rgb888(const uint8_t grb) {
    const uint8_t g3 = (uint8_t)((grb >> 5) & 0x07);
    const uint8_t r3 = (uint8_t)((grb >> 2) & 0x07);
    const uint8_t b2 = (uint8_t)(grb & 0x03);
    const uint8_t R = (uint8_t)((r3 << 5) | (r3 << 2) | (r3 >> 1));
    const uint8_t G = (uint8_t)((g3 << 5) | (g3 << 2) | (g3 >> 1));
    const uint8_t B = (uint8_t)((b2 << 6) | (b2 << 4) | (b2 << 2) | b2);
    return ((uint32_t)R << 16) | ((uint32_t)G << 8) | B;
}

/* One colour's two 16-bit line patterns, through the same Bayer cell the
 * menu uses. `scale` is 256 for full and less for the scanline twin. */
static void ulaplus_entry(const uint32_t rgb, const int scale,
                          uint16_t out[2]) {
    const uint32_t r = (((rgb >> 16) & 0xFF) * (uint32_t)scale) >> 8;
    const uint32_t g = (((rgb >> 8)  & 0xFF) * (uint32_t)scale) >> 8;
    const uint32_t b = (( rgb        & 0xFF) * (uint32_t)scale) >> 8;

    uint8_t p[4];
    vga_bayer4((r << 16) | (g << 8) | b, p);
    for (int i = 0; i < 4; i++) p[i] |= 0xC0u;      // both syncs idle high

    out[0] = (uint16_t)(p[0] | (p[1] << 8));        // even line: cell row 0
    out[1] = (uint16_t)(p[2] | (p[3] << 8));        // odd line:  cell row 1
}

/*
 * Rebuild the ULA+ tables. Called from the scanline interrupt's own
 * frame boundary, never from inside a port write: sixty-four entries is
 * far too much work to do between two Z80 clock edges.
 */
void vga_build_ulaplus(void) {
    for (int i = 0; i < 64; i++) {
        const uint32_t rgb = grb_to_rgb888(spec_ulaplus_pal[i]);
        ulaplus_entry(rgb, 256, ulaplus_pal16[i]);
        ulaplus_entry(rgb, 168, ulaplus_pal16_dim[i]);   // ~2/3, as the ZX dim is
    }
    vga_refresh_ulaplus_border();
}

/*
 * The border takes the paper of group 0, which is entry 8 + colour.
 *
 * Its own function, and called every frame rather than only on a palette
 * write, because OUT (#FE) changes the border without touching the
 * palette -- and a border that only moved when the palette did would be
 * a fault nobody could find from the guest's side.
 */
void vga_refresh_ulaplus_border(void) {
    const uint8_t bi = (uint8_t)(8u + (spec_border & 7u));
    ulaplus_border16[0]     = ulaplus_pal16[bi][0];
    ulaplus_border16[1]     = ulaplus_pal16[bi][1];
    ulaplus_border16_dim[0] = ulaplus_pal16_dim[bi][0];
    ulaplus_border16_dim[1] = ulaplus_pal16_dim[bi][1];
}

/*
 * Gigascreen: every pair of ZX colours, blended, as two line patterns.
 *
 * giga16[a][b][row] is what a pixel that is colour `a` on bank 5 and
 * colour `b` on bank 7 looks like -- the average of the two, dithered
 * across the 2x2 block the pixel already occupies. The table is 16x16
 * rather than pico-speccy's 120 entries because it is indexed rather
 * than searched, and a kilobyte is cheaper here than the arithmetic to
 * fold a[b] and b[a] onto the same slot.
 */
static uint16_t giga16[16][16][2];
static uint16_t giga16_dim[16][16][2];

/* The ZX colour `i` as RGB888, at `lvl` for a lit channel. */
static uint32_t zx_rgb888(const int i, const uint32_t lvl) {
    const int c = i & 7;
    return (((c & 2) ? lvl : 0) << 16)      /* red   */
         | (((c & 4) ? lvl : 0) << 8)       /* green */
         |  ((c & 1) ? lvl : 0);            /* blue  */
}

static void build_gigascreen(void) {
    for (int a = 0; a < 16; a++) {
        for (int b = 0; b < 16; b++) {
            const uint32_t ra = zx_rgb888(a, (a & 8) ? 0xFFu : 0xAAu);
            const uint32_t rb = zx_rgb888(b, (b & 8) ? 0xFFu : 0xAAu);
            const uint32_t mix =
                  ((((ra >> 16 & 0xFF) + (rb >> 16 & 0xFF)) / 2) << 16)
                | ((((ra >> 8  & 0xFF) + (rb >> 8  & 0xFF)) / 2) << 8)
                |  (((ra       & 0xFF) + (rb       & 0xFF)) / 2);
            ulaplus_entry(mix, 256, giga16[a][b]);
            ulaplus_entry(mix, 168, giga16_dim[a][b]);
        }
    }
}

/*
 * The look of the 16 ZX colours: a palette, a scanline strength and a CRT
 * filter, all applied here and none of them per pixel.
 *
 * Palettes are pico-speccy's (Video.cpp, Unreal Speccy's format): black,
 * normal and bright levels and a 3x3 matrix. "Sinclair" is this board's
 * own AA/FF, every colour on the ladder's grid -- solid, as it has always
 * been. The others land between grid levels and come out through the
 * 2x2 dither the ULA+ already uses.
 */
typedef struct { uint8_t zz, nn, bb; uint16_t m[9]; } zx_palette_t;
static const zx_palette_t kZxPalettes[] = {
    { 0x00, 0xAA, 0xFF, { 0x100,0,0, 0,0x100,0, 0,0,0x100 } },                          // Sinclair
    { 0x00, 0xCD, 0xFF, { 0x100,0,0, 0,0x100,0, 0,0,0x100 } },                          // Pulsar
    { 0x00, 0xA0, 0xFF, { 0x100,0,0, 0,0x100,0, 0,0,0x100 } },                          // Alone
    { 0x00, 0xCD, 0xFF, { 0x049,0x092,0x024, 0x049,0x092,0x024, 0x049,0x092,0x024 } },  // Grayscale
    { 0x00, 0xCD, 0xFF, { 0x100,0,0, 0x040,0x0C0,0, 0,0x040,0x0C0 } },                  // Mars
    { 0x00, 0xCD, 0xFF, { 0x0D0,0,0x030, 0,0x0D0,0x030, 0,0,0x100 } },                  // Ocean
};
#define ZX_PALETTES (sizeof kZxPalettes / sizeof kZxPalettes[0])

/* Scanline strength: what the dimmed line keeps, out of 256. */
static const uint16_t kScanScale[5] = { 256, 224, 184, 128, 64 };

/* pico-speccy's CRT colour stage: P22-ish phosphors, rows summing to 1.0
 * so white stays white, then a gamma curve and a black lift. Levels 1-3,
 * and 4-6 the same with the aperture grille. */
static const uint16_t kCrtPhosphor[4][9] = {
    { 0x100, 0, 0,  0, 0x100, 0,  0, 0, 0x100 },
    {  246,  8, 3,   5,  248, 3,   3, 15, 238 },
    {  238, 15, 3,  10,  241, 5,   5, 26, 225 },
    {  230, 23, 3,  15,  233, 8,   8, 36, 212 },
};
static const uint8_t kCrtLift[4] = { 0, 4, 8, 12 };
/* 255 * (x/255)^gamma for gamma 1.0 / 1.15 / 1.30 / 1.45, on 17 points; linear between. */
static const uint8_t kCrtGamma[4][17] = {
    {   0,  16,  32,  48,  64,  80,  96, 112, 128, 144, 160, 176, 192, 208, 224, 240, 255 },
    {   0,  11,  23,  37,  52,  67,  83,  99, 115, 132, 149, 166, 184, 202, 220, 238, 255 },
    {   0,   7,  17,  29,  42,  57,  72,  88, 104, 121, 139, 157, 176, 196, 215, 236, 255 },
    {   0,   5,  13,  23,  34,  47,  62,  77,  94, 111, 130, 149, 169, 190, 211, 234, 255 },
};

static uint32_t mat3(const uint32_t rgb, const uint16_t m[9]) {
    const int r = (rgb >> 16) & 0xFF, g = (rgb >> 8) & 0xFF, b = rgb & 0xFF;
    int o[3];
    for (int k = 0; k < 3; k++) {
        const int v = (r * m[3 * k] + g * m[3 * k + 1] + b * m[3 * k + 2]) >> 8;
        o[k] = v > 255 ? 255 : v;
    }
    return ((uint32_t)o[0] << 16) | ((uint32_t)o[1] << 8) | (uint32_t)o[2];
}

static uint8_t gamma_lift(const uint8_t v, const unsigned st) {
    const uint8_t *g = kCrtGamma[st];
    const unsigned i = v >> 4, f = v & 15u;
    int o = (int)(g[i] + ((g[i + 1] - g[i]) * (int)f) / 16) + kCrtLift[st];
    return (uint8_t)(o > 255 ? 255 : o);
}

static uint32_t zx_colour(const int i) {
    const zx_palette_t *p = &kZxPalettes[settings.palette < ZX_PALETTES ? settings.palette : 0];
    const uint32_t lvl = (i & 8) ? p->bb : p->nn;
    const int c = i & 7;
    uint32_t rgb = (((c & 2) ? lvl : p->zz) << 16) | (((c & 4) ? lvl : p->zz) << 8) | ((c & 1) ? lvl : p->zz);
    rgb = mat3(rgb, p->m);
    const unsigned st = settings.crt <= 3 ? settings.crt : settings.crt - 3u;
    if (st) {
        rgb = mat3(rgb, kCrtPhosphor[st]);
        rgb = ((uint32_t)gamma_lift((uint8_t)(rgb >> 16), st) << 16)
            | ((uint32_t)gamma_lift((uint8_t)(rgb >> 8), st) << 8)
            |  (uint32_t)gamma_lift((uint8_t)rgb, st);
    }
    return rgb;
}

static inline uint8_t grid_level(const uint32_t v) { return (uint8_t)((v + 42u) / 85u); }

static uint8_t to_px(const uint32_t rgb, const unsigned scale) {
    return (uint8_t)PX(grid_level((((rgb >> 16) & 0xFF) * scale) >> 8),
                       grid_level((((rgb >> 8)  & 0xFF) * scale) >> 8),
                       grid_level((( rgb        & 0xFF) * scale) >> 8));
}

static void build_spec_palette(void) {
    const unsigned scan = kScanScale[settings.scanlines < 5 ? settings.scanlines : 1];
    const bool grille = settings.crt >= 4;
    for (int i = 0; i < 16; i++) {
        const uint32_t rgb = zx_colour(i);
        for (int d = 0; d < 2; d++) {
            const unsigned scale = d ? scan : 256u;
            uint16_t rows[2];
            ulaplus_entry(rgb, (int)scale, rows);
            if (grille) {
                /* The aperture grille: the right pixel of each doubled pair
                 * at two thirds -- 171, the one factor that keeps the
                 * ladder's grid on the grid. */
                uint16_t dark[2];
                ulaplus_entry(rgb, (int)(scale * 171u >> 8), dark);
                for (int r = 0; r < 2; r++) rows[r] = (uint16_t)((rows[r] & 0x00FFu) | (dark[r] & 0xFF00u));
            }
            spec_pal16x[d][0][i] = rows[0];
            spec_pal16x[d][1][i] = rows[1];
        }
        /* The byte-wide paths (16 colours, hi-res) have no 2x2 cell to
         * dither in: the nearest grid colour. */
        spec_palette[i]     = to_px(rgb, 256u);
        spec_palette_dim[i] = to_px(rgb, scan);
    }
}

volatile bool vga_spec_palette_dirty;
void vga_spec_palette_changed(void) { vga_spec_palette_dirty = true; }

/*
 * Nibble to four doubled pixels.
 *
 * dmask2[n][0] covers bits 3 and 2 of the nibble, each becoming two
 * output bytes; dmask2[n][1] covers bits 1 and 0. Bit 3 is leftmost,
 * because in Spectrum screen memory bit 7 of a byte is the leftmost
 * pixel — the opposite convention to this firmware's font, which is a
 * good way to get one of them wrong.
 */
static uint32_t dmask2[16][2];

static void build_dmask2(void) {
    for (int n = 0; n < 16; n++) {
        // word 0: bit3 bit3 bit2 bit2   word 1: bit1 bit1 bit0 bit0
        const uint32_t b3 = (n & 8) ? 0xFFFFu : 0, b2 = (n & 4) ? 0xFFFF0000u : 0;
        const uint32_t b1 = (n & 2) ? 0xFFFFu : 0, b0 = (n & 1) ? 0xFFFF0000u : 0;
        dmask2[n][0] = b3 | b2;
        dmask2[n][1] = b1 | b0;
    }
}

/*
 * One Spectrum scanline, straight out of the guest's RAM.
 *
 * The screen address is the famous one: the third of the screen, the
 * character row within it and the pixel row within the character are
 * three separate fields of the address and they are not in that order.
 * Bits 6-5 of y pick the third, bits 2-0 pick the line within a
 * character cell, and bits 5-3 pick the row of cells.
 */
/*
 * The frame the ULA saw, and where in its border changes this HDMI frame
 * has got to. Taken at the first visible line and held for the whole
 * HDMI frame, so a picture never mixes two guest frames.
 */
static const ula_frame_t *beam_frame;
static uint32_t beam_i, beam_col;           // the cursor at the current guest line...
static uint32_t beam_i0, beam_col0;         // ...and where that line started

/* Eight guest pixels of border at T-state t: four words. */
__force_inline static uint32_t *beam_border(uint32_t *__restrict out, const ula_frame_t *f,
                                            const uint16_t *__restrict pal, const uint32_t t) {
    while (beam_i < f->nev && ULA_EV_T(f->ev[beam_i]) <= t) beam_col = ULA_EV_COL(f->ev[beam_i++]);
    const uint32_t w = (uint32_t)pal[beam_col & 7u] * 0x00010001u;
    out[0] = w; out[1] = w; out[2] = w; out[3] = w;
    return out + 4;
}

/*
 * One line of the beam-accurate picture.
 *
 * The paper comes from the frame core 1 built, cell by cell as the ULA
 * fetched it; the border is walked through its list of changes, eight
 * guest pixels -- four T-states -- at a time, which is the finest the
 * real ULA's border latch resolves on a Sinclair machine.
 */
__force_inline static void render_beam_line(uint32_t *__restrict out, const uint32_t line,
                                            const ula_frame_t *f, const uint16_t *__restrict pal) {
    // Both output lines of a guest line start from the same place.
    if (!(line & 1)) { beam_i0 = beam_i; beam_col0 = beam_col; }
    else             { beam_i = beam_i0; beam_col = beam_col0; }

    const int32_t row = (int32_t)(line >> 1) - (int32_t)(SPEC_Y0 / 2);   // paper row, -24..215
    const uint32_t tline = (uint32_t)((int32_t)f->paper_t0 + row * (int32_t)f->line_t);
    const uint32_t tleft = tline - 16u;          // 32 guest pixels of left border

    if (row < 0 || row >= 192) {
        for (uint32_t k = 0; k < VISIBLE_BYTES / 16u; k++) out = beam_border(out, f, pal, tleft + k * 4u);
        return;
    }

    for (uint32_t k = 0; k < SPEC_X0 / 16u; k++) out = beam_border(out, f, pal, tleft + k * 4u);

    const uint8_t *__restrict bmp = f->bmp[row];
    const uint8_t *__restrict att = f->att[row];
    const bool flash = spec_flash;
    for (int col = 0; col < 32; col++) {
        const uint8_t bits = bmp[col];
        const uint8_t a    = att[col];
        const uint8_t bright = (a & 0x40) >> 3;
        uint32_t ink   = (uint32_t)pal[(a & 0x07) | bright] * 0x00010001u;
        uint32_t paper = (uint32_t)pal[((a >> 3) & 0x07) | bright] * 0x00010001u;
        if (unlikely(flash && (a & 0x80))) { const uint32_t t = ink; ink = paper; paper = t; }
        const uint32_t *hi = dmask2[bits >> 4];
        const uint32_t *lo = dmask2[bits & 15];
        *out++ = (ink & hi[0]) | (paper & ~hi[0]);
        *out++ = (ink & hi[1]) | (paper & ~hi[1]);
        *out++ = (ink & lo[0]) | (paper & ~lo[0]);
        *out++ = (ink & lo[1]) | (paper & ~lo[1]);
    }

    for (uint32_t k = 0; k < SPEC_X0 / 16u; k++) out = beam_border(out, f, pal, tline + 128u + k * 4u);
}

/*
 * The ATM's palette in place of the Spectrum's, while an ATM is running.
 *
 * Sixteen entries, each two bits a channel, so it maps onto this board's
 * ladder exactly. Rebuilt in the blanking when the guest writes one, the
 * same way ULA+ is; put back to the Spectrum's when the machine changes.
 */
static bool pal_is_atm;

static void build_atm_palette(void) {
    uint8_t rgb[16][3];
    atm_palette_rgb2(rgb);
    for (int i = 0; i < 16; i++) {
        spec_palette[i] = PX(rgb[i][0], rgb[i][1], rgb[i][2]);
        spec_palette_dim[i] = PX(rgb[i][0] ? rgb[i][0] - 1 : 0, rgb[i][1] ? rgb[i][1] - 1 : 0,
                                 rgb[i][2] ? rgb[i][2] - 1 : 0);
        for (int r = 0; r < 2; r++) {
            spec_pal16x[0][r][i] = (uint16_t)(spec_palette[i]     * 0x0101u);
            spec_pal16x[1][r][i] = (uint16_t)(spec_palette_dim[i] * 0x0101u);
        }
    }
}

/* Eight hires pixels, one output byte each: ink is attr 6,2-0, paper 7,5-3. */
__force_inline static uint8_t *atm_bits8(uint8_t *__restrict px, const uint8_t bits, const uint8_t a,
                                         const uint8_t *__restrict pal) {
    const uint8_t ink   = pal[((a & 0x40u) >> 3) | (a & 7u)];
    const uint8_t paper = pal[((a & 0x80u) >> 4) | ((a >> 3) & 7u)];
    for (int k = 0; k < 8; k++) *px++ = (bits & (0x80u >> k)) ? ink : paper;
    return px;
}

/*
 * The ATM's own modes, after pico-speccy's VIDEO::atmRenderLine: 200
 * lines doubled into the middle 400 of the 480, from the screen page (5
 * or 7) and the page four below it.
 */
static void render_atm_line(uint32_t *__restrict out, const uint32_t line, const atm_vmode_t vm,
                            const bool dim) {
    const uint8_t  *__restrict pal   = dim ? spec_palette_dim : spec_palette;
    const uint16_t *__restrict pal16 = spec_pal16x[dim][line & 1];
    const uint32_t border32 = (uint32_t)pal16[atm_border_index() & 15u] * 0x00010001u;
    if (line < 40u || line >= 440u) {
        for (int i = 0; i < VISIBLE_BYTES / 4; i++) out[i] = border32;
        return;
    }
    const uint32_t y = (line - 40u) >> 1;               // 0..199
    const uint8_t *__restrict scr = spec_ram[spec_screen_bank];
    const uint8_t *__restrict alt = spec_ram[spec_screen_bank - 4u];
    const uint32_t o = y * 40u;

    if (vm == ATM_VM_EGA) {
        uint16_t *__restrict px = (uint16_t *)out;
        for (uint32_t c = 0; c < 40u; c++) {
            const uint8_t b[4] = { alt[o + c], scr[o + c], alt[0x2000u + o + c], scr[0x2000u + o + c] };
            for (int k = 0; k < 4; k++) {
                const uint8_t v = b[k];
                *px++ = pal16[((v & 0x40u) >> 3) | (v & 0x07u)];
                *px++ = pal16[((v & 0x80u) >> 4) | ((v >> 3) & 0x07u)];
            }
        }
        return;
    }

    uint8_t *__restrict px = (uint8_t *)out;
    if (vm == ATM_VM_HIRES) {
        for (uint32_t x = 0; x < 40u; x++) {
            px = atm_bits8(px, scr[o + x], alt[o + x], pal);
            px = atm_bits8(px, scr[0x2000u + o + x], alt[0x2000u + o + x], pal);
        }
        return;
    }

    // 80x25 text: 64-byte rows from #01C0; even columns' symbols at +0 and
    // attributes at alt+#2000, odd columns' at +#2000 and alt+1.
    const uint8_t *__restrict fnt = atm_font_ptr() + (y & 7u);
    const uint32_t base = 0x01C0u + (y >> 3) * 64u;
    for (uint32_t x = 0; x < 40u; x++) {
        px = atm_bits8(px, fnt[scr[base + x] * 8u], alt[0x2000u + base + x], pal);
        px = atm_bits8(px, fnt[scr[0x2000u + base + x] * 8u], alt[base + 1u + x], pal);
    }
}

__force_inline static void render_spectrum_line(uint32_t *__restrict out, const uint32_t line) {
    if (unlikely(spec_is_atm(spec_machine))) {
        const atm_vmode_t vm = atm_video_mode();
        if (vm != ATM_VM_ZX) {
            render_atm_line(out, line, vm, settings.scanlines && (line & 1));
            return;
        }
    }
    if (unlikely(line == 0)) {
        beam_frame = ula_frame_latest();
        beam_i = 0;
        beam_col = beam_frame ? beam_frame->border_at_start : 0u;
    }

    /*
     * Which of the two palettes this output line uses.
     *
     * The picture is doubled, so every odd line of a pair is the one to
     * darken -- including in the border, or the effect stops at the edge
     * of the paper and looks like a fault rather than a filter.
     *
     * The same parity picks the Bayer row in ULA+, which is not a
     * coincidence worth relying on and is written out separately below.
     */
    const bool dim = (settings.scanlines && (line & 1));
    const int  yb  = (line & 1) ? 1 : 0;

    /*
     * The SCLD's mode, or none of it.
     *
     * Read once, here, rather than at each of the three places below:
     * the setting can be turned off while the guest has left a mode
     * selected, and a stale 6 would go on drawing hi-res out of a
     * machine that no longer has a SCLD in it.
     */
    const uint8_t tmode = (settings.timex || spec_has_scld(spec_machine)) ? spec_timex_mode : 0u;

    if (unlikely(spec_ulaplus_on)) {
        const uint16_t (*__restrict p16)[2] = dim ? ulaplus_pal16_dim : ulaplus_pal16;
        const uint16_t b16 = dim ? ulaplus_border16_dim[yb] : ulaplus_border16[yb];
        const uint32_t border32 = (uint32_t)b16 * 0x00010001u;

        if (line < SPEC_Y0 || line >= SPEC_Y0 + SPEC_H) {
            for (int i = 0; i < VISIBLE_BYTES / 4; i++) out[i] = border32;
            return;
        }

        const uint32_t y = (line - SPEC_Y0) >> 1;
        const uint8_t *__restrict scr = spec_ram[spec_screen_bank];
        const uint8_t *__restrict bmp = scr + (((y & 0xC0) << 5) | ((y & 0x38) << 2) | ((y & 0x07) << 8));
        const uint8_t *__restrict att = scr + 0x1800 + ((y >> 3) << 5);

        for (int i = 0; i < SPEC_X0 / 4; i++) *out++ = border32;

        for (int col = 0; col < 32; col++) {
            const uint8_t bits = bmp[col];
            const uint8_t a    = att[col];

            /*
             * The attribute means something else here: bits 7-6 pick one
             * of four groups of sixteen, 5-3 the paper inside it, 2-0 the
             * ink. There is no FLASH and no BRIGHT left to honour.
             */
            const unsigned grp = (unsigned)(a >> 6) * 16u;
            const uint32_t ink   = (uint32_t)p16[grp + (a & 7u)][yb]            * 0x00010001u;
            const uint32_t paper = (uint32_t)p16[grp + 8u + ((a >> 3) & 7u)][yb] * 0x00010001u;

            const uint32_t *hi = dmask2[bits >> 4];
            const uint32_t *lo = dmask2[bits & 15];

            *out++ = (ink & hi[0]) | (paper & ~hi[0]);
            *out++ = (ink & hi[1]) | (paper & ~hi[1]);
            *out++ = (ink & lo[0]) | (paper & ~lo[0]);
            *out++ = (ink & lo[1]) | (paper & ~lo[1]);
        }

        for (int i = 0; i < SPEC_X0 / 4; i++) *out++ = border32;
        return;
    }

    if (unlikely(spec_pent16c)) {
        /*
         * The Pentagon 1024SL's 16-colour mode, after pico-speccy's
         * col16RenderLine: four planes -- banks 4 and 5 (or 6 and 7,
         * with the shadow screen) at +0 and +#2000 -- at the bitmap's
         * line address, each byte two pixels of four bits, IGRB: the
         * low pixel bits 6 and 2-0, the high bits 7 and 5-3.
         */
        const uint16_t *__restrict pal = spec_pal16x[dim][line & 1];
        const uint32_t border32 = (uint32_t)pal[spec_border & 7] * 0x00010001u;
        if (line < SPEC_Y0 || line >= SPEC_Y0 + SPEC_H) {
            for (int i = 0; i < VISIBLE_BYTES / 4; i++) out[i] = border32;
            return;
        }
        const uint32_t y = (line - SPEC_Y0) >> 1;
        const uint32_t off = ((y & 0xC0) << 5) | ((y & 0x38) << 2) | ((y & 0x07) << 8);
        const bool alt = spec_screen_bank == 7;
        const uint8_t *__restrict pa = spec_ram[alt ? 6 : 4] + off;
        const uint8_t *__restrict pb = spec_ram[alt ? 7 : 5] + off;
        const uint8_t *__restrict pc = pa + 0x2000u;
        const uint8_t *__restrict pd = pb + 0x2000u;
        for (int i = 0; i < SPEC_X0 / 4; i++) *out++ = border32;
        uint16_t *__restrict px = (uint16_t *)out;
        for (int j = 0; j < 32; j++) {
            const uint8_t bytes[4] = { pa[j], pb[j], pc[j], pd[j] };
            for (int k = 0; k < 4; k++) {
                const uint8_t x = bytes[k];
                *px++ = pal[((x >> 3) & 0x08u) | (x & 0x07u)];          // left
                *px++ = pal[((x >> 4) & 0x08u) | ((x >> 3) & 0x07u)];   // right
            }
        }
        out = (uint32_t *)px;
        for (int i = 0; i < SPEC_X0 / 4; i++) *out++ = border32;
        return;
    }

    if (unlikely(spec_machine == MACHINE_PROFI && (spec_port_dffd & 0x80u))) {
        /*
         * The Profi's DS80: 512x240, a bitmap byte and an attribute byte
         * for every eight pixels, the bitmap in page 4 (6 with #7FFD bit
         * 3) and the colours in page 56 (58). After pico-speccy's DS80
         * branch of MainScreen. Exactly 480 lines doubled, so no top or
         * bottom border; 512 pixels one to a byte, as Timex hi-res.
         */
        const uint8_t *__restrict pal = dim ? spec_palette_dim : spec_palette;
        const uint32_t border32 = (uint32_t)pal[spec_border & 7] * 0x01010101u;
        const uint32_t y = line >> 1;                       // 0..239
        const bool alt = spec_screen_bank == 7;
        const uint8_t *__restrict g = spec_bank_host(alt ? 6u : 4u);
        const uint8_t *__restrict c = spec_bank_host(alt ? 58u : 56u);
        const uint32_t off = 2048u * (y >> 6) + 256u * (y & 7u) + ((y & 0x38u) << 2);
        for (int i = 0; i < SPEC_X0 / 4; i++) *out++ = border32;
        uint8_t *__restrict px = (uint8_t *)out;
        for (uint32_t j = 0; j < 32u; j++) {
            for (int half = 0; half < 2; half++) {
                const uint32_t at = off + j + (half ? 0u : 8192u);   // even from +#2000
                const uint8_t bits = g[at], a = c[at];
                const uint8_t ink   = pal[(a & 0x07u) | ((a & 0x40u) >> 3)];
                const uint8_t paper = pal[((a & 0x38u) >> 3) | ((a & 0x80u) >> 4)];
                for (int k = 0; k < 8; k++) *px++ = (bits & (0x80u >> k)) ? ink : paper;
            }
        }
        out = (uint32_t *)px;
        for (int i = 0; i < SPEC_X0 / 4; i++) *out++ = border32;
        return;
    }

    /*
     * The SCLD's mode bits are three switches, not a number -- bit 2
     * hi-res, bit 1 hi-colour, bit 0 the second screen -- as Fuse decodes
     * them. The TC2068's own ROM boots in mode 7, which is hi-res with the
     * second-screen bit set and ignored; treating only 6 as hi-res showed
     * it as an empty first screen.
     */
    if (unlikely(tmode & 4u)) {
        /*
         * Timex hi-res: 512 monochrome pixels, and the picture area is
         * already 512 output pixels wide. So this is the one mode that
         * does not double -- each byte written here is one real pixel,
         * which is the whole point of the mode and the reason it needs
         * no framebuffer trickery to show on this board.
         *
         * Two colours, from port #FF rather than from any attribute:
         * theirs is (ink & 7) | 8 and the paper is its complement, both
         * bright.
         */
        const uint8_t *__restrict pal = dim ? spec_palette_dim : spec_palette;
        const uint8_t ink   = pal[(spec_timex_ink & 7u) | 8u];
        const uint8_t paper = pal[((~spec_timex_ink) & 7u) | 8u];
        const uint32_t border32 = (uint32_t)pal[spec_border & 7] * 0x01010101u;

        if (line < SPEC_Y0 || line >= SPEC_Y0 + SPEC_H) {
            for (int i = 0; i < VISIBLE_BYTES / 4; i++) out[i] = border32;
            return;
        }

        const uint32_t y = (line - SPEC_Y0) >> 1;
        const uint32_t off = ((y & 0xC0) << 5) | ((y & 0x38) << 2) | ((y & 0x07) << 8);
        const uint8_t *__restrict s0 = spec_ram[5] + off;
        const uint8_t *__restrict s1 = spec_ram[5] + 0x2000u + off;

        for (int i = 0; i < SPEC_X0 / 4; i++) *out++ = border32;

        uint8_t *__restrict px = (uint8_t *)out;
        for (int col = 0; col < 32; col++) {
            const uint8_t a = s0[col], b = s1[col];
            for (int k = 0; k < 8; k++) *px++ = (a & (0x80u >> k)) ? ink : paper;
            for (int k = 0; k < 8; k++) *px++ = (b & (0x80u >> k)) ? ink : paper;
        }
        out = (uint32_t *)px;

        for (int i = 0; i < SPEC_X0 / 4; i++) *out++ = border32;
        return;
    }

    if (unlikely(settings.gigascreen == 1 || (settings.gigascreen == 2 && spec_giga_auto))) {
        /*
         * Both screens at once, blended in the 2x2 block.
         *
         * The slow path on purpose: a pixel here can be any of four
         * combinations -- ink or paper on bank 5, crossed with ink or
         * paper on bank 7 -- so the nibble-mask trick the ordinary path
         * uses has nothing to mask with, and this walks the eight pixels
         * of a cell by hand. Two hundred and fifty-six iterations a
         * line, which is nothing against a line's thirty-one
         * microseconds, and it only runs when the setting is on.
         */
        const uint16_t (*__restrict g)[16][2] = dim ? giga16_dim : giga16;
        const uint16_t *__restrict pal = spec_pal16x[dim][line & 1];
        const uint32_t border32 = (uint32_t)pal[spec_border & 7] * 0x00010001u;

        if (line < SPEC_Y0 || line >= SPEC_Y0 + SPEC_H) {
            for (int i = 0; i < VISIBLE_BYTES / 4; i++) out[i] = border32;
            return;
        }

        const uint32_t y = (line - SPEC_Y0) >> 1;
        const uint32_t off = ((y & 0xC0) << 5) | ((y & 0x38) << 2) | ((y & 0x07) << 8);
        const uint32_t aoff = 0x1800u + ((y >> 3) << 5);

        const uint8_t *__restrict b5 = spec_ram[5] + off;
        const uint8_t *__restrict a5 = spec_ram[5] + aoff;
        const uint8_t *__restrict b7 = spec_ram[7] + off;
        const uint8_t *__restrict a7 = spec_ram[7] + aoff;

        for (int i = 0; i < SPEC_X0 / 4; i++) *out++ = border32;

        uint16_t *__restrict px = (uint16_t *)out;
        for (int col = 0; col < 32; col++) {
            const uint8_t m5 = b5[col], t5 = a5[col];
            const uint8_t m7 = b7[col], t7 = a7[col];

            const uint8_t br5 = (uint8_t)((t5 & 0x40) >> 3);
            const uint8_t br7 = (uint8_t)((t7 & 0x40) >> 3);
            const uint8_t i5 = (uint8_t)((t5 & 7) | br5), p5 = (uint8_t)(((t5 >> 3) & 7) | br5);
            const uint8_t i7 = (uint8_t)((t7 & 7) | br7), p7 = (uint8_t)(((t7 >> 3) & 7) | br7);

            for (int b = 0; b < 8; b++) {
                const uint8_t mask = (uint8_t)(0x80u >> b);
                *px++ = g[(m5 & mask) ? i5 : p5][(m7 & mask) ? i7 : p7][yb];
            }
        }
        out = (uint32_t *)px;

        for (int i = 0; i < SPEC_X0 / 4; i++) *out++ = border32;
        return;
    }

    const uint16_t *__restrict pal = spec_pal16x[dim][line & 1];

    if (likely(beam_frame) && tmode == 0u) {
        render_beam_line(out, line, beam_frame, pal);
        return;
    }

    const uint32_t border32 = (uint32_t)pal[spec_border & 7] * 0x00010001u;

    if (line < SPEC_Y0 || line >= SPEC_Y0 + SPEC_H) {
        for (int i = 0; i < VISIBLE_BYTES / 4; i++) out[i] = border32;
        return;
    }

    const uint32_t y = (line - SPEC_Y0) >> 1;           // 0..191

    const uint8_t *__restrict scr = spec_ram[spec_screen_bank];
    const uint32_t boff = ((y & 0xC0) << 5) | ((y & 0x38) << 2) | ((y & 0x07) << 8);

    /*
     * Where the bitmap and the attributes are, which the SCLD moves.
     *
     * Mode 1 puts both in the second area. Mode 2 -- hi-colour -- leaves
     * the bitmap where it was and reads the attributes from the second
     * area at the BITMAP's offset, which is what turns one attribute per
     * character row into one per pixel row.
     */
    const uint8_t *__restrict bmp;
    const uint8_t *__restrict att;
    if (tmode & 2u) {                   // hi-colour (bit 0 does not apply)
        bmp = scr + boff;
        att = scr + 0x2000u + boff;
    } else if (tmode & 1u) {            // the second screen
        bmp = scr + 0x2000u + boff;
        att = scr + 0x2000u + 0x1800u + ((y >> 3) << 5);
    } else {
        bmp = scr + boff;
        att = scr + 0x1800u + ((y >> 3) << 5);
    }

    const bool flash = spec_flash;

    for (int i = 0; i < SPEC_X0 / 4; i++) *out++ = border32;

    for (int col = 0; col < 32; col++) {
        const uint8_t bits = bmp[col];
        const uint8_t a    = att[col];

        const uint8_t bright = (a & 0x40) >> 3;         // -> palette bit 3
        uint32_t ink   = (uint32_t)pal[(a & 0x07) | bright] * 0x00010001u;
        uint32_t paper = (uint32_t)pal[((a >> 3) & 0x07) | bright] * 0x00010001u;

        // FLASH swaps the pair, every sixteen frames, for this cell only.
        if (unlikely(flash && (a & 0x80))) { const uint32_t t = ink; ink = paper; paper = t; }

        const uint32_t *hi = dmask2[bits >> 4];
        const uint32_t *lo = dmask2[bits & 15];

        *out++ = (ink & hi[0]) | (paper & ~hi[0]);
        *out++ = (ink & hi[1]) | (paper & ~hi[1]);
        *out++ = (ink & lo[0]) | (paper & ~lo[0]);
        *out++ = (ink & lo[1]) | (paper & ~lo[1]);
    }

    for (int i = 0; i < SPEC_X0 / 4; i++) *out++ = border32;
}

/*
 * One line of the menu's surface, doubled.
 *
 * Each of the 320 bytes is a palette index and becomes two output
 * bytes, and each of the 240 rows becomes two output lines -- the same
 * doubling the Spectrum's picture gets, and for the same reason: it is
 * free here and it is what lets the UI be authored at the resolution
 * pico-speccy authored it at.
 *
 * The two output pixels a source byte becomes are the two HALVES of its
 * dither block, picked by the line's parity -- so the block the eye
 * averages is exactly the doubled pixel and not a 2x2 grid of them.
 *
 * Two source bytes make one output word, so the inner loop runs 160
 * times and never touches a byte pointer on the output side.
 */
/*
 * The menu's rows, fetched ahead into SRAM.
 *
 * The surface is in the PSRAM, and reading a row of it from inside the
 * line interrupt cost up to 28 us of a 31.8 us line while the menu was
 * redrawing on the same core -- every so often a line interrupt came too
 * late, the frame went out a line short, and the whole menu jumped a
 * line on the screen. So the interrupt asks for the NEXT row, and a
 * software interrupt one priority below copies it into one of two SRAM
 * rows while the current one is going out; it has two lines to do it
 * in, and the line interrupt preempts it whenever it needs to.
 */
static uint8_t           ui_row[2][VGA_UI_W] __attribute__((aligned(4)));
static volatile int      ui_row_tag[2] = { -1, -1 };   // which surface row each holds
static volatile int      ui_row_want = -1;
static uint              ui_row_irq;

static void __time_critical_func(ui_row_fetch)(void) {
    const int r = ui_row_want;
    if (r < 0) return;
    ui_row_tag[r & 1] = -1;
    const uint32_t *src = (const uint32_t *)&vga_ui_fb[r * VGA_UI_W];
    uint32_t *dst = (uint32_t *)ui_row[r & 1];
    for (int i = 0; i < VGA_UI_W / 4; i++) dst[i] = src[i];
    ui_row_tag[r & 1] = r;
}

static inline void ui_row_request(const int r) {
    ui_row_want = r;
    irq_set_pending(ui_row_irq);
}

__force_inline static void render_ui_line(uint32_t *__restrict out, const uint32_t line) {
    const int r = (int)(line >> 1);
    if (!(line & 1u) && r + 1 < VGA_UI_H) ui_row_request(r + 1);
    // The fetched row if it made it in time; the PSRAM itself if not.
    const uint8_t *__restrict src = ui_row_tag[r & 1] == r ? ui_row[r & 1]
                                                          : &vga_ui_fb[r * VGA_UI_W];
    const int yb = (line & 1) ? 2 : 0;      // which row of the 2x2 dither block

    for (int i = 0; i < VGA_UI_W; i += 2) {
        const uint8_t *__restrict a = ui_dither[src[i]     & 31];
        const uint8_t *__restrict b = ui_dither[src[i + 1] & 31];
        *out++ = (uint32_t)a[yb] | ((uint32_t)a[yb + 1] << 8)
               | ((uint32_t)b[yb] << 16) | ((uint32_t)b[yb + 1] << 24);
    }
}

// --- PIO: one instruction, `out pins, 8`, wrapped onto itself ---
static uint16_t vga_pio_instructions[] = { 0x6008 };
static const struct pio_program vga_pio_program = {
    .instructions = vga_pio_instructions,
    .length = 1,
    .origin = -1,
};

// scanline_buffers: [0] blank line, [1] vsync line, [2] the picture line
enum { VBLANK, VSYNC, IMAGE, SCANLINE_BUFFERS };
static uint32_t *scanline_buffers[SCANLINE_BUFFERS];
static uint32_t  scanline_mem[SCANLINE_BUFFERS * (LINE_BYTES / 4)] __attribute__((aligned(32)));

/*
 * A second physical buffer behind the IMAGE slot.
 *
 * The DMA control channel reads scanline_buffers[IMAGE] the instant it
 * fires, to hand the data channel a source for the line going out now —
 * and the interrupt that follows immediately starts rendering the *next*
 * line into it. With one buffer those are the same memory, and whichever
 * words the data channel has not reached yet come out as the new line
 * instead of the old one: a sparse, timing-dependent tear. Alternating
 * gives each buffer a full line to drain.
 */
static uint32_t image_buffer_b[LINE_BYTES / 4] __attribute__((aligned(32)));

static uint sm;
static int dma_ctrl_channel;
static int dma_data_channel;

// ============================================================================
// One scanline
// ============================================================================

void __time_critical_func(vga_scanline)(void) {
    static uint32_t line = 0;
    static bool image_toggle = false;

    dma_hw->ints0 = 1u << dma_ctrl_channel;

    static uint32_t last_us;
    const uint32_t now_us = timer0_hw->timerawl;
    if (unlikely(now_us - last_us > 48u) && last_us) vga_late_lines++;   // a line is 31.8 us
    last_us = now_us;

    if (unlikely(++line == TOTAL_LINES)) {
        line = 0;
        vga_frames++;
    }

    if (unlikely(line >= VISIBLE_LINES)) {
        /*
         * Sixty-four entries through a Bayer cell is far too much work to
         * do between two Z80 clock edges, so a port write only raises a
         * flag and the rebuild happens here, in the blanking, where there
         * is nothing else to do with the time.
         */
        if (line == VISIBLE_LINES) {
            const bool atm = spec_is_atm(spec_machine);
            if (atm && (atm_pal_dirty || !pal_is_atm)) {
                atm_pal_dirty = false;
                build_atm_palette();
                pal_is_atm = true;
            } else if (!atm && (pal_is_atm || vga_spec_palette_dirty)) {
                vga_spec_palette_dirty = false;
                build_spec_palette();
                pal_is_atm = false;
            }
        }
        if (unlikely(spec_ulaplus_on) && line == VISIBLE_LINES) {
            if (spec_ulaplus_dirty) {
                spec_ulaplus_dirty = false;
                vga_build_ulaplus();
    build_gigascreen();
            } else {
                vga_refresh_ulaplus_border();   // OUT (#FE) raises no flag
            }
        }
        if (line == VISIBLE_LINES && vga_mode == VGA_MODE_UI) ui_row_request(0);
        const int which = (line >= VSYNC_START_LINE && line <= VSYNC_END_LINE) ? VSYNC : VBLANK;
        dma_channel_set_read_addr(dma_ctrl_channel, &scanline_buffers[which], false);
        return;
    }

    image_toggle = !image_toggle;
    scanline_buffers[IMAGE] = image_toggle ? image_buffer_b
                                           : &scanline_mem[IMAGE * (LINE_BYTES / 4)];
    dma_channel_set_read_addr(dma_ctrl_channel, &scanline_buffers[IMAGE], false);

    uint32_t *__restrict out = scanline_buffers[IMAGE] + PICTURE_OFFSET_BYTES / 4;

    if (vga_mode == VGA_MODE_SPECTRUM) {
        render_spectrum_line(out, line);
    } else if (likely(vga_mode == VGA_MODE_UI)) {
        render_ui_line(out, line);
    } else {
        /*
         * Nothing else. Every picture this board draws is one of the two
         * above: the machine's, or the menus' own surface.
         */
        for (int i = 0; i < VISIBLE_BYTES / 4; i++) out[i] = 0xC0C0C0C0u;
    }
    const uint32_t took = timer0_hw->timerawl - now_us;
    if (unlikely(took > vga_line_max_us)) vga_line_max_us = took;
}

// ============================================================================
// Bring-up
// ============================================================================

static void build_sync_templates(void) {
    const uint8_t active = 0xC0;            // both syncs idle high
    const uint8_t hsync  = active ^ 0x40;   // H low
    const uint8_t vsync  = active ^ 0x80;   // V low, H idle
    const uint8_t both   = active ^ 0xC0;   // the sync pulse inside a V line

    memset(scanline_buffers[VBLANK], active, LINE_BYTES);
    memset(scanline_buffers[VBLANK], hsync,  HSYNC_BYTES);

    memset(scanline_buffers[VSYNC], vsync, LINE_BYTES);
    memset(scanline_buffers[VSYNC], both,  HSYNC_BYTES);

    // The picture lines start life as blank lines and only ever have
    // their visible 640 bytes overwritten, so the sync and the porches
    // are written once here and never touched again.
    memcpy(scanline_buffers[IMAGE], scanline_buffers[VBLANK], LINE_BYTES);
    memcpy(image_buffer_b,          scanline_buffers[VBLANK], LINE_BYTES);
}

static void configure_dma(void) {
    dma_channel_config data_cfg = dma_channel_get_default_config(dma_data_channel);
    channel_config_set_transfer_data_size(&data_cfg, DMA_SIZE_32);
    channel_config_set_read_increment(&data_cfg, true);
    channel_config_set_write_increment(&data_cfg, false);
    channel_config_set_dreq(&data_cfg, (PIO_VGA == pio0 ? DREQ_PIO0_TX0 : DREQ_PIO1_TX0) + sm);
    channel_config_set_chain_to(&data_cfg, dma_ctrl_channel);
    dma_channel_configure(dma_data_channel, &data_cfg,
                          &PIO_VGA->txf[sm], scanline_buffers[VBLANK],
                          LINE_BYTES / 4, false);

    // The control channel's whole job is to reload the data channel's
    // read address from a pointer the scanline interrupt has already
    // updated, then chain straight back into it. Two channels ping-pong
    // for ever and the CPU is only ever asked what the *next* line is.
    dma_channel_config ctrl_cfg = dma_channel_get_default_config(dma_ctrl_channel);
    channel_config_set_transfer_data_size(&ctrl_cfg, DMA_SIZE_32);
    channel_config_set_read_increment(&ctrl_cfg, false);
    channel_config_set_write_increment(&ctrl_cfg, false);
    channel_config_set_chain_to(&ctrl_cfg, dma_data_channel);
    dma_channel_configure(dma_ctrl_channel, &ctrl_cfg,
                          &dma_hw->ch[dma_data_channel].read_addr,
                          &scanline_buffers[VBLANK], 1, false);
}

void vga_init(void) {
    vga_build_ui_palette();
    build_dmask2();
    build_spec_palette();

    /*
     * ULA+ starts black but valid.
     *
     * A zeroed table is not "black": every output byte carries both sync
     * bits, and 0x00 has them LOW. Leaving the tables unbuilt until the
     * first palette write would mean a guest that enables ULA+ before
     * filling it drives sync low for a whole frame, which is not a dark
     * screen -- it is no signal at all.
     */
    vga_build_ulaplus();

    /*
     * The menus' surface starts black.
     *
     * It is in the PSRAM, which holds whatever it held before the reset
     * -- and every byte of it is a valid palette index, so an uncleared
     * surface is a screenful of coloured noise for the millisecond or
     * two before the boot report paints over it.
     */
    memset(vga_ui_fb, C_BG_INDEX, sizeof vga_ui_fb);

    for (int i = 0; i < SCANLINE_BUFFERS; i++)
        scanline_buffers[i] = &scanline_mem[i * (LINE_BYTES / 4)];

    // The ladder is on GP30-37, which a state machine in the default
    // window cannot reach past GP31. Moving PIO1's window to 16 puts all
    // eight inside it.
    pio_set_gpio_base(PIO_VGA, 16);
    const uint offset = pio_add_program(PIO_VGA, &vga_pio_program);
    sm = pio_claim_unused_sm(PIO_VGA, true);

    for (int i = 0; i < VGA_PINS; i++) {
        gpio_init(VGA_BASE_PIN + i);
        gpio_set_dir(VGA_BASE_PIN + i, GPIO_OUT);
        pio_gpio_init(PIO_VGA, VGA_BASE_PIN + i);
    }
    pio_sm_set_consecutive_pindirs(PIO_VGA, sm, VGA_BASE_PIN, VGA_PINS, true);

    pio_sm_config cfg = pio_get_default_sm_config();
    sm_config_set_out_pin_base(&cfg, VGA_BASE_PIN);
    sm_config_set_wrap(&cfg, offset, offset + vga_pio_program.length - 1);
    sm_config_set_fifo_join(&cfg, PIO_FIFO_JOIN_TX);
    sm_config_set_out_shift(&cfg, true, true, 32);
    sm_config_set_out_pins(&cfg, VGA_BASE_PIN, VGA_PINS);

    pio_sm_init(PIO_VGA, sm, offset, &cfg);
    pio_sm_set_enabled(PIO_VGA, sm, true);
    pio_sm_set_clkdiv(PIO_VGA, sm, (float)clock_get_hz(clk_sys) / PIXEL_CLOCK);

    dma_ctrl_channel = dma_claim_unused_channel(true);
    dma_data_channel = dma_claim_unused_channel(true);
    configure_dma();
    build_sync_templates();

    // The line interrupt above everything; the menu's row fetch just
    // below it, and above the USB and the rest at the default priority.
    ui_row_irq = (uint)user_irq_claim_unused(true);
    irq_set_exclusive_handler(ui_row_irq, ui_row_fetch);
    irq_set_priority(ui_row_irq, 0x40);
    irq_set_enabled(ui_row_irq, true);

    irq_set_exclusive_handler(VGA_DMA_IRQ, vga_scanline);
    irq_set_priority(VGA_DMA_IRQ, 0x00);
    dma_channel_set_irq0_enabled(dma_ctrl_channel, true);
    irq_set_enabled(VGA_DMA_IRQ, true);

    dma_start_channel_mask(1u << dma_data_channel);
}
