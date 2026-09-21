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
#include "tap.h"

static uint8_t reference[TAPE_MAX_BYTES];
static uint32_t reference_len;

static uint32_t slurp_into_image(const char *p) {
    FILE *f = fopen(p, "rb");
    if (!f) { printf("  FAIL  cannot open %s\n", p); return 0; }
    const size_t n = fread(tape_image, 1, TAPE_MAX_BYTES, f);
    fclose(f);
    return (uint32_t)n;
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

    return tap_done("tape");
}
