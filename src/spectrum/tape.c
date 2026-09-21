/*
 * frank-dino — an RP2350B acting as the whole machine around a real Z80
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-dino
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "tape.h"

#include <stdio.h>
#include <string.h>

#include "dino.h"
#include "spectrum.h"
#include "z80_bus.h"

uint8_t tape_image[TAPE_MAX_BYTES];

static uint32_t image_len;
static uint32_t blocks;
static uint32_t block_at;       // byte offset of the next block's length word
static uint32_t block_no;
static bool     mounted;

bool     tape_mounted(void)     { return mounted; }
uint32_t tape_block_count(void) { return blocks; }
uint32_t tape_block_index(void) { return block_no; }

// ---------------------------------------------------------------------------
// The patched ROM
// ---------------------------------------------------------------------------
/*
 * A copy of the machine's ROM with twenty bytes changed.
 *
 * Copied rather than served with a compare on every read: the ROM read
 * path is z80_rpage[a>>14][a&0x3FFF] with no branch in it, and putting
 * one there to check for a single address would cost more, on every
 * instruction the machine ever executes, than 16 KB of RAM does once.
 */
static uint8_t patched_rom[SPEC_BANK_SIZE] __attribute__((aligned(4)));

/*
 * The port the stub talks through.
 *
 * Low byte 0x7B, and the value is chosen rather than picked: OUT (n),A
 * puts A on A8-A15, so only the low byte is dependable. 0x7B has A0 set,
 * which keeps it clear of the ULA; A1 set, which keeps it clear of both
 * the 0x7FFD paging latch and the AY's select port; and it is not
 * anything a Spectrum answers. Nothing but the stub can reach it anyway,
 * since the stub only exists inside a ROM page this firmware installed.
 */
#define TAPE_PORT 0x7B

/*
 * The stub, assembled by hand, over LD-BYTES at 0x0556.
 *
 *   0556  F3        DI              ; as the real routine does
 *   0557  D3 7B     OUT (7Bh),A     ; the block type the ROM wants
 *   0559  F5        PUSH AF
 *   055A  E1        POP HL
 *   055B  7D        LD A,L
 *   055C  D3 7B     OUT (7Bh),A     ; F, whose bit 0 is LOAD-not-VERIFY
 *   055E  DD E5     PUSH IX
 *   0560  E1        POP HL
 *   0561  7C        LD A,H
 *   0562  D3 7B     OUT (7Bh),A     ; IX high
 *   0564  7D        LD A,L
 *   0565  D3 7B     OUT (7Bh),A     ; IX low
 *   0567  7A        LD A,D
 *   0568  D3 7B     OUT (7Bh),A     ; DE high
 *   056A  7B        LD A,E
 *   056B  D3 7B     OUT (7Bh),A     ; DE low -- and the host loads the block
 *   056D  DB 7B     IN A,(7Bh)      ; 1 if it worked
 *   056F  1F        RRA             ; carry = bit 0, which is the ROM's answer
 *   0570  FB        EI
 *   0571  C9        RET
 *
 * Twenty-eight bytes, which fit inside LD-BYTES with room to spare: the
 * routine runs to 0x05E2 and nothing outside it jumps into the part being
 * overwritten.
 *
 * Three details that are not free choices:
 *
 * The register order is the protocol. A has to go out first because every
 * instruction after it destroys A, and the host counts writes rather than
 * inspecting them, so the sequence is the only thing identifying which
 * register each byte is.
 *
 * HL is used as scratch and not restored, which is allowed because the
 * real LD-BYTES destroys HL too -- it keeps the running parity byte in H.
 *
 * The DI and EI bracket the whole thing for the same reason the real
 * routine has them: an interrupt arriving between two of these OUTs would
 * run the ROM's keyboard scan in the middle of the handshake. That is
 * harmless here, because the state machine lives in the host and the
 * handler touches none of it -- but the real routine returns with
 * interrupts enabled, and matching it is one less difference to remember.
 */
#define TRAP_ADDR 0x0556

static const uint8_t stub[] = {
    0xF3,
    0xD3, TAPE_PORT,
    0xF5, 0xE1, 0x7D, 0xD3, TAPE_PORT,
    0xDD, 0xE5, 0xE1, 0x7C, 0xD3, TAPE_PORT,
    0x7D, 0xD3, TAPE_PORT,
    0x7A, 0xD3, TAPE_PORT,
    0x7B, 0xD3, TAPE_PORT,
    0xDB, TAPE_PORT,
    0x1F,
    0xFB,
    0xC9,
};

// What the stub has told us so far, in the order above.
static uint8_t  want_type, want_flags, ix_hi, ix_lo, de_hi, de_lo;
static uint8_t  step;
static bool     last_ok;

// ---------------------------------------------------------------------------

static void install(void) {
    // The 48K ROM is what has LD-BYTES in it: on a 128K that is ROM 1,
    // which is also the one a tape-loading program will have paged in.
    memcpy(patched_rom, spec_rom_48k_image(), SPEC_BANK_SIZE);
    memcpy(&patched_rom[TRAP_ADDR], stub, sizeof stub);
    spec_set_tape_rom(patched_rom);
}

void tape_machine_changed(void) {
    if (mounted) install();
}

bool tape_mount(const uint8_t *image, const uint32_t len,
                char *detail, const unsigned detail_len) {
    if (image != tape_image) {
        if (len > TAPE_MAX_BYTES) {
            snprintf(detail, detail_len, "tape is %luK; the limit is %uK",
                     (unsigned long)(len / 1024), TAPE_MAX_BYTES / 1024);
            return false;
        }
        memcpy(tape_image, image, len);
    }

    // Walk the blocks once, to count them and to reject a file whose
    // length words do not add up -- which is the only structural check a
    // .TAP admits of, and catches a truncated download.
    uint32_t at = 0, n = 0;
    while (at + 2 <= len) {
        const uint32_t blen = (uint32_t)tape_image[at] | ((uint32_t)tape_image[at + 1] << 8);
        if (blen == 0 || at + 2 + blen > len) break;
        at += 2 + blen;
        n++;
    }
    if (n == 0) {
        snprintf(detail, detail_len, "no usable blocks in that .tap");
        return false;
    }

    image_len = len;
    blocks    = n;
    block_at  = 0;
    block_no  = 0;
    step      = 0;
    mounted   = true;
    install();

    snprintf(detail, detail_len, "tape mounted: %lu blocks, %luK",
             (unsigned long)n, (unsigned long)(len / 1024));
    return true;
}

void tape_eject(void) {
    mounted = false;
    spec_set_tape_rom(NULL);
}

// ---------------------------------------------------------------------------
// Serving a block
// ---------------------------------------------------------------------------

static void rewind_if_past_end(void) {
    if (block_at + 2 > image_len) { block_at = 0; block_no = 0; }
}

/*
 * Hand the next block to the address the ROM asked for.
 *
 * The ROM's contract: A is the block type it expects (0x00 for a header,
 * 0xFF for data), DE is how many bytes it wants, IX is where they go, and
 * the carry flag says LOAD rather than VERIFY. Success is carry set.
 *
 * A type mismatch is not an error — it is how the ROM skips past blocks
 * it is not looking for, so the tape advances and the call fails, which
 * is exactly what a real tape would have done.
 */
static bool serve(void) {
    rewind_if_past_end();

    const uint32_t blen = (uint32_t)tape_image[block_at] |
                          ((uint32_t)tape_image[block_at + 1] << 8);
    if (blen < 2 || block_at + 2 + blen > image_len) { block_at = 0; block_no = 0; return false; }

    const uint8_t *body = &tape_image[block_at + 2];
    const uint8_t  type = body[0];

    // Consume it either way.
    block_at += 2 + blen;
    block_no++;

    if (type != want_type) return false;

    const uint16_t want = (uint16_t)((de_hi << 8) | de_lo);
    uint16_t       dest = (uint16_t)((ix_hi << 8) | ix_lo);

    // The body is flag + data + checksum, so the data is blen - 2 long.
    const uint16_t avail = (uint16_t)(blen - 2);
    const uint16_t count = want < avail ? want : avail;

    // Carry set on entry means LOAD; clear means VERIFY, which reads the
    // block and checks it without storing anything. Rare, but the ROM
    // offers it and a tape that verifies is a tape that loaded.
    if (want_flags & 0x01)
        for (uint16_t i = 0; i < count; i++)
            z80_wpage[(uint16_t)(dest + i) >> 14][(uint16_t)(dest + i) & 0x3FFF] = body[1 + i];

    // Checksum over flag and data, which is what the last byte is.
    uint8_t sum = 0;
    for (uint16_t i = 0; i < avail + 1u; i++) sum ^= body[i];

    return sum == body[avail + 1] && want <= avail;
}

bool tape_port_out(const uint16_t port, const uint8_t value) {
    if (!mounted || (port & 0xFF) != TAPE_PORT) return false;

    switch (step) {
        case 0: want_type  = value; step = 1; break;
        case 1: want_flags = value; step = 2; break;
        case 2: ix_hi      = value; step = 3; break;
        case 3: ix_lo      = value; step = 4; break;
        case 4: de_hi      = value; step = 5; break;
        case 5: de_lo      = value; step = 0; last_ok = serve(); break;
        default: step = 0; break;
    }
    return true;
}

bool tape_port_in(const uint16_t port, uint8_t *value) {
    if (!mounted || (port & 0xFF) != TAPE_PORT) return false;
    *value = last_ok ? 1 : 0;
    step = 0;              // resynchronise, in case a sequence was cut short
    return true;
}
