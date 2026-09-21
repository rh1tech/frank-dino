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
#include "psram.h"
#include "spectrum.h"
#include "z80_bus.h"

/*
 * The image lives in the PSRAM, not the SRAM.
 *
 * Sixty-four kilobytes is an eighth of the chip's memory and the tape is
 * read a few hundred bytes at a time, a few times a minute -- so it is
 * exactly the kind of thing the 8 MB on CS1 is for. Nothing on the Z80's
 * critical path may go there (see src/drivers/psram/psram.h), and this
 * is not: the CPU is stopped inside an OUT while the block is copied.
 *
 * A board assembled without the chip therefore cannot mount a tape, and
 * tape_mount() says so rather than writing into an unmapped window.
 */
uint8_t tape_image[TAPE_MAX_BYTES] __attribute__((section(".psram")));

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
    if (!psram_size()) {
        snprintf(detail, detail_len, "tapes need the PSRAM, and none was found");
        return false;
    }

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

// ---------------------------------------------------------------------------
// .TZX
// ---------------------------------------------------------------------------
/*
 * A .TZX is a .TAP with the timing put back, and then some.
 *
 * Where a .TAP is a bare list of blocks, a .TZX is a list of *records*:
 * data blocks that carry their own pilot and bit timings, but also pure
 * tones, raw pulse trains, pauses, loops, jumps, selection menus and
 * several kinds of text. It exists because a .TAP cannot represent a
 * loader that does not use the ROM's timings, which most commercial
 * tapes did not.
 *
 * None of which this firmware can replay -- a ROM trap answers a call to
 * LD-BYTES and has nowhere to put a pulse train. What it CAN use is the
 * two record types whose payload is exactly a .TAP block: 0x10, standard
 * speed data, and 0x11, turbo speed data. Both are flag, data and
 * checksum; only the timings around them differ, and the timings are the
 * part being skipped anyway.
 *
 * So a .TZX is converted to a .TAP on the way in. Everything else in the
 * file is stepped over -- which is the fiddly part, because stepping over
 * a record means knowing the length of every record type there is,
 * including the ones that will never be used.
 *
 * A file whose loader is a pulse train will come out with a header or two
 * and nothing else, and the caller says so. That is honest: those tapes
 * need the signal, not the bytes.
 */

/* Little-endian reads, at a bounds-checked offset. */
static uint32_t at16(const uint32_t p) { return (uint32_t)tape_image[p] | ((uint32_t)tape_image[p + 1] << 8); }
static uint32_t at24(const uint32_t p) { return at16(p) | ((uint32_t)tape_image[p + 2] << 16); }
static uint32_t at32(const uint32_t p) { return at16(p) | ((uint32_t)at16(p + 2) << 16); }

/*
 * Convert in place, which is safe because the output is never longer.
 *
 * A 0x10 record costs five bytes of header and produces two; every other
 * record produces nothing at all. So the write pointer can never catch
 * the read pointer, and the conversion needs no second buffer -- which
 * matters, since the buffer is 64 KB and there is one of it.
 */
static uint32_t tzx_to_tap(const uint32_t len) {
    uint32_t in = 10;               // past "ZXTape!\x1A" and the two version bytes
    uint32_t out = 0;

    while (in < len) {
        const uint8_t id = tape_image[in++];
        uint32_t data = 0, dlen = 0;

        switch (id) {
            case 0x10:                                  // standard speed data
                if (in + 4 > len) return out;
                dlen = at16(in + 2);
                data = in + 4;
                in   = data + dlen;
                break;

            case 0x11:                                  // turbo speed data
                if (in + 0x12 > len) return out;
                dlen = at24(in + 0x0F);
                data = in + 0x12;
                in   = data + dlen;
                break;

            // Everything below is skipped. The lengths are the format's,
            // and getting one wrong desynchronises the rest of the file --
            // which is why they are all here rather than only the common ones.
            case 0x12: in += 4; break;                              // pure tone
            case 0x13: if (in >= len) return out;
                       in += 1 + 2u * tape_image[in]; break;        // pulse sequence
            case 0x14: if (in + 0x0A > len) return out;
                       in += 0x0A + at24(in + 7); break;            // pure data
            case 0x15: if (in + 0x08 > len) return out;
                       in += 0x08 + at24(in + 5); break;            // direct recording
            case 0x18: case 0x19:                                   // CSW, generalized
                       if (in + 4 > len) return out;
                       in += 4 + at32(in); break;
            case 0x20: in += 2; break;                              // pause / stop
            case 0x21: if (in >= len) return out;
                       in += 1 + tape_image[in]; break;             // group start
            case 0x22: break;                                       // group end
            case 0x23: in += 2; break;                              // jump
            case 0x24: in += 2; break;                              // loop start
            case 0x25: break;                                       // loop end
            case 0x26: if (in + 2 > len) return out;
                       in += 2 + 2u * at16(in); break;              // call sequence
            case 0x27: break;                                       // return
            case 0x28: if (in + 2 > len) return out;
                       in += 2 + at16(in); break;                   // select
            case 0x2A: in += 4; break;                              // stop if 48K
            case 0x2B: in += 5; break;                              // set signal level
            case 0x30: if (in >= len) return out;
                       in += 1 + tape_image[in]; break;             // text description
            case 0x31: if (in + 2 > len) return out;
                       in += 2 + tape_image[in + 1]; break;         // message
            case 0x32: if (in + 2 > len) return out;
                       in += 2 + at16(in); break;                   // archive info
            case 0x33: if (in >= len) return out;
                       in += 1 + 3u * tape_image[in]; break;        // hardware type
            case 0x35: if (in + 0x14 > len) return out;
                       in += 0x14 + at32(in + 0x10); break;         // custom info
            case 0x5A: in += 9; break;                              // glue

            default:
                // An unknown record has no length field to trust, so the
                // rest of the file is unreadable. Keep what was found.
                return out;
        }

        if (!dlen) continue;
        if (data + dlen > len) return out;

        // Emit it as a .TAP block: a length word and the bytes.
        tape_image[out++] = (uint8_t)(dlen & 0xFF);
        tape_image[out++] = (uint8_t)(dlen >> 8);
        memmove(&tape_image[out], &tape_image[data], dlen);
        out += dlen;
    }

    return out;
}

bool tape_mount_tzx(const uint32_t len, char *detail, const unsigned detail_len) {
    if (len < 10 || memcmp(tape_image, "ZXTape!\x1A", 8) != 0) {
        snprintf(detail, detail_len, "that is not a .tzx");
        return false;
    }

    const uint32_t converted = tzx_to_tap(len);
    if (converted < 4) {
        snprintf(detail, detail_len,
                 "no ROM-loadable blocks: this .tzx is a custom loader");
        return false;
    }

    return tape_mount(tape_image, converted, detail, detail_len);
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
