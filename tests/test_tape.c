/*
 * The three tape containers, which must all arrive at the same bytes.
 *
 * The fixtures hold one header block and one data block, written once
 * as .tap, once as .tzx and once as .pzx. A ROM trap loads the block
 * CONTENTS, so a container that parses without error but hands over a
 * byte too many is a loader that fails a checksum halfway through a
 * game -- which is why this compares against the .tap byte for byte
 * rather than just counting blocks.
 */
#include <stdio.h>
#include <string.h>

#include "tape.h"
#include "spectrum.h"
#include "z80_bus.h"
#include "tap.h"

extern bool stub_beta_paged;

static uint8_t reference[TAPE_MAX_BYTES];
static uint32_t reference_len;

static uint32_t slurp_into_image(const char *p) {
    FILE *f = fopen(p, "rb");
    if (!f) { printf("  FAIL  cannot open %s\n", p); return 0; }
    const size_t n = fread(tape_image, 1, TAPE_MAX_BYTES, f);
    fclose(f);
    return (uint32_t)n;
}

/*
 * Be the stub: hand over A, F, IXh, IXl, Dh, El and read the answer.
 * `load` is the carry flag the ROM enters with -- set for LOAD, clear
 * for VERIFY.
 */
static uint8_t load_block(const uint8_t type, const bool load,
                          const uint16_t dest, const uint16_t count) {
    tape_port_out(0x7B, type);
    tape_port_out(0x7B, (uint8_t)(load ? 0x01 : 0x00));
    tape_port_out(0x7B, (uint8_t)(dest >> 8));
    tape_port_out(0x7B, (uint8_t)(dest & 0xFF));
    tape_port_out(0x7B, (uint8_t)(count >> 8));
    tape_port_out(0x7B, (uint8_t)(count & 0xFF));
    uint8_t ok = 0xAA;
    tape_port_in(0x7B, &ok);
    return ok;
}

int main(void) {
    char d[128];

    spec_machine = MACHINE_48K;

    /* ---------------------------------------------------------------- */
    GROUP(".tap");
    uint32_t len = slurp_into_image("tests/fixtures/tape.tap");
    EQ(len, 281, "fixture length");
    CHECK(tape_mount(tape_image, len, d, sizeof d), "mount: %s", d);
    CHECK(tape_mounted(), "mounted");
    EQ(tape_block_count(), 2, "two blocks");
    EQ(tape_block_index(), 0, "rewound");
    /* Keep this as the reference the other two must reproduce. */
    reference_len = len;
    memcpy(reference, tape_image, len);

    /* ---------------------------------------------------------------- */
    GROUP(".tzx");
    tape_eject();
    CHECK(!tape_mounted(), "eject");
    len = slurp_into_image("tests/fixtures/tape.tzx");
    CHECK(tape_mount_tzx(len, d, sizeof d), "mount: %s", d);
    EQ(tape_block_count(), 2, "two blocks");
    /* A .tzx of standard blocks converts to exactly the .tap. */
    EQ(memcmp(tape_image, reference, reference_len), 0,
       "converts to the same bytes as the .tap");

    /* ---------------------------------------------------------------- */
    GROUP(".pzx");
    tape_eject();
    len = slurp_into_image("tests/fixtures/tape.pzx");
    CHECK(tape_mount_pzx(len, d, sizeof d), "mount: %s", d);
    EQ(tape_block_count(), 2, "two blocks -- the ragged one was dropped");
    EQ(memcmp(tape_image, reference, reference_len), 0,
       "converts to the same bytes as the .tap");
    /* Checksums intact: a block's last byte is the XOR of the rest. */
    uint32_t at = 0;
    for (unsigned b = 0; b < 2; b++) {
        const uint32_t n = (uint32_t)tape_image[at] | ((uint32_t)tape_image[at+1] << 8);
        uint8_t chk = 0;
        for (uint32_t i = 0; i < n - 1; i++) chk ^= tape_image[at + 2 + i];
        EQ(tape_image[at + 2 + n - 1], chk, "block %u checksum is intact", b);
        at += 2 + n;
    }
    EQ(at, reference_len, "the blocks account for the whole image");

    /* ---------------------------------------------------------------- */
    GROUP("rejections");
    tape_eject();
    memcpy(tape_image, "this is not a tape image at all", 31);
    CHECK(!tape_mount_pzx(31, d, sizeof d), "a non-pzx is refused");
    STREQ(d, "that is not a .pzx", "and says so");
    CHECK(!tape_mount_tzx(31, d, sizeof d), "a non-tzx is refused");
    CHECK(!tape_mount_pzx(4, d, sizeof d), "a stub file is refused");

    /* A .pzx with no ROM-loadable block is a custom loader, and has to
     * say so rather than mounting an empty tape. */
    const uint8_t custom[] = {
        'P','Z','X','T', 2,0,0,0, 1, 0,
        'P','U','L','S', 4,0,0,0, 0x00,0x10, 0xF4,0x01,
        'S','T','O','P', 2,0,0,0, 0,0,
    };
    memcpy(tape_image, custom, sizeof custom);
    CHECK(!tape_mount_pzx(sizeof custom, d, sizeof d), "a custom-loader pzx is refused");
    CHECK(strstr(d, "custom loader") != NULL, "and explains why: %s", d);

    /* ---------------------------------------------------------------- */
    /*
     * Actually loading, which is the point of all of the above.
     *
     * The trap is a stub over LD-BYTES at 0x0556 that hands the ROM's
     * six registers out through port 0x7B and reads back whether it
     * worked. So the way to test loading without a Z80 is to BE the
     * stub: emit that sequence and look at where the bytes landed.
     *
     * The register order is the protocol -- the host counts writes
     * rather than inspecting them -- so a test that gets the order
     * wrong is indistinguishable from a firmware that does.
     */
    GROUP("loading");
    for (unsigned i = 0; i < 4; i++) z80_map_page(i, spec_ram[i], spec_ram[i]);

    tape_eject();
    len = slurp_into_image("tests/fixtures/tape.tap");
    CHECK(tape_mount(tape_image, len, d, sizeof d), "mount: %s", d);

    /* The stub must be in the ROM window, or nothing else can happen. */
    const uint8_t *rom = spec_basic_slot();
    EQ(rom[0x0556], 0xF3, "the trap is installed: DI");
    EQ(rom[0x0557], 0xD3, "OUT (n),A");
    EQ(rom[0x0558], 0x7B, "...on the tape port");
    EQ(rom[0x0571], 0xC9, "and it returns");

    /* The ROM's header block: type 0x00, seventeen bytes. */
    EQ(load_block(0x00, true, 0x8000, 17), 1, "header block loads");
    EQ(z80_peek(0x8000), 0x00, "header: type byte");
    char name[11] = {0};
    for (int i = 0; i < 10; i++) name[i] = (char)z80_peek((uint16_t)(0x8001 + i));
    STREQ(name, "testtape  ", "header: the program name");
    EQ(z80_peek(0x800B) | (z80_peek(0x800C) << 8), 256, "header: length");

    /* Then the data block: type 0xFF, 256 bytes. */
    EQ(load_block(0xFF, true, 0x9000, 256), 1, "data block loads");
    unsigned bad = 0;
    for (unsigned i = 0; i < 256; i++)
        if (z80_peek((uint16_t)(0x9000 + i)) != (uint8_t)((i * 11) & 0xFF)) bad++;
    EQ(bad, 0, "every byte of the data block arrived");

    /* Past the end, the tape rewinds -- a real one would have to be
     * rewound too, but a file has nowhere else to go. */
    EQ(load_block(0x00, true, 0x8000, 17), 1, "the tape wraps to the start");

    /* VERIFY: carry clear means read and check without storing. */
    for (unsigned i = 0; i < 256; i++) z80_poke((uint16_t)(0xA000 + i), 0x5A);
    EQ(load_block(0xFF, false, 0xA000, 256), 1, "a VERIFY succeeds");
    unsigned touched = 0;
    for (unsigned i = 0; i < 256; i++)
        if (z80_peek((uint16_t)(0xA000 + i)) != 0x5A) touched++;
    EQ(touched, 0, "and stores nothing");

    /* A type mismatch is how the ROM skips blocks it does not want: the
     * call fails and the tape advances anyway. */
    EQ(load_block(0xFF, true, 0x8000, 17), 0, "asking for data at a header fails");
    EQ(load_block(0xFF, true, 0x9000, 256), 1, "and the tape moved on to the data");

    /* Asking for more than the block holds must fail rather than
     * running off the end of it. */
    EQ(load_block(0x00, true, 0x8000, 4096), 0, "an over-long request fails");

    /*
     * The tape port collides with the Beta Disk by construction: A0 and
     * A1 both set is what selects that interface, and #7B's A7-A5 land
     * on its data register -- the same one #7F reaches. The two are
     * separated by state, not address, so with TR-DOS paged in this
     * handler has to stand down or a disk write goes to the tape.
     */
    stub_beta_paged = true;
    uint8_t v = 0xAA;
    CHECK(!tape_port_out(0x7B, 0x00), "TR-DOS in: #7B belongs to the WD1793");
    CHECK(!tape_port_in(0x7B, &v),    "TR-DOS in: and so does reading it");
    stub_beta_paged = false;
    CHECK(tape_port_out(0x7B, 0x00),  "TR-DOS out: the tape has it back");
    CHECK(tape_port_in(0x7B, &v),     "TR-DOS out: and can answer");

    /* Nothing answers the port with no tape in. */
    tape_eject();
    CHECK(!tape_port_out(0x7B, 0x00), "no tape: the port is not ours");
    CHECK(!tape_port_in(0x7B, &v),    "no tape: nothing to read");

    return tap_done("tape");
}
