/*
 * The +3's uPD765, driven the way +3DOS drives it.
 *
 * SPECIFY, RECALIBRATE, SENSE INTERRUPT, SEEK, READ DATA, WRITE DATA --
 * through the real fdc_port_in/out, against a generated .dsk whose every
 * sector byte is known, so a wrong track offset shows up as wrong data
 * rather than as a plausible-looking read.
 *
 * Both flavours, because they differ in exactly the thing most likely to
 * be got wrong: an extended image may leave a track unformatted, which
 * takes no space in the file, so track offsets cannot be computed by
 * multiplying.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "fdc.h"
#include "spectrum.h"
#include "tap.h"

#define STATUS 0x2FFD
#define DATA   0x3FFD
#define MS_EXM 0x20        /* execution phase: bytes to move */

static void out(const uint16_t p, const uint8_t v) { fdc_port_out(p, v); }
static uint8_t in(const uint16_t p) { uint8_t v = 0xAA; fdc_port_in(p, &v); return v; }
static void cmd(const uint8_t *b, const int n) { for (int i = 0; i < n; i++) out(DATA, b[i]); }

/* SENSE INTERRUPT, which the ROM issues after every seek. */
static void sense_int(uint8_t *st0, uint8_t *pcn) {
    out(DATA, 0x08); *st0 = in(DATA); *pcn = in(DATA);
}

static void seek(const uint8_t cyl) {
    const uint8_t c[] = { 0x0F, 0x00, cyl }; cmd(c, sizeof c);
    uint8_t st0, pcn; sense_int(&st0, &pcn);
}

/* READ DATA. Returns bytes moved, or -1 if the FDC reported no data. */
static int read_sector(const uint8_t cyl, const uint8_t head, const uint8_t r,
                       uint8_t *buf, const int max, uint8_t *st1_out) {
    const uint8_t c[] = { 0x46, (uint8_t)(head << 2), cyl, head, r, 2, r, 0x2A, 0xFF };
    cmd(c, sizeof c);
    int n = 0;
    while ((in(STATUS) & MS_EXM) && n < max) buf[n++] = in(DATA);
    uint8_t res[7]; for (int i = 0; i < 7; i++) res[i] = in(DATA);
    if (st1_out) *st1_out = res[1];
    return (res[1] & 0x04) ? -1 : n;       /* ST1 bit 2 = no data */
}

/* WRITE DATA. */
static int write_sector(const uint8_t cyl, const uint8_t head, const uint8_t r,
                        const uint8_t *buf, const int len) {
    const uint8_t c[] = { 0x45, (uint8_t)(head << 2), cyl, head, r, 2, r, 0x2A, 0xFF };
    cmd(c, sizeof c);
    int n = 0;
    while ((in(STATUS) & MS_EXM) && n < len) out(DATA, buf[n++]);
    uint8_t res[7]; for (int i = 0; i < 7; i++) res[i] = in(DATA);
    return (res[1] & 0x04) ? -1 : n;
}

static uint32_t load(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { printf("  FAIL  cannot open %s\n", path); exit(2); }
    const size_t n = fread(dsk_image, 1, DSK_MAX_BYTES, f);
    fclose(f);
    return (uint32_t)n;
}

static void ready(void) {
    fdc_reset();
    const uint8_t spec[]  = { 0x03, 0xAF, 0x02 };   cmd(spec, sizeof spec);
    const uint8_t recal[] = { 0x07, 0x00 };         cmd(recal, sizeof recal);
    uint8_t st0, pcn; sense_int(&st0, &pcn);
    EQ(pcn, 0, "recalibrate leaves the head at track 0");
    EQ(st0 & 0x20, 0x20, "ST0 seek-end after recalibrate");
}

int main(void) {
    char d[96];
    uint8_t buf[512];

    spec_machine   = MACHINE_PLUS3;
    spec_port_1ffd = 0x08;                  /* motor on */

    /* ---------------------------------------------------------------- */
    GROUP("standard .dsk");
    uint32_t len = load("tests/fixtures/std.dsk");
    CHECK(fdc_mount(len, "tests/fixtures/std.dsk", d, sizeof d), "mount: %s", d);
    STREQ(d, "40 tracks, 1 side, standard", "geometry");
    EQ(fdc_tracks(), 40, "tracks"); EQ(fdc_sides(), 1, "sides");
    ready();

    const uint8_t st3_cmd[] = { 0x04, 0x00 }; cmd(st3_cmd, sizeof st3_cmd);
    const uint8_t st3 = in(DATA);
    EQ(st3 & 0x20, 0x20, "SENSE DRIVE says ready");
    EQ(st3 & 0x10, 0x10, "SENSE DRIVE says track 0");

    /* Every track, first and last sector, checked against known content. */
    for (unsigned t = 0; t < 40; t++) {
        seek((uint8_t)t);
        for (unsigned s = 0; s < 9; s += 8) {
            const uint8_t r = (uint8_t)(0xC1 + s);
            const int n = read_sector((uint8_t)t, 0, r, buf, sizeof buf, NULL);
            EQ(n, 512, "track %u sector %02X length", t, r);
            const uint8_t want = (uint8_t)((t * 16 + s) & 0xFF);
            unsigned bad = 0;
            for (int i = 0; i < n; i++) if (buf[i] != want) bad++;
            EQ(bad, 0, "track %u sector %02X content (want %02X)", t, r, want);
        }
    }
    /* A sector that is not on the disk must be reported, not invented. */
    seek(5);
    uint8_t st1 = 0;
    EQ(read_sector(5, 0, 0xF0, buf, sizeof buf, &st1), -1, "absent sector fails");
    EQ(st1 & 0x04, 0x04, "ST1 no-data for an absent sector");

    /* ---------------------------------------------------------------- */
    GROUP("extended .dsk");
    len = load("tests/fixtures/ext.dsk");
    CHECK(fdc_mount(len, "tests/fixtures/ext.dsk", d, sizeof d), "mount: %s", d);
    STREQ(d, "40 tracks, 1 side, extended", "geometry");
    ready();
    /* Track 3 is unformatted and occupies no bytes in the file, so every
     * track above it is displaced. Reading 4..39 correctly is the proof
     * that the offsets came from the size table and not from a product. */
    for (unsigned t = 0; t < 40; t++) {
        seek((uint8_t)t);
        const int n = read_sector((uint8_t)t, 0, 0xC1, buf, sizeof buf, &st1);
        if (t == 3) {
            EQ(n, -1, "unformatted track 3 has no sector");
            continue;
        }
        EQ(n, 512, "track %u length", t);
        const uint8_t want = (uint8_t)((t * 16) & 0xFF);
        unsigned bad = 0;
        for (int i = 0; i < n; i++) if (buf[i] != want) bad++;
        EQ(bad, 0, "track %u content past the gap (want %02X)", t, want);
    }

    /* ---------------------------------------------------------------- */
    GROUP("write-back");
    len = load("tests/fixtures/std.dsk");
    CHECK(fdc_mount(len, "tests/fixtures/scratch.dsk", d, sizeof d), "mount: %s", d);
    ready();
    CHECK(!fdc_dirty(), "a freshly mounted disk is clean");

    uint8_t pattern[512];
    for (int i = 0; i < 512; i++) pattern[i] = (uint8_t)(0x5A ^ i);
    seek(7);
    EQ(write_sector(7, 0, 0xC3, pattern, 512), 512, "WRITE DATA moved 512 bytes");
    CHECK(fdc_dirty(), "writing marks the image dirty");

    /* Read it straight back through the FDC. */
    EQ(read_sector(7, 0, 0xC3, buf, sizeof buf, NULL), 512, "read back length");
    EQ(memcmp(buf, pattern, 512), 0, "read back matches what was written");

    /* And the other sectors of that track must be untouched. */
    EQ(read_sector(7, 0, 0xC1, buf, sizeof buf, NULL), 512, "neighbour length");
    unsigned bad = 0;
    for (int i = 0; i < 512; i++) if (buf[i] != 0x70) bad++;
    EQ(bad, 0, "neighbouring sector untouched");

    /* Save, remount from the file, and look again. */
    CHECK(fdc_save(d, sizeof d), "save: %s", d);
    CHECK(!fdc_dirty(), "saving clears the dirty flag");
    fdc_eject();
    CHECK(!fdc_mounted(), "eject");

    memset(dsk_image, 0, 4096);
    len = load("tests/fixtures/scratch.dsk");
    CHECK(fdc_mount(len, "tests/fixtures/scratch.dsk", d, sizeof d), "remount: %s", d);
    ready();
    seek(7);
    EQ(read_sector(7, 0, 0xC3, buf, sizeof buf, NULL), 512, "reread length");
    EQ(memcmp(buf, pattern, 512), 0, "the write survived the round trip to disk");
    remove("tests/fixtures/scratch.dsk");

    /* ---------------------------------------------------------------- */
    GROUP("rejections");
    memcpy(dsk_image, "not a disk at all, not even close", 33);
    CHECK(!fdc_mount(4096, "x", d, sizeof d), "a non-disk is refused");
    STREQ(d, "that is not a .dsk", "and says so");
    CHECK(!fdc_mount(8, "x", d, sizeof d), "a stub file is refused");

    /* The FDC is the +3's alone; nothing else may answer its ports. */
    spec_machine = MACHINE_128K;
    uint8_t v = 0;
    CHECK(!fdc_port_in(STATUS, &v), "no FDC on a 128K");
    CHECK(!fdc_port_out(DATA, 0), "no FDC writes on a 128K");
    spec_machine = MACHINE_PLUS3;
    CHECK(fdc_port_in(STATUS, &v), "and it is back on a +3");

    return tap_done("fdc");
}
