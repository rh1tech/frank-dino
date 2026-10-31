/*
 * frank-dino — an RP2350B acting as the whole machine around a real Z80
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-dino
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Teledisk record layout as UnrealSpeccy's wldr_td0.cpp and pico-speccy's
 * wd1793.cpp read it; the LZH and sector decoding is td0.cpp beside this.
 */

#include "diskconv.h"

#include <string.h>

#include "ff.h"
#include "pico/malloc.h"
#include "td0.h"

/* td0.cpp asks before it takes ~9 KB for the LZH tables. */
size_t getContiguousHeap(void) { return 16384; }

namespace {
struct Sink { uint8_t *at; uint32_t left; uint32_t n; };

bool sink_fn(void *ctx, const unsigned char *buf, unsigned len) {
    Sink *s = (Sink *)ctx;
    const unsigned take = len < s->left ? len : s->left;
    memcpy(s->at, buf, take);
    s->at += take; s->left -= take; s->n += take;
    return take == len;
}
}

extern "C" bool diskconv_td0(void *fp, uint8_t *scratch, const uint32_t scratch_max,
                             uint8_t *trd, const uint32_t trd_max, uint32_t *out_len) {
    FIL *f = (FIL *)fp;
    uint8_t h[12];
    UINT got = 0;
    if (f_read(f, h, 12, &got) != FR_OK || got != 12) return false;
    const bool packed = h[0] == 't' && h[1] == 'd';
    if (!packed && !(h[0] == 'T' && h[1] == 'D')) return false;

    // Everything after the header, decompressed or not, into the scratch.
    uint32_t n = 0;
    if (packed) {
        Sink s = { scratch, scratch_max, 0 };
        n = td0_unpack_lzh_from_file(f, sink_fn, &s);
    } else {
        if (f_read(f, scratch, scratch_max, &got) != FR_OK) return false;
        n = got;
    }

    uint32_t p = 0;
    if (h[7] & 0x80u) {                         // a comment block first
        if (p + 10 > n) return false;
        p += 10 + (uint32_t)(scratch[p + 2] | (scratch[p + 3] << 8));
    }

    memset(trd, 0, trd_max);
    uint32_t hi = 0;
    static uint8_t sec[8192];
    while (p + 4 <= n) {
        const unsigned nsec = scratch[p], cyl = scratch[p + 1], head = scratch[p + 2] & 1u;
        p += 4;
        if (nsec == 0xFF) break;
        for (unsigned i = 0; i < nsec && p + 6 <= n; i++) {
            const uint8_t *e = scratch + p;     // C H R N flags crc
            p += 6;
            if (e[4] & (TD0_SEC_NO_DATA | TD0_SEC_NO_DATA2)) continue;
            if (p + 2 > n) break;
            const unsigned dlen = (unsigned)(scratch[p] | (scratch[p + 1] << 8));
            p += 2;
            if (p + dlen > n) break;
            const unsigned size = 128u << (e[3] & 7u);
            if (e[3] == 1 && e[2] >= 1 && e[2] <= 16 && size <= sizeof sec
                && td0_decode_sector(scratch + p, dlen, size, sec)) {
                const uint32_t at = ((cyl * 2u + head) * 16u + (e[2] - 1u)) * 256u;
                if (at + 256u <= trd_max) {
                    memcpy(trd + at, sec, 256);
                    if (at + 256u > hi) hi = at + 256u;
                }
            }
            p += dlen;
        }
    }
    *out_len = hi;
    return hi != 0;
}
