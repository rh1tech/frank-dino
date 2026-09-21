/*
 * The container formats: .zip archives and Timex .dck cartridges.
 *
 * Both are read straight off the host filesystem through the FatFs
 * shim, so what is exercised is the real parser against a real file.
 * The zip fixture deliberately holds one stored member, one deflated
 * member large enough that deflate is doing real work, and one member
 * in a format the firmware does not load -- which must be listed out,
 * not listed wrong.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "zip.h"
#include "dck.h"
#include "storage.h"
#include "spectrum.h"
#include "tap.h"

static uint8_t expect[256 * 1024];

static uint32_t slurp(const char *p, uint8_t *dst, const uint32_t max) {
    FILE *f = fopen(p, "rb");
    if (!f) return 0;
    const size_t n = fread(dst, 1, max, f);
    fclose(f);
    return (uint32_t)n;
}

int main(void) {
    char d[128];

    /* ---------------------------------------------------------------- */
    GROUP("zip listing");
    zip_member_t m[8];
    const int n = zip_list("tests/fixtures/games.zip", m, 8);
    EQ(n, 2, "two loadable members of three");
    /* readme.txt is in the archive and must not be offered. */
    for (int i = 0; i < n; i++)
        CHECK(strcmp(m[i].name, "readme.txt") != 0, "readme.txt not offered");

    const zip_member_t *tap = NULL, *z80 = NULL;
    for (int i = 0; i < n; i++) {
        if (!strcmp(m[i].name, "small.tap")) tap = &m[i];
        if (!strcmp(m[i].name, "big.z80"))   z80 = &m[i];
    }
    CHECK(tap && z80, "both loadable members found");
    if (tap) { EQ(tap->kind, FILE_TAP, "small.tap kind"); EQ(tap->size, 3072, "small.tap size"); }
    if (z80) { EQ(z80->kind, FILE_Z80, "big.z80 kind"); EQ(z80->size, 180 * 1024, "big.z80 size"); }

    CHECK(zip_list("tests/fixtures/cart.dck", m, 8) == -1, "a non-zip is refused");

    /* ---------------------------------------------------------------- */
    GROUP("zip extract");
    /* Stored. */
    if (tap) {
        CHECK(zip_extract("tests/fixtures/games.zip", tap, d, sizeof d), "stored: %s", d);
        const uint32_t got = slurp(&ZIP_TEMP_PATH[1], expect, sizeof expect);
        EQ(got, 3072, "stored member length");
        uint8_t want[3072];
        slurp("tests/fixtures/small.tap.expected", want, sizeof want);
        EQ(memcmp(expect, want, 3072), 0, "stored member is byte-for-byte");
    }
    /* Deflate, 180 KB of it. */
    if (z80) {
        CHECK(zip_extract("tests/fixtures/games.zip", z80, d, sizeof d), "deflate: %s", d);
        const uint32_t got = slurp(&ZIP_TEMP_PATH[1], expect, sizeof expect);
        EQ(got, 180 * 1024, "deflated member length");
        static uint8_t want[256 * 1024];
        slurp("tests/fixtures/big.z80.expected", want, sizeof want);
        EQ(memcmp(expect, want, 180 * 1024), 0, "deflated member is byte-for-byte");
    }
    remove(&ZIP_TEMP_PATH[1]);

    /* ---------------------------------------------------------------- */
    GROUP("dck cartridge");
    spec_machine = MACHINE_TIMEX;
    CHECK(dck_mount("tests/fixtures/cart.dck", d, sizeof d), "mount: %s", d);
    CHECK(dck_mounted(), "mounted");
    /*
     * The fixture: 0,1 ROM with data; 3 RAM with data; 4 RAM the file
     * does not carry; 2,5,6,7 absent. Chunk 4 is the one that matters
     * most -- it occupies no bytes in the file, so if its 8 KB were
     * counted in the stride every chunk after it would be misread.
     */
    EQ(dck_present(), 0x1B, "chunks 0, 1, 3 and 4 present");
    EQ(dck_writable(0) | dck_writable(1), 0, "ROM chunks are read-only");
    CHECK(dck_writable(3) && dck_writable(4), "both RAM chunks are writable");
    CHECK(!dck_writable(5), "an absent chunk is not writable");
    for (unsigned i = 0; i < 8; i++) {
        const uint8_t *c = dck_chunk(i);
        CHECK(c != NULL, "chunk %u has a page", i);
        if (i == 0 || i == 1 || i == 3) {
            EQ(c[0], 0xC0 + i, "chunk %u first byte", i);
            EQ(c[8191], 0xC0 + i, "chunk %u last byte", i);
        } else if (i == 4) {
            /* Present, writable, and empty: the file carries nothing. */
            EQ(c[0], 0x00, "uncarried RAM chunk starts empty");
            EQ(c[8191], 0x00, "uncarried RAM chunk is empty throughout");
        } else {
            /* An empty socket reads as an open bus. */
            EQ(c[0], 0xFF, "absent chunk %u reads 0xFF", i);
            EQ(c[8191], 0xFF, "absent chunk %u reads 0xFF throughout", i);
        }
    }

    dck_eject();
    CHECK(!dck_mounted(), "eject");
    EQ(dck_present(), 0, "nothing present after eject");

    CHECK(!dck_mount("tests/fixtures/games.zip", d, sizeof d), "a non-cartridge is refused");

    return tap_done("media");
}
