/*
 * FDI and UDI images of a TR-DOS disk, unwrapped into a .trd.
 *
 * Each image is built here with every sector holding its own address, so
 * a sector that lands in the wrong place says where it came from.
 */
#include <stdio.h>
#include <string.h>

#include "diskconv.h"
#include "tap.h"

static uint8_t img[200000], trd[655360];

static void fill(uint8_t *p, unsigned cyl, unsigned head, unsigned r) {
    for (int i = 0; i < 256; i++) p[i] = (uint8_t)(cyl * 7 + head * 3 + r + i);
}

static int check(unsigned cyls) {
    int bad = 0;
    uint8_t want[256];
    for (unsigned c = 0; c < cyls; c++)
        for (unsigned h = 0; h < 2; h++)
            for (unsigned r = 1; r <= 16; r++) {
                fill(want, c, h, r);
                if (memcmp(trd + ((c * 2 + h) * 16 + r - 1) * 256, want, 256)) bad++;
            }
    return bad;
}

int main(void) {
    GROUP("fdi");
    {
        // Two cylinders, two heads, sixteen 256-byte sectors, in reverse order.
        uint32_t n = 14;
        memcpy(img, "FDI", 3);
        img[4] = 2; img[6] = 2;
        const uint32_t hdr_end = 14 + 4 * (7 + 16 * 7);
        img[10] = (uint8_t)hdr_end; img[11] = (uint8_t)(hdr_end >> 8);
        uint32_t track_off = 0;
        for (unsigned c = 0; c < 2; c++) for (unsigned h = 0; h < 2; h++) {
            img[n] = (uint8_t)track_off; img[n + 1] = (uint8_t)(track_off >> 8);
            img[n + 2] = (uint8_t)(track_off >> 16); img[n + 3] = 0;
            img[n + 6] = 16; n += 7;
            for (unsigned s = 0; s < 16; s++) {
                const unsigned r = 16 - s;
                img[n] = (uint8_t)c; img[n + 1] = (uint8_t)h; img[n + 2] = (uint8_t)r; img[n + 3] = 1;
                img[n + 5] = (uint8_t)(s * 256); img[n + 6] = (uint8_t)((s * 256) >> 8);
                fill(img + hdr_end + track_off + s * 256, c, h, r);
                n += 7;
            }
            track_off += 16 * 256;
        }
        uint32_t len = 0;
        CHECK(diskconv_fdi(img, hdr_end + track_off, trd, sizeof trd, &len), "converts");
        EQ(len, 4u * 16u * 256u, "four tracks of sectors");
        EQ(check(2), 0, "every sector in its place");
    }

    GROUP("udi");
    {
        memset(img, 0x4E, sizeof img);
        memcpy(img, "UDI!", 4);
        img[9] = 1; img[10] = 1;                      // cylinders 0-1, heads 0-1
        img[12] = img[13] = img[14] = img[15] = 0;
        uint32_t p = 16;
        for (unsigned c = 0; c < 2; c++) for (unsigned h = 0; h < 2; h++) {
            const uint32_t tlen = 6250;
            img[p] = 0; img[p + 1] = (uint8_t)tlen; img[p + 2] = (uint8_t)(tlen >> 8);
            uint8_t *t = img + p + 3;
            uint32_t at = 60;
            for (unsigned s = 0; s < 16; s++) {
                const unsigned r = (s * 5) % 16 + 1;   // interleaved
                t[at] = t[at + 1] = t[at + 2] = 0xA1; t[at + 3] = 0xFE;
                t[at + 4] = (uint8_t)c; t[at + 5] = (uint8_t)h; t[at + 6] = (uint8_t)r; t[at + 7] = 1;
                at += 10 + 22;
                t[at] = t[at + 1] = t[at + 2] = 0xA1; t[at + 3] = 0xFB;
                fill(t + at + 4, c, h, r);
                at += 4 + 256 + 2 + 54;
            }
            p += 3 + tlen + (tlen + 7) / 8;
        }
        uint32_t len = 0;
        CHECK(diskconv_udi(img, p, trd, sizeof trd, &len), "converts");
        EQ(check(2), 0, "every sector from the raw track in its place");
    }

    return tap_done("diskconv");
}
