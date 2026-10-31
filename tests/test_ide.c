/*
 * The hard disk, through the NEMO interface's ports.
 *
 * An image is made with every sector holding its own number, mounted,
 * and then driven the way a ROM would: IDENTIFY, a read by CHS, a read
 * by LBA, a write and a read back.
 */
#include <stdio.h>
#include <string.h>

#include "ide.h"
#include "settings.h"
#include "tap.h"

static void out(uint8_t lo, uint8_t v) { ide_port_out(lo, v); }
static uint8_t in(uint8_t lo) { uint8_t v = 0; ide_port_in(lo, &v); return v; }

/* NEMO: register r at (r << 5) | 0x10, the high byte through #11. */
#define R(r) (uint8_t)(((r) << 5) | 0x10)

static void read_sector(uint8_t *b) {
    for (int i = 0; i < 512; i += 2) { b[i] = in(R(0)); b[i + 1] = in(0x11); }
}

int main(void) {
    const char *path = "tests/out/disk.img";
    FILE *f = fopen(path, "wb");
    for (unsigned s = 0; s < 2048; s++) {
        uint8_t b[512];
        memset(b, (uint8_t)s, sizeof b);
        b[0] = (uint8_t)s; b[1] = (uint8_t)(s >> 8);
        fwrite(b, 1, 512, f);
    }
    fclose(f);

    settings.ide = IDE_NEMO;
    char d[64];
    GROUP("mount");
    CHECK(ide_mount(path, d, sizeof d), "mounts a raw image");
    ide_reset();
    EQ(in(R(7)) & 0x40, 0x40, "the drive is ready");

    GROUP("identify");
    uint8_t b[512];
    out(R(6), 0xA0);
    out(R(7), 0xEC);
    EQ(in(R(7)) & 0x08, 0x08, "data is waiting");
    read_sector(b);
    EQ(b[12], 63, "sectors per track in word 6");
    EQ((unsigned)(b[120] | (b[121] << 8)), 2048u, "and the LBA size in word 60");

    GROUP("read");
    out(R(6), 0xE0);                           // LBA, master
    out(R(3), 100); out(R(4), 0); out(R(5), 0); out(R(2), 1);
    out(R(7), 0x20);
    read_sector(b);
    EQ((unsigned)(b[0] | (b[1] << 8)), 100u, "LBA 100 is sector 100");
    out(R(6), 0xA0 | 1);                       // CHS: cylinder 1, head 1, sector 1
    out(R(3), 1); out(R(4), 1); out(R(5), 0); out(R(2), 2);
    out(R(7), 0x20);
    read_sector(b);
    EQ((unsigned)(b[0] | (b[1] << 8)), (1u * 16u + 1u) * 63u, "CHS 1/1/1 by the geometry");
    read_sector(b);
    EQ((unsigned)(b[0] | (b[1] << 8)), (1u * 16u + 1u) * 63u + 1u, "a two-sector read runs on");

    GROUP("write");
    out(R(6), 0xE0);
    out(R(3), 7); out(R(4), 0); out(R(5), 0); out(R(2), 1);
    out(R(7), 0x30);
    for (int i = 0; i < 512; i += 2) { out(0x11, 0xBE); out(R(0), 0xEF); }
    out(R(3), 7); out(R(2), 1);
    out(R(7), 0x20);
    read_sector(b);
    EQ(b[0], 0xEF, "the written low byte");
    EQ(b[1], 0xBE, "and the high one through the latch");

    ide_eject();
    return tap_done("ide");
}
