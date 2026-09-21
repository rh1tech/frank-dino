/*
 * frank-dino — an RP2350B acting as the whole machine around a real Z80
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-dino
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "loader.h"

#include <stdio.h>
#include <string.h>

#include "ff.h"
#include "launch.h"
#include "spectrum.h"
#include "tape.h"
#include "z80_bus.h"

#define FAIL(...) do { snprintf(detail, detail_len, __VA_ARGS__); return false; } while (0)

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

/*
 * Read one guest address through the current mapping.
 *
 * The window table is the truth about where an address lives, so using
 * it keeps one definition of the memory map rather than a second one
 * here that could disagree with it.
 */
static uint8_t peek(const uint16_t addr) {
    return z80_rpage[addr >> 14][addr & 0x3FFF];
}

// ---------------------------------------------------------------------------
// .SNA
// ---------------------------------------------------------------------------
/*
 * Twenty-seven bytes of registers, then memory.
 *
 * The 48K form has no program counter in its header at all: the snapshot
 * was taken inside an interrupt, so PC is on the stack and the restore
 * ends with a RETN. Here that means reading the word at SP out of the RAM
 * that has just been loaded and moving SP past it — the trampoline jumps
 * rather than returns, so the stack must be left as the program expects
 * to find it.
 *
 * The 128K form keeps PC in a footer instead, along with the paging
 * latch, and stores the banks in a fixed order with the three that are
 * currently mapped written first.
 */
#define SNA_HEADER 27
#define SNA_48_LEN (SNA_HEADER + 49152)

static void sna_header(const uint8_t *h, z80_state_t *st) {
    st->i   = h[0];
    st->hl_ = rd16(h + 1);
    st->de_ = rd16(h + 3);
    st->bc_ = rd16(h + 5);
    st->af_ = rd16(h + 7);
    st->hl  = rd16(h + 9);
    st->de  = rd16(h + 11);
    st->bc  = rd16(h + 13);
    st->iy  = rd16(h + 15);
    st->ix  = rd16(h + 17);
    st->iff = (h[19] & 0x04) != 0;      // IFF2, which is where it is kept
    st->r   = h[20];
    st->af  = rd16(h + 21);
    st->sp  = rd16(h + 23);
    st->im  = h[25] & 3;
    st->border = h[26] & 7;
}

static bool load_sna(FIL *f, const uint32_t size, char *detail, const unsigned detail_len) {
    uint8_t h[SNA_HEADER];
    UINT got = 0;
    if (f_read(f, h, SNA_HEADER, &got) != FR_OK || got != SNA_HEADER)
        FAIL("could not read the snapshot header");

    z80_state_t st = {0};
    sna_header(h, &st);

    /*
     * The staging buffer is the tape's.
     *
     * A 16 KB static here was 16 KB the chip did not have once the tape
     * buffer arrived, and the two can never be wanted at the same time:
     * a snapshot replaces the entire machine, so whatever tape was
     * mounted was going to be irrelevant a moment later anyway. Ejecting
     * it first makes that explicit rather than leaving a mounted tape
     * pointing at a buffer this is about to overwrite.
     */
    tape_eject();
    uint8_t *const stage = tape_image;

    if (size == SNA_48_LEN) {
        spec_set_machine(MACHINE_48K);
        st.port7ffd = 0x30;             // meaningless on a 48K; harmless

        // Straight into the banks the 48K map puts at 4000, 8000 and C000,
        // rather than through poke() a byte at a time.
        uint8_t *const dst48[3] = { spec_ram[5], spec_ram[2], spec_ram[0] };
        for (int i = 0; i < 3; i++)
            if (f_read(f, dst48[i], SPEC_BANK_SIZE, &got) != FR_OK || got != SPEC_BANK_SIZE)
                FAIL("48K snapshot is short in bank %d", i);

        // PC off the stack, and then past it.
        st.pc = (uint16_t)(peek(st.sp) | (peek((uint16_t)(st.sp + 1)) << 8));
        st.sp = (uint16_t)(st.sp + 2);
    } else {
        spec_set_machine(MACHINE_128K);

        // Banks 5 and 2 first, then whichever one the footer says was at
        // 0xC000 -- which cannot be known until the footer is read, so it
        // is parked and sorted out afterwards.
        for (int which = 0; which < 3; which++) {
            uint8_t *dst = which == 0 ? spec_ram[5] : which == 1 ? spec_ram[2] : stage;
            if (f_read(f, dst, SPEC_BANK_SIZE, &got) != FR_OK || got != SPEC_BANK_SIZE)
                FAIL("128K snapshot is short in the first three banks");
        }

        uint8_t footer[4];
        if (f_read(f, footer, 4, &got) != FR_OK || got != 4)
            FAIL("128K snapshot has no footer");

        st.pc       = rd16(footer);
        st.port7ffd = footer[2];

        const uint8_t cur = st.port7ffd & 7;
        memcpy(spec_ram[cur], stage, SPEC_BANK_SIZE);

        // The rest, in ascending order, skipping the three already placed.
        for (uint8_t b = 0; b < SPEC_RAM_BANKS; b++) {
            if (b == 5 || b == 2 || b == cur) continue;
            if (f_read(f, spec_ram[b], SPEC_BANK_SIZE, &got) != FR_OK || got != SPEC_BANK_SIZE)
                break;                  // a short file simply leaves the rest as it was
        }
    }

    spec_set_paging(st.port7ffd);
    if (!spec_launch(&st))
        FAIL("the CPU would not leave the restore code");

    snprintf(detail, detail_len, "%s snapshot running from %04X",
             size == SNA_48_LEN ? "48K" : "128K", st.pc);
    return true;
}

// ---------------------------------------------------------------------------
// .SCR
// ---------------------------------------------------------------------------
/*
 * Six and three-quarter kilobytes straight into the screen.
 *
 * Not a program, so nothing is launched: the picture is written into the
 * display bank and the machine goes on running whatever it was. Useful
 * mostly as the cheapest possible end-to-end test that the card, the
 * paths and the ULA renderer all agree.
 */
#define SCR_LEN 6912

static bool load_scr(FIL *f, char *detail, const unsigned detail_len) {
    UINT got = 0;
    if (f_read(f, spec_ram[spec_screen_bank], SCR_LEN, &got) != FR_OK || got != SCR_LEN)
        FAIL("screen file is not %d bytes", SCR_LEN);
    snprintf(detail, detail_len, "screen loaded into bank %u", spec_screen_bank);
    return true;
}

// ---------------------------------------------------------------------------
// .TAP
// ---------------------------------------------------------------------------
/*
 * Mount the tape, cold-start the machine, and type LOAD "" into it.
 *
 * The typing is the part worth explaining. Nothing about the tape trap
 * needs it -- a tape sitting mounted will be picked up by a LOAD typed by
 * hand -- but "select a file and watch it load" is the whole point of a
 * browser, and the alternative is a status line telling the user to go
 * and type something.
 *
 * What to type depends on the machine, in a way that is not cosmetic. A
 * 48K boots into K mode, where the first keypress of a line is a keyword
 * and LOAD is the J key; typing L, O, A, D there produces four different
 * keywords. A 128K boots into a menu that has to be dismissed first, and
 * then into an editor with no keyword mode at all, where LOAD is spelled
 * out letter by letter.
 */
static bool load_tap(FIL *f, const uint32_t size, char *detail, const unsigned detail_len) {
    if (size > TAPE_MAX_BYTES)
        FAIL("that .tap is %luK; the limit is %uK",
             (unsigned long)(size / 1024), TAPE_MAX_BYTES / 1024);

    // Straight into the tape's own buffer: 64 KB is too much to have two of.
    UINT got = 0;
    if (f_read(f, tape_image, size, &got) != FR_OK || got != size)
        FAIL("could not read the whole .tap");

    if (!tape_mount(tape_image, size, detail, detail_len)) return false;

    // Keep the message tape_mount() wrote, and add to it below.
    char mounted[64];
    snprintf(mounted, sizeof mounted, "%s", detail);

    spec_set_machine(spec_machine);      // cold start, tape still mounted

    // ENTER picks "128 BASIC" off the menu; the 48K has no menu to pick from.
    spec_type(spec_machine == MACHINE_48K ? "j\"\"\r" : "\rload \"\"\r", 100);

    snprintf(detail, detail_len, "%s -- loading", mounted);
    return true;
}

// ---------------------------------------------------------------------------

bool loader_load(const char *path, const file_kind_t kind,
                 char *detail, const unsigned detail_len) {
    FILINFO fi;
    if (f_stat(path, &fi) != FR_OK) FAIL("cannot stat %s", path);

    FIL f;
    if (f_open(&f, path, FA_READ) != FR_OK) FAIL("cannot open %s", path);

    bool ok;
    switch (kind) {
        case FILE_SNA: ok = load_sna(&f, (uint32_t)fi.fsize, detail, detail_len); break;
        case FILE_SCR: ok = load_scr(&f, detail, detail_len); break;

        case FILE_TAP: ok = load_tap(&f, (uint32_t)fi.fsize, detail, detail_len); break;

        // Not yet, and saying which is better than a generic refusal.
        case FILE_TZX: snprintf(detail, detail_len, ".tzx is not implemented yet; .tap is"); ok = false; break;
        case FILE_Z80: snprintf(detail, detail_len, ".z80 snapshots are not implemented yet"); ok = false; break;
        case FILE_TRD: snprintf(detail, detail_len, "TR-DOS needs a Beta Disk interface"); ok = false; break;
        default:       snprintf(detail, detail_len, "nothing here knows what that file is"); ok = false; break;
    }

    f_close(&f);
    return ok;
}
