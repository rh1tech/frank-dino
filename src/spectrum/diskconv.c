/*
 * frank-dino — an RP2350B acting as the whole machine around a real Z80
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-dino
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The FDI and UDI layouts are as pico-speccy's wd1793.cpp
 * (https://github.com/drewpo28/pico-speccy, GPL-3.0) and UnrealSpeccy read
 * them; TD0 through pico-speccy's td0.cpp, after UnrealSpeccy's wldr_td0.
 */

/*
 * Disk images that are not .trd, turned into one.
 *
 * The Beta Disk here answers by result rather than by flux: it keeps a
 * TR-DOS disk as its sectors in order, which is what a .trd is. An FDI,
 * a UDI or a Teledisk image of an ordinary TR-DOS disk carries those same
 * sixteen 256-byte sectors a track, wrapped in a description of the
 * track they sat on -- so each is unwrapped into the .trd layout on the
 * way in. A disk whose protection lives in that wrapping (odd sector
 * sizes, missing sectors, deliberate CRC errors) loses it, as the .trd
 * itself would; everything else boots.
 */
#include "diskconv.h"

#include <string.h>

#define SEC     256u
#define SPT     16u

static uint32_t rd16(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return rd16(p) | (rd16(p + 2) << 16); }

/* One sector into its .trd place, if it is a TR-DOS sector. */
static void place(uint8_t *trd, const uint32_t trd_max, uint32_t *hi,
                  const unsigned cyl, const unsigned head, const unsigned r,
                  const unsigned n, const uint8_t *data) {
    if (n != 1 || r < 1 || r > SPT) return;
    const uint32_t at = ((cyl * 2u + head) * SPT + (r - 1u)) * SEC;
    if (at + SEC > trd_max) return;
    memcpy(trd + at, data, SEC);
    if (at + SEC > *hi) *hi = at + SEC;
}

bool diskconv_fdi(const uint8_t *src, const uint32_t len, uint8_t *trd,
                  const uint32_t trd_max, uint32_t *out_len) {
    if (len < 14 || memcmp(src, "FDI", 3) != 0) return false;
    const unsigned cyls = rd16(src + 4), heads = rd16(src + 6);
    const uint32_t data_off = rd16(src + 10), extra = rd16(src + 12);
    uint32_t p = 14 + extra;
    uint32_t hi = 0;
    memset(trd, 0, trd_max);
    for (unsigned c = 0; c < cyls; c++) {
        for (unsigned h = 0; h < heads; h++) {
            if (p + 7 > len) return false;
            const uint32_t track_off = rd32(src + p);
            const unsigned nsec = src[p + 6];
            p += 7;
            for (unsigned s = 0; s < nsec; s++) {
                if (p + 7 > len) return false;
                const uint8_t *e = src + p;          // C H R N flags offset(2)
                const uint32_t at = data_off + track_off + rd16(e + 5);
                if (at + SEC <= len && h < 2) place(trd, trd_max, &hi, e[0], h, e[2], e[3], src + at);
                p += 7;
            }
        }
    }
    *out_len = hi;
    return hi != 0;
}

bool diskconv_udi(const uint8_t *src, const uint32_t len, uint8_t *trd,
                  const uint32_t trd_max, uint32_t *out_len) {
    if (len < 16 || memcmp(src, "UDI!", 4) != 0) return false;
    const unsigned cyls = src[9] + 1u, heads = src[10] + 1u;
    uint32_t p = 16 + rd32(src + 12);
    uint32_t hi = 0;
    memset(trd, 0, trd_max);
    for (unsigned c = 0; c < cyls; c++) {
        for (unsigned h = 0; h < heads; h++) {
            if (p + 3 > len) goto done;
            const unsigned type = src[p];
            const uint32_t tlen = rd16(src + p + 1);
            const uint8_t *raw = src + p + 3;
            const uint32_t clk = (tlen + 7u) / 8u;     // the sync-mark bitmap
            if (type != 0 || p + 3 + tlen + clk > len) { p += 3 + tlen + clk; continue; }
            /*
             * An MFM track as bytes: find each ID mark (A1 A1 A1 FE) and
             * the data mark after it (A1 A1 A1 FB or F8); the ID's N says
             * how much data follows.
             */
            for (uint32_t i = 3; i + 7 < tlen; i++) {
                if (raw[i] != 0xFE || raw[i - 1] != 0xA1 || raw[i - 2] != 0xA1 || raw[i - 3] != 0xA1)
                    continue;
                const unsigned ic = raw[i + 1], ir = raw[i + 3], in = raw[i + 4];
                for (uint32_t j = i + 7; j + 4 < tlen && j < i + 60; j++) {
                    if ((raw[j] == 0xFB || raw[j] == 0xF8) && raw[j - 1] == 0xA1 && raw[j - 2] == 0xA1) {
                        if (j + 1 + SEC <= tlen && h < 2) place(trd, trd_max, &hi, ic, h, ir, in, raw + j + 1);
                        break;
                    }
                }
            }
            p += 3 + tlen + clk;
        }
    }
done:
    *out_len = hi;
    return hi != 0;
}
