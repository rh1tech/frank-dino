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

#include "dino.h"       // likely()/unlikely(), and VGA_BASE_PIN
#include "settings.h"
#include "spectrum.h"

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
 * pico-speccy's UI palette, twice, from its src/ui/UiGfx.cpp.
 *
 * kUiPalette is the menu's own "slate" theme in RGB888. Almost none of
 * it lands on the {00,55,AA,FF} grid a two-resistor ladder can make, so
 * their VGA driver dithers it across a 2x2 block of output pixels --
 * which is the default there and is the default here, because the
 * doubling this driver already does gives every logical UI pixel its own
 * 2x2 block to spend on exactly that.
 *
 * kUiPaletteVga is their on-grid twin, for the same menu when the dither
 * is not wanted. Their comment explains the choices in it: four levels
 * per channel cannot keep the slate theme's subtle darks apart, so the
 * dark ladder is re-spread by hand over black, navy and blue while the
 * text, selection and icon hues stay on the nearest grid point.
 */
static const uint32_t kUiPalette[16] = {
    0x101116,   // C_BG
    0x1C232F,   // C_PANEL
    0x262D3A,   // C_PANEL_ALT
    0x343A4B,   // C_SEP
    0xE8EDEF,   // C_TEXT
    0x8B95A7,   // C_TEXT_DIM
    0xFFFFFF,   // C_WHITE
    0x3A6DF6,   // C_SEL_BG
    0x2A3445,   // C_SEL_BAND
    0x4ADD84,   // C_ACCENT
    0x16212A,   // C_FOOT_BG
    0x081013,   // C_SHADOW
    0xE23A3A,   // C_ICON_R
    0xEFC43A,   // C_ICON_Y
    0x3AC7E2,   // C_ICON_C
    0x5A6477,   // C_DISABLED
};

static const uint32_t kUiPaletteVga[16] = {
    0x000000,   // C_BG
    0x000055,   // C_PANEL
    0x0000AA,   // C_PANEL_ALT
    0x555555,   // C_SEP
    0xFFFFFF,   // C_TEXT
    0xAAAAAA,   // C_TEXT_DIM
    0xFFFFFF,   // C_WHITE
    0x5555FF,   // C_SEL_BG
    0x0000AA,   // C_SEL_BAND
    0x55FFAA,   // C_ACCENT
    0x000000,   // C_FOOT_BG
    0x000000,   // C_SHADOW
    0xFF5555,   // C_ICON_R
    0xFFAA55,   // C_ICON_Y
    0x55AAFF,   // C_ICON_C
    0x555555,   // C_DISABLED
};

/*
 * The same sixteen roles again, on the grid, with the darks pulled down.
 *
 * Not pico-speccy's -- theirs, with two entries moved, and the reason is
 * this board's display rather than taste. Their VGA ladder spreads the
 * dark end over black, navy and blue so the three dark roles stay apart;
 * through an MS9288A onto an HDMI panel that navy reads as a lit blue
 * field behind every screen of text, which is not what a near-black
 * slate theme looks like and is not what the design wants. So C_PANEL
 * takes C_BG's black and C_PANEL_ALT takes C_PANEL's navy: the ladder is
 * the same ladder, one step lower, and the roles keep their order.
 *
 * What holds the layout together once the panel is black is the grey of
 * C_SEP -- the window border, the band rules and the pane separator are
 * the structure, exactly as they are in the slate original.
 */
static const uint32_t kUiPaletteBlack[16] = {
    0x000000,   // C_BG
    0x000000,   // C_PANEL     (theirs: 0x000055)
    0x000055,   // C_PANEL_ALT (theirs: 0x0000AA)
    0x555555,   // C_SEP
    0xFFFFFF,   // C_TEXT
    0xAAAAAA,   // C_TEXT_DIM
    0xFFFFFF,   // C_WHITE
    0x5555FF,   // C_SEL_BG
    0x000055,   // C_SEL_BAND  (follows C_PANEL_ALT, as theirs does)
    0x55FFAA,   // C_ACCENT
    0x000000,   // C_FOOT_BG
    0x000000,   // C_SHADOW
    0xFF5555,   // C_ICON_R
    0xFFAA55,   // C_ICON_Y
    0x55AAFF,   // C_ICON_C
    0x555555,   // C_DISABLED
};

/*
 * Each UI colour as the four pixels of its 2x2 dither block.
 *
 * ui_dither[c][(x & 1) + 2 * (y & 1)], in OUTPUT pixel coordinates -- so
 * the block lands inside the single logical pixel that the doubler is
 * expanding, and the eye averages it there rather than across a 2x2 grid
 * of UI pixels.
 */
static uint8_t ui_dither[16][4];

/*
 * The text mode's sixteen colours: the same theme, undithered.
 *
 * The file browser is all that is left in the eighty-column mode, and it
 * draws with the same role names the menu does -- so it takes the same
 * palette, rounded to the grid because a character cell has no 2x2 block
 * to dither inside.
 */
static uint8_t palette16[16];


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

/* 0..255 to the nearest of {00,55,AA,FF}. */
static inline uint8_t grid_level(const uint8_t v) { return (uint8_t)((v + 42) / 85); }

void vga_build_ui_palette(void) {
    const uint32_t *pal = settings.ui_theme == UI_THEME_NAVY  ? kUiPaletteVga
                        : settings.ui_theme == UI_THEME_SLATE ? kUiPalette
                                                              : kUiPaletteBlack;
    for (int c = 0; c < 16; c++) {
        vga_bayer4(pal[c], ui_dither[c]);
        for (int i = 0; i < 4; i++) ui_dither[c][i] |= 0xC0;   // both syncs idle high

        palette16[c] = PX(grid_level((pal[c] >> 16) & 0xFF),
                          grid_level((pal[c] >> 8)  & 0xFF),
                          grid_level( pal[c]        & 0xFF));
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
static uint16_t spec_pal16[16];
static uint16_t spec_pal16_dim[16];

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

static void build_spec_palette(void) {
    for (int i = 0; i < 16; i++) {
        const int lvl = (i & 8) ? 3 : 2;
        const int c   = i & 7;
        spec_palette[i] = PX((c & 2) ? lvl : 0,      // red
                             (c & 4) ? lvl : 0,      // green
                             (c & 1) ? lvl : 0);     // blue

        const int dim = lvl - 1;
        spec_palette_dim[i] = PX((c & 2) ? dim : 0,
                                 (c & 4) ? dim : 0,
                                 (c & 1) ? dim : 0);

        spec_pal16[i]     = (uint16_t)(spec_palette[i]     * 0x0101u);
        spec_pal16_dim[i] = (uint16_t)(spec_palette_dim[i] * 0x0101u);
    }
}

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
__force_inline static void render_spectrum_line(uint32_t *__restrict out, const uint32_t line) {
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
    const uint8_t tmode = settings.timex ? spec_timex_mode : 0u;

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

    if (unlikely(tmode == 6u)) {
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

    if (unlikely(settings.gigascreen)) {
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
        const uint16_t *__restrict pal = dim ? spec_pal16_dim : spec_pal16;
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

    const uint16_t *__restrict pal = dim ? spec_pal16_dim : spec_pal16;

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
    switch (tmode) {
        case 1:
            bmp = scr + 0x2000u + boff;
            att = scr + 0x2000u + 0x1800u + ((y >> 3) << 5);
            break;
        case 2:
            bmp = scr + boff;
            att = scr + 0x2000u + boff;
            break;
        default:
            bmp = scr + boff;
            att = scr + 0x1800u + ((y >> 3) << 5);
            break;
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
__force_inline static void render_ui_line(uint32_t *__restrict out, const uint32_t line) {
    const uint8_t *__restrict src = &vga_ui_fb[(line >> 1) * VGA_UI_W];
    const int yb = (line & 1) ? 2 : 0;      // which row of the 2x2 dither block

    for (int i = 0; i < VGA_UI_W; i += 2) {
        const uint8_t *__restrict a = ui_dither[src[i]     & 15];
        const uint8_t *__restrict b = ui_dither[src[i + 1] & 15];
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
        if (unlikely(spec_ulaplus_on) && line == VISIBLE_LINES) {
            if (spec_ulaplus_dirty) {
                spec_ulaplus_dirty = false;
                vga_build_ulaplus();
    build_gigascreen();
            } else {
                vga_refresh_ulaplus_border();   // OUT (#FE) raises no flag
            }
        }
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
        return;
    }
    if (likely(vga_mode == VGA_MODE_UI)) {
        render_ui_line(out, line);
        return;
    }

    /*
     * Nothing else. Every picture this board draws is one of the two
     * above: the machine's, or the menus' own surface.
     */
    for (int i = 0; i < VISIBLE_BYTES / 4; i++) out[i] = 0xC0C0C0C0u;
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

    irq_set_exclusive_handler(VGA_DMA_IRQ, vga_scanline);
    dma_channel_set_irq0_enabled(dma_ctrl_channel, true);
    irq_set_enabled(VGA_DMA_IRQ, true);

    dma_start_channel_mask(1u << dma_data_channel);
}
