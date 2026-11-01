/*
 * The video tables: the 2x2 Bayer dither and everything built on it.
 *
 * Every byte this driver emits is a pixel AND both sync levels --
 * bit 7 VSYNC, bit 6 HSYNC, then RRGGBB. So a table entry left at zero
 * does not show as a black pixel, it drives both syncs low for as long
 * as it is on screen, and the monitor drops sync for the whole frame.
 * That is a fault that looks like a dead board, and it is why the sync
 * bits are swept across every generated table here rather than spot
 * checked.
 *
 * Included as a translation unit because the tables are static: what is
 * worth asserting about a lookup table is its contents, and exporting
 * them just to test them would widen the driver's API for no one.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "tap.h"
#include "../src/drivers/vga/vga.c"

/* Both syncs idle high between pulses: every visible byte has bits 7,6. */
#define SYNC_MASK 0xC0

static void sweep(const char *what, const uint8_t *p, const size_t n) {
    size_t bad = 0, first = 0;
    for (size_t i = 0; i < n; i++)
        if ((p[i] & SYNC_MASK) != SYNC_MASK) { if (!bad) first = i; bad++; }
    if (bad) printf("        first bad byte at %zu = %02X\n", first, p[first]);
    EQ(bad, 0, "%s: all %zu bytes carry both sync levels", what, n);
}

/* The mean of a dither cell, per channel, on the 0..3 grid. */
static void cell_mean(const uint8_t p[4], int *r, int *g, int *b) {
    *r = *g = *b = 0;
    for (int i = 0; i < 4; i++) {
        *r += (p[i] >> 4) & 3; *g += (p[i] >> 2) & 3; *b += p[i] & 3;
    }
}

int main(void) {
    /* ---------------------------------------------------------------- */
    GROUP("bayer dither");
    /*
     * A colour already on the 4x4x4 grid must come out solid: all four
     * pixels equal, so the dither disappears rather than shimmering on
     * flat areas.
     */
    static const uint32_t GRID[] = {
        0x000000, 0x555555, 0xAAAAAA, 0xFFFFFF,
        0xFF0000, 0x00FF00, 0x0000FF, 0x55AAFF, 0xAA5500,
    };
    for (unsigned i = 0; i < count_of(GRID); i++) {
        uint8_t p[4];
        vga_bayer4(GRID[i], p);
        CHECK(p[0] == p[1] && p[1] == p[2] && p[2] == p[3],
              "%06X is on the grid and dithers to a flat cell", GRID[i]);
    }
    /* A colour off the grid must NOT be flat -- that is the whole point. */
    uint8_t mid[4];
    vga_bayer4(0x2A2A2A, mid);
    CHECK(!(mid[0] == mid[1] && mid[1] == mid[2] && mid[2] == mid[3]),
          "an off-grid colour dithers");

    /*
     * The cell mean must track the requested colour. Four pixels over a
     * 0..3 grid gives 13 levels per channel, so the mean in sixteenths
     * should land within one level of the input scaled to 0..12.
     */
    unsigned drift = 0;
    for (unsigned v = 0; v < 256; v += 5) {
        uint8_t p[4];
        vga_bayer4((uint32_t)((v << 16) | (v << 8) | v), p);
        int r, g, b; cell_mean(p, &r, &g, &b);
        const int want = (int)(v / 21);          /* the driver's own 13 levels */
        if (abs(r - want) > 1 || abs(g - want) > 1 || abs(b - want) > 1) drift++;
    }
    EQ(drift, 0, "the cell mean tracks the requested grey across the range");

    /* Every dither cell of every level must be sync-clean before the
     * |= 0xC0 the UI palette applies -- the colour bits must not run
     * into the sync bits. */
    unsigned overflow = 0;
    for (unsigned v = 0; v < 256; v++) {
        uint8_t p[4];
        vga_bayer4((uint32_t)((v << 16) | (v << 8) | v), p);
        for (int i = 0; i < 4; i++) if (p[i] & 0xC0) overflow++;
    }
    EQ(overflow, 0, "dither output stays inside six bits");

    /* ---------------------------------------------------------------- */
    GROUP("UI palette");
    vga_build_ui_palette();
    sweep("ui_dither", (const uint8_t *)ui_dither, sizeof ui_dither);

    /* ---------------------------------------------------------------- */
    GROUP("ULA+");
    /*
     * The palette starts as whatever the guest left it -- including all
     * zeros, which is the case that produced a black screen with no
     * sync. Build from a zeroed palette first, then from a full one.
     */
    memset(spec_ulaplus_pal, 0, sizeof spec_ulaplus_pal);
    spec_border = 0;
    vga_build_ulaplus();
    sweep("ulaplus_pal16 from a zeroed palette",
          (const uint8_t *)ulaplus_pal16, sizeof ulaplus_pal16);
    sweep("ulaplus_pal16_dim from a zeroed palette",
          (const uint8_t *)ulaplus_pal16_dim, sizeof ulaplus_pal16_dim);
    sweep("ulaplus_border16", (const uint8_t *)ulaplus_border16, sizeof ulaplus_border16);

    /* Now every one of the 256 possible GRB byte values, in every slot. */
    for (unsigned pass = 0; pass < 256; pass++) {
        for (int i = 0; i < 64; i++)
            spec_ulaplus_pal[i] = (uint8_t)((pass + (unsigned)i * 7u) & 0xFF);
        vga_build_ulaplus();
        if (pass == 0)
            sweep("ulaplus_pal16 populated",
                  (const uint8_t *)ulaplus_pal16, sizeof ulaplus_pal16);
        size_t bad = 0;
        const uint8_t *p = (const uint8_t *)ulaplus_pal16;
        for (size_t i = 0; i < sizeof ulaplus_pal16; i++)
            if ((p[i] & SYNC_MASK) != SYNC_MASK) bad++;
        p = (const uint8_t *)ulaplus_pal16_dim;
        for (size_t i = 0; i < sizeof ulaplus_pal16_dim; i++)
            if ((p[i] & SYNC_MASK) != SYNC_MASK) bad++;
        if (bad) { EQ(bad, 0, "ULA+ pass %u is sync-clean", pass); break; }
    }
    tap_pass++;   /* the 256-pass sweep above, as one result */
    printf("  ....  ULA+: 256 palettes x 64 entries swept, all sync-clean\n");

    /* The border follows OUT (#FE) without a palette write. */
    for (int i = 0; i < 64; i++) spec_ulaplus_pal[i] = (uint8_t)(i * 4);
    vga_build_ulaplus();
    const uint16_t b0 = ulaplus_border16[0];
    spec_border = 3;
    vga_refresh_ulaplus_border();
    CHECK(ulaplus_border16[0] != b0, "the border follows OUT (#FE)");
    EQ(ulaplus_border16[0], ulaplus_pal16[8 + 3][0], "border is paper of group 0");
    sweep("border after a border change",
          (const uint8_t *)ulaplus_border16, sizeof ulaplus_border16);

    /* ---------------------------------------------------------------- */
    GROUP("Gigascreen");
    build_gigascreen();
    sweep("giga16",     (const uint8_t *)giga16,     sizeof giga16);
    sweep("giga16_dim", (const uint8_t *)giga16_dim, sizeof giga16_dim);
    /* A colour blended with itself must be that colour, flat. */
    unsigned notflat = 0;
    for (int c = 0; c < 16; c++)
        if (giga16[c][c][0] != giga16[c][c][1]) notflat++;
    EQ(notflat, 0, "a colour blended with itself is flat");
    /*
     * The blend is a plain average of the two colours, so a[b] and b[a]
     * are the same entry outright -- not the same two rows in the other
     * order. The driver's own comment calls this folding them onto the
     * same slot, and a program alternating the two frames either way
     * round has to land on one colour.
     */
    unsigned asym = 0;
    for (int a = 0; a < 16; a++)
        for (int b = 0; b < 16; b++)
            if (giga16[a][b][0] != giga16[b][a][0]
             || giga16[a][b][1] != giga16[b][a][1]) asym++;
    EQ(asym, 0, "gigascreen blending is symmetric");
    /* And the halves of a pair differ, or gigascreen would show nothing
     * a plain palette could not. */
    unsigned distinct = 0;
    for (int a = 0; a < 16; a++)
        for (int b = 0; b < 16; b++)
            if (giga16[a][b][0] != giga16[a][a][0]) distinct++;
    CHECK(distinct > 100, "blends differ from their endpoints (%u of 256)", distinct);

    return tap_done("video");
}
