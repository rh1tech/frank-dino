/*
 * frank-dino — an RP2350B acting as the whole machine around a real Z80
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-dino
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "ui_capture.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ff.h"
#include "spectrum.h"
#include "storage.h"

#define CAP_DIR    "/captures"
#define CAP_W      320
#define CAP_H      240
#define CAP_X0     32           /* 256 wide inside 320 */
#define CAP_Y0     24           /* 192 tall inside 240 */

/*
 * One output row, in the PSRAM with everything else the UI keeps there.
 *
 * Three hundred and twenty bytes would fit anywhere; it is over there
 * because it is written between two card calls with the Z80 stopped, so
 * it costs nothing and the SRAM has none to spare.
 */
static uint8_t row[CAP_W] __attribute__((section(".psram")));

/*
 * A 40-byte BMP header and a 256-entry palette, as theirs writes them:
 * BITMAPFILEHEADER, BITMAPINFOHEADER, then the palette, then the pixels
 * bottom-up. Eight bits a pixel, because the picture only ever has
 * sixteen colours in it and a 24-bit file would be three times the card
 * traffic for nothing.
 */
#define BMP_PAL_ENTRIES 256
#define BMP_PIX_OFFSET  (14 + 40 + BMP_PAL_ENTRIES * 4)

static void put32(uint8_t *p, const uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static void put16(uint8_t *p, const uint16_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
}

/*
 * The sixteen ZX colours in BMP's BGRA order.
 *
 * Bright is full scale and normal is two thirds, which is the same
 * decision the video driver's palette makes and for the same reason --
 * so a capture looks like what the screen looked like rather than like
 * what the attribute bytes said.
 *
 * The attribute's colour bits are GRB, not RGB: bit 0 is blue, bit 1 is
 * red, bit 2 is green.
 */
static void zx_palette(uint8_t *pal) {
    memset(pal, 0, BMP_PAL_ENTRIES * 4);
    for (int i = 0; i < 16; i++) {
        const int lvl = (i & 8) ? 0xFF : 0xAA;
        const int c   = i & 7;
        pal[i * 4 + 0] = (uint8_t)((c & 1) ? lvl : 0);      /* blue  */
        pal[i * 4 + 1] = (uint8_t)((c & 4) ? lvl : 0);      /* green */
        pal[i * 4 + 2] = (uint8_t)((c & 2) ? lvl : 0);      /* red   */
    }
}

/* One row of the picture, paper and border, as palette indices. */
static void render_row(const int y) {
    const uint8_t border = spec_border & 7;
    memset(row, border, CAP_W);

    if (y < CAP_Y0 || y >= CAP_Y0 + 192) return;

    const uint32_t sy = (uint32_t)(y - CAP_Y0);
    const uint8_t *scr = spec_ram[spec_screen_bank];
    const uint8_t *bmp = scr + (((sy & 0xC0) << 5) | ((sy & 0x38) << 2) | ((sy & 0x07) << 8));
    const uint8_t *att = scr + 0x1800 + ((sy >> 3) << 5);

    for (int col = 0; col < 32; col++) {
        const uint8_t bits = bmp[col];
        const uint8_t a    = att[col];
        const uint8_t bright = (a & 0x40) >> 3;
        uint8_t ink   = (uint8_t)((a & 0x07) | bright);
        uint8_t paper = (uint8_t)(((a >> 3) & 0x07) | bright);
        if (spec_flash && (a & 0x80)) { const uint8_t t = ink; ink = paper; paper = t; }

        uint8_t *out = &row[CAP_X0 + col * 8];
        for (int b = 0; b < 8; b++)
            out[b] = (bits & (0x80 >> b)) ? ink : paper;
    }
}

/*
 * The next free number in /captures, found by reading what is there.
 *
 * Theirs does the same and for the same reason: a counter in memory
 * restarts at every boot and quietly overwrites yesterday's captures.
 */
static int next_number(void) {
    DIR dir;
    if (f_opendir(&dir, CAP_DIR) != FR_OK) return 1;

    int best = 0;
    FILINFO fi;
    while (f_readdir(&dir, &fi) == FR_OK && fi.fname[0]) {
        if (strncmp(fi.fname, "DINO", 4) != 0) continue;
        const int n = atoi(fi.fname + 4);
        if (n > best) best = n;
    }
    f_closedir(&dir);
    return best + 1;
}

bool ui_capture_screen(char *detail, const unsigned detail_len) {
    if (!storage_mounted()) {
        snprintf(detail, detail_len, "No card to write to.");
        return false;
    }

    FILINFO fi;
    if (f_stat(CAP_DIR, &fi) != FR_OK && f_mkdir(CAP_DIR) != FR_OK) {
        snprintf(detail, detail_len, "Could not make " CAP_DIR);
        return false;
    }

    char path[48];
    snprintf(path, sizeof path, CAP_DIR "/DINO%04d.bmp", next_number());

    FIL f;
    if (f_open(&f, path, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) {
        snprintf(detail, detail_len, "Could not write %s", path);
        return false;
    }

    const uint32_t pixels = (uint32_t)CAP_W * CAP_H;

    uint8_t hdr[14 + 40];
    memset(hdr, 0, sizeof hdr);
    hdr[0] = 'B'; hdr[1] = 'M';
    put32(hdr + 2, BMP_PIX_OFFSET + pixels);        /* bfSize */
    put32(hdr + 10, BMP_PIX_OFFSET);                /* bfOffBits */
    put32(hdr + 14, 40);                            /* biSize */
    put32(hdr + 18, CAP_W);
    put32(hdr + 22, CAP_H);                         /* positive: bottom-up */
    put16(hdr + 26, 1);                             /* biPlanes */
    put16(hdr + 28, 8);                             /* biBitCount */
    put32(hdr + 34, pixels);                        /* biSizeImage */
    put32(hdr + 46, BMP_PAL_ENTRIES);               /* biClrUsed */

    UINT put = 0;
    bool ok = (f_write(&f, hdr, sizeof hdr, &put) == FR_OK && put == sizeof hdr);

    if (ok) {
        uint8_t pal[BMP_PAL_ENTRIES * 4];
        zx_palette(pal);
        ok = (f_write(&f, pal, sizeof pal, &put) == FR_OK && put == sizeof pal);
    }

    /* Bottom-up, which is the one thing about BMP that surprises people. */
    for (int y = CAP_H - 1; ok && y >= 0; y--) {
        render_row(y);
        ok = (f_write(&f, row, CAP_W, &put) == FR_OK && put == CAP_W);
    }

    f_close(&f);
    if (!ok) {
        f_unlink(path);
        snprintf(detail, detail_len, "The card stopped taking it.");
        return false;
    }
    snprintf(detail, detail_len, "Saved %s", path);
    return true;
}
