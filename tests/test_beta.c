/*
 * The Beta Disk interface: a WD1793 over a .trd.
 *
 * Driven through the real ports, with the real register decode -- which
 * is looser than the port names suggest. The interface looks at A0, A1
 * and A7-A5 only, so #1F is just the name: software uses the aliases,
 * and so does this.
 *
 * The side bit in the system register is INVERTED -- zero selects side
 * one -- so the fixture identifies every sector by track, side and
 * number, and a side that comes back wrong shows as wrong data rather
 * than as a plausible read of the wrong half of the disk.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "beta.h"
#include "spectrum.h"
#include "settings.h"
#include "tap.h"

#define CMD  0x1F       /* command / status */
#define TRK  0x3F
#define SEC  0x5F
#define DAT  0x7F
#define SYS  0xFF

#define ST_BUSY     0x01
#define ST_DRQ      0x02
#define ST_NOTFOUND 0x10

static void out(const uint16_t p, const uint8_t v) { beta_port_out(p, v); }
static uint8_t in(const uint16_t p) { uint8_t v = 0xAA; beta_port_in(p, &v); return v; }

/* Side 0 or 1, through the inverted bit, with /RESET left high. */
static void set_side(const unsigned side) { out(SYS, (uint8_t)(0x04 | ((side ^ 1u) << 4))); }

static int read_sector(const uint8_t t, const unsigned side, const uint8_t s,
                       uint8_t *buf) {
    set_side(side);
    out(TRK, t); out(SEC, s);
    out(CMD, 0x80);                        /* read sector */
    if (in(CMD) & ST_NOTFOUND) return -1;
    int n = 0;
    while ((in(CMD) & ST_DRQ) && n < 256) buf[n++] = in(DAT);
    return n;
}

static int write_sector(const uint8_t t, const unsigned side, const uint8_t s,
                        const uint8_t *buf) {
    set_side(side);
    out(TRK, t); out(SEC, s);
    out(CMD, 0xA0);                        /* write sector */
    if (in(CMD) & ST_NOTFOUND) return -1;
    int n = 0;
    while ((in(CMD) & ST_DRQ) && n < 256) out(DAT, buf[n++]);
    return n;
}

static uint32_t load(const char *p) {
    FILE *f = fopen(p, "rb");
    if (!f) { printf("  FAIL  cannot open %s\n", p); exit(2); }
    const size_t n = fread(trd_image, 1, TRD_MAX_BYTES, f);
    fclose(f);
    return (uint32_t)n;
}

int main(void) {
    char d[128];
    uint8_t buf[256];

    memset(&settings, 0, sizeof settings);
    settings.betadisk = true;
    spec_machine = MACHINE_PENTAGON;

    /* ---------------------------------------------------------------- */
    GROUP("ports");
    beta_set_rom_paged(false);
    uint8_t v = 0;
    CHECK(!beta_port_out(CMD, 0), "silent while the ROM is out");
    CHECK(!beta_port_in(CMD, &v), "and answers nothing");
    beta_set_rom_paged(true);
    CHECK(beta_port_out(CMD, 0xD0), "ours once the ROM is in");
    /* A0 and A1 both set is what selects it; #1F is only the usual name. */
    CHECK(beta_port_out(0x3F, 0), "#3F selects the track register");
    CHECK(!beta_port_out(0x1C, 0), "A0/A1 clear is not the interface");
    CHECK(!beta_port_out(0x1D, 0), "A0 alone is not either");
    CHECK(beta_port_out(0x7B, 0), "#7B is an alias the real hardware has too");

    /* ---------------------------------------------------------------- */
    GROUP("mount");
    uint32_t len = load("tests/fixtures/disk.trd");
    EQ(len, 327680, "fixture length");
    CHECK(beta_mount(len, "tests/fixtures/disk.trd", d, sizeof d), "mount: %s", d);
    CHECK(beta_mounted(), "mounted");
    EQ(beta_tracks(), 40, "forty tracks");
    beta_reset();
    beta_set_rom_paged(true);

    /* ---------------------------------------------------------------- */
    GROUP("seek");
    out(CMD, 0x00);                        /* restore */
    EQ(in(TRK), 0, "restore puts the head at track 0");
    out(DAT, 17); out(CMD, 0x10);          /* seek to the data register */
    EQ(in(TRK), 17, "seek goes where it was told");
    out(CMD, 0x50);                        /* step in */
    EQ(in(TRK), 18, "step in");
    out(CMD, 0x70);                        /* step out */
    EQ(in(TRK), 17, "step out");

    /* ---------------------------------------------------------------- */
    GROUP("read");
    /* Every track, both sides, first and last sector. */
    unsigned wrong = 0, short_read = 0;
    for (unsigned t = 0; t < 40; t++) {
        for (unsigned side = 0; side < 2; side++) {
            for (unsigned s = 1; s <= 16; s += 15) {
                const int n = read_sector((uint8_t)t, side, (uint8_t)s, buf);
                if (n != 256) { short_read++; continue; }
                const uint8_t want = (uint8_t)((t * 32 + side * 16 + s) & 0xFF);
                for (int i = 0; i < 256; i++) if (buf[i] != want) { wrong++; break; }
            }
        }
    }
    EQ(short_read, 0, "every sector read 256 bytes");
    EQ(wrong, 0, "every sector held the data for its own track and side");

    /* Sector 0 does not exist on a TR-DOS disk: they are numbered 1-16. */
    EQ(read_sector(0, 0, 0, buf), -1, "sector 0 is not found");
    EQ(read_sector(0, 0, 17, buf), -1, "sector 17 is not found");
    EQ(read_sector(200, 0, 1, buf), -1, "a track past the end is not found");

    /* ---------------------------------------------------------------- */
    GROUP("write-back");
    len = load("tests/fixtures/disk.trd");
    CHECK(beta_mount(len, "tests/fixtures/scratch.trd", d, sizeof d), "mount: %s", d);
    beta_reset(); beta_set_rom_paged(true);
    CHECK(!beta_dirty(), "a freshly mounted disk is clean");

    uint8_t pattern[256];
    for (int i = 0; i < 256; i++) pattern[i] = (uint8_t)(0x3C ^ (i * 7));
    EQ(write_sector(11, 1, 5, pattern), 256, "write sector moved 256 bytes");
    CHECK(beta_dirty(), "writing marks the image dirty");
    EQ(read_sector(11, 1, 5, buf), 256, "read back length");
    EQ(memcmp(buf, pattern, 256), 0, "read back matches");
    /* The neighbouring sector, and the same sector on the other side,
     * must be untouched -- the inverted side bit makes that the likely
     * place for a write to land wrong. */
    EQ(read_sector(11, 1, 6, buf), 256, "neighbour length");
    EQ(buf[0], (uint8_t)((11 * 32 + 16 + 6) & 0xFF), "neighbouring sector untouched");
    EQ(read_sector(11, 0, 5, buf), 256, "other side length");
    EQ(buf[0], (uint8_t)((11 * 32 + 0 + 5) & 0xFF), "the same sector on side 0 is untouched");

    /* Write protection must actually protect. */
    settings.disk_wrprot = true;
    uint8_t other[256]; memset(other, 0x99, sizeof other);
    write_sector(12, 0, 3, other);
    EQ(read_sector(12, 0, 3, buf), 256, "read after a protected write");
    EQ(buf[0], (uint8_t)((12 * 32 + 3) & 0xFF), "a write-protected disk kept its data");
    settings.disk_wrprot = false;

    CHECK(beta_save(d, sizeof d), "save: %s", d);
    CHECK(!beta_dirty(), "saving clears the dirty flag");
    beta_eject();
    CHECK(!beta_mounted(), "eject");

    memset(trd_image, 0, 65536);
    len = load("tests/fixtures/scratch.trd");
    CHECK(beta_mount(len, "tests/fixtures/scratch.trd", d, sizeof d), "remount: %s", d);
    beta_reset(); beta_set_rom_paged(true);
    EQ(read_sector(11, 1, 5, buf), 256, "reread length");
    EQ(memcmp(buf, pattern, 256), 0, "the write survived the round trip to disk");
    remove("tests/fixtures/scratch.trd");

    /* ---------------------------------------------------------------- */
    GROUP("boot request");
    beta_boot_next_reset();
    CHECK(beta_take_boot_request(), "a boot request is taken once");
    CHECK(!beta_take_boot_request(), "and only once");

    /* ---------------------------------------------------------------- */
    GROUP("drives B-D, from the card");
    {
        // Four cylinders, every byte saying where it is: cylinder*16 + side*8 + sector.
        enum { CYL = 2 * 16 * 256 };
        static uint8_t img[4 * CYL];
        for (int c = 0; c < 4; c++)
            for (int sd = 0; sd < 2; sd++)
                for (int sec = 0; sec < 16; sec++)
                    memset(&img[c * CYL + (sd * 16 + sec) * 256], c * 16 + sd * 8 + sec, 256);
        FILE *f = fopen("tests/out/driveb.trd", "wb");
        fwrite(img, 1, sizeof img, f); fclose(f);

        beta_set_rom_paged(true);
        out(SYS, 0x04 | 0x10 | 0x02);                 // drive C, side 0: nothing in it
        out(CMD, 0x00);                               // restore
        CHECK(in(CMD) & 0x80, "an empty drive C is not ready");

        CHECK(beta_mount_drive(1, "/tests/out/driveb.trd", d, sizeof d), "a .trd goes into drive B (%s)", d);
        out(SYS, 0x04 | 0x10 | 0x01);                 // drive B, side 0
        out(TRK, 2); out(SEC, 5);
        out(CMD, 0x80);                               // read sector: cylinder 2 is not in yet
        CHECK(in(CMD) & ST_BUSY, "held, BUSY, while core 0 fetches the cylinder");
        CHECK(!(in(CMD) & ST_DRQ), "and no data offered yet");
        beta_task();                                  // core 0
        int n = 0, good = 0;
        while ((in(CMD) & ST_DRQ) && n < 256) { if (in(DAT) == 2 * 16 + 0 * 8 + 4) good++; n++; }
        EQ(n, 256, "then the whole sector comes");
        EQ(good, 256, "and it is cylinder 2, side 0, sector 5");

        out(SYS, 0x04 | 0x00 | 0x01);                 // side 1
        out(SEC, 1);
        out(CMD, 0xA0);                               // write sector 1, side 1 -- same cylinder, no wait
        n = 0;
        while ((in(CMD) & ST_DRQ) && n < 256) { out(DAT, 0x5A); n++; }
        EQ(n, 256, "a write goes straight into the cached cylinder");
        out(TRK, 3); out(SEC, 1);
        out(CMD, 0x80);                               // another cylinder: the change goes back first
        beta_task();
        f = fopen("tests/out/driveb.trd", "rb");
        fseek(f, 2 * CYL + (16 + 0) * 256, SEEK_SET);
        uint8_t back[4] = {0};
        fread(back, 1, 4, f); fclose(f);
        EQ(back[0], 0x5A, "and is on the card when the head moves on");
        while (in(CMD) & ST_DRQ) in(DAT);
        beta_eject_drive(1);
        CHECK(!beta_drive_mounted(1), "drive B ejects");
        beta_set_rom_paged(false);
    }

    return tap_done("beta");
}
