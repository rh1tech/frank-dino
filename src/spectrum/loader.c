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
#include "beta.h"
#include "dck.h"
#include "divmmc.h"
#include "fdc.h"
#include "settings.h"
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
    return z80_peek(addr);
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
     * One staging buffer, for the bank a 128K snapshot writes first and
     * names last.
     *
     * In the PSRAM: it is touched once per snapshot load, with the Z80
     * stopped, which is the whole test for what belongs over there. It
     * used to be in SRAM so a board with no PSRAM could still load a
     * snapshot -- but there is no such board, U7 is soldered down, and
     * main() now refuses to start without it.
     */
    static uint8_t stage[SPEC_BANK_SIZE] __attribute__((section(".psram")));

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
// .Z80
// ---------------------------------------------------------------------------
/*
 * The format everything is archived in, and the most negotiated one.
 *
 * Three versions, distinguished by a value rather than a version field: a
 * program counter of zero in the v1 header means "this is not a v1, the
 * real PC is in the extension block". The extension block's own length
 * then says v2 (23) or v3 (54 or 55).
 *
 * Memory is run-length compressed with ED ED as the escape, except where
 * a block declares itself 0xFFFF long, which means sixteen uncompressed
 * kilobytes. A v1 file is one stream covering 0x4000-0xFFFF; a v2 or v3
 * is a series of blocks, each naming the page it belongs to.
 *
 * The rules are pico-speccy's FileZ80::load(), which is where the page
 * numbering and the machine-byte table below come from.
 */

/*
 * A buffered reader, because this decompresses a byte at a time.
 *
 * f_read() per byte through FatFs and an SPI card is about as slow as
 * this machine gets; 512 bytes at a time is one sector and makes a 48 KB
 * snapshot a hundred reads rather than fifty thousand.
 */
typedef struct {
    FIL     *f;
    uint8_t  buf[512];
    unsigned have, at;
    bool     eof;
} reader_t;

static int rd(reader_t *r) {
    if (r->at >= r->have) {
        UINT got = 0;
        if (f_read(r->f, r->buf, sizeof r->buf, &got) != FR_OK || got == 0) {
            r->eof = true;
            return -1;
        }
        r->have = got;
        r->at   = 0;
    }
    return r->buf[r->at++];
}

/* Where a guest address lives right now. ROM writes fall in the bucket. */
static void poke(const uint16_t addr, const uint8_t v) {
    z80_poke(addr, v);
}

/*
 * Expand one compressed run into a page, or into the address space.
 *
 * Two destinations rather than one because a v1 stream is addressed and a
 * v2/v3 block is a bank that may not be mapped anywhere at the time. A
 * NULL `page` means "use `base` as a guest address".
 */
static void unpack(reader_t *r, uint32_t in_len, uint8_t *page, uint16_t base, uint16_t out_len) {
    uint32_t in = 0;
    uint16_t out = 0;

    while (in < in_len && out < out_len) {
        const int b = rd(r);
        if (b < 0) return;
        in++;

        if (b != 0xED) {
            if (page) page[out++] = (uint8_t)b; else poke((uint16_t)(base + out++), (uint8_t)b);
            continue;
        }

        const int b2 = rd(r);
        if (b2 < 0) return;
        in++;

        if (b2 != 0xED) {
            if (page) page[out++] = 0xED; else poke((uint16_t)(base + out++), 0xED);
            if (out < out_len) {
                if (page) page[out++] = (uint8_t)b2; else poke((uint16_t)(base + out++), (uint8_t)b2);
            }
            continue;
        }

        const int cnt = rd(r), val = rd(r);
        if (cnt < 0 || val < 0) return;
        in += 2;
        for (int i = 0; i < cnt && out < out_len; i++) {
            if (page) page[out++] = (uint8_t)val; else poke((uint16_t)(base + out++), (uint8_t)val);
        }
    }
}

static void raw(reader_t *r, uint8_t *page, uint16_t base, uint16_t len) {
    for (uint16_t i = 0; i < len; i++) {
        const int b = rd(r);
        if (b < 0) return;
        if (page) page[i] = (uint8_t)b; else poke((uint16_t)(base + i), (uint8_t)b);
    }
}

static void z80_header(const uint8_t *h, z80_state_t *st) {
    st->af  = (uint16_t)((h[0] << 8) | h[1]);       // A then F
    st->bc  = rd16(h + 2);
    st->hl  = rd16(h + 4);
    st->pc  = rd16(h + 6);
    st->sp  = rd16(h + 8);
    st->i   = h[10];

    /*
     * R is seven bits in the header and its eighth is somewhere else.
     * Byte 12's bit 0 carries it, which is the sort of thing that only
     * matters until a program uses R as a random number source.
     */
    st->r   = (uint8_t)((h[11] & 0x7F) | ((h[12] & 0x01) << 7));

    st->de  = rd16(h + 13);
    st->bc_ = rd16(h + 15);
    st->de_ = rd16(h + 17);
    st->hl_ = rd16(h + 19);
    st->af_ = (uint16_t)((h[21] << 8) | h[22]);     // A' then F'
    st->iy  = rd16(h + 23);
    st->ix  = rd16(h + 25);
    st->iff = h[27] != 0;
    st->im  = h[29] & 3;
    st->border = (uint8_t)((h[12] >> 1) & 7);
}

/*
 * Which machine a .z80 was taken on.
 *
 * Byte 34 of the extension block, and it means different things in v2 and
 * v3 -- 4 is "128K + Interface 1" in one and "128K" in the other, which
 * is the kind of difference that silently loads a snapshot into the wrong
 * machine. Anything this firmware does not have is mapped to the nearest
 * machine it does, because a snapshot is RAM and registers: a Pentagon
 * dump runs on a 128K, slightly wrongly, which is a great deal better
 * than refusing it.
 */
static machine_t z80_machine(const uint8_t ver, const uint8_t mch, bool *exact) {
    *exact = true;
    if (ver == 2) {
        switch (mch) {
            case 0: case 1:  return MACHINE_48K;
            case 3: case 4:  return MACHINE_128K;
            default: *exact = false; return MACHINE_128K;
        }
    }
    switch (mch) {
        case 0: case 1: case 3: return MACHINE_48K;     // 48K, +IF1, +MGT
        case 4: case 5: case 6: return MACHINE_128K;    // 128K, +IF1, +MGT
        case 12:                return MACHINE_PLUS2;
        default: *exact = false; return MACHINE_128K;   // +3, Pentagon, Scorpion...
    }
}

static bool load_z80(FIL *f, const uint32_t size, char *detail, const unsigned detail_len) {
    uint8_t h[87];
    UINT got = 0;
    if (f_read(f, h, 30, &got) != FR_OK || got != 30)
        FAIL("could not read the .z80 header");

    z80_state_t st = {0};
    z80_header(h, &st);

    uint8_t  ver = 1;
    uint8_t  mch = 0;
    unsigned ext = 0;
    bool     exact = true;

    if (st.pc == 0) {
        if (f_read(f, h + 30, 2, &got) != FR_OK || got != 2) FAIL(".z80 ends at the version word");
        ext = rd16(h + 30);
        if      (ext == 23)             ver = 2;
        else if (ext == 54 || ext == 55) ver = 3;
        else FAIL(".z80 extension block is %u bytes; expected 23, 54 or 55", (unsigned)ext);

        if (ext > sizeof h - 32) FAIL(".z80 extension block will not fit");
        if (f_read(f, h + 32, ext, &got) != FR_OK || got != ext)
            FAIL(".z80 extension block is short");

        st.pc = rd16(h + 32);
        mch   = h[34];
    }

    const machine_t m = ver == 1 ? MACHINE_48K : z80_machine(ver, mch, &exact);
    spec_set_machine(m);

    static reader_t r;      // 512 bytes; core 0's stack is not that big
    r = (reader_t){ .f = f };

    if (ver == 1) {
        /*
         * One stream, 48 KB, starting at 0x4000. Byte 12 bit 5 says
         * whether it is compressed -- and that byte is 255 in files
         * written by a handful of old tools, which by convention means
         * 1 and therefore "compressed".
         */
        const uint8_t b12 = h[12] == 255 ? 1 : h[12];
        st.port7ffd = 0x30;                     // meaningless on a 48K
        spec_set_paging(st.port7ffd);

        if (b12 & 0x20) unpack(&r, size - 30, NULL, 0x4000, 0xC000);
        else            raw(&r, NULL, 0x4000, 0xC000);
    } else if (m == MACHINE_48K) {
        st.port7ffd = 0x30;
        spec_set_paging(st.port7ffd);

        // Page ids on a 48K name addresses, not banks, and only three of
        // the eleven exist.
        for (;;) {
            const int l0 = rd(&r), l1 = rd(&r), id = rd(&r);
            if (l0 < 0 || l1 < 0 || id < 0) break;
            const uint16_t len = (uint16_t)(l0 | (l1 << 8));

            const uint16_t base = id == 4 ? 0x8000 : id == 5 ? 0xC000 : id == 8 ? 0x4000 : 0;
            if (!base) break;                   // ROM or something unknown; stop

            if (len == 0xFFFF) raw(&r, NULL, base, SPEC_BANK_SIZE);
            else               unpack(&r, len, NULL, base, SPEC_BANK_SIZE);
        }
    } else {
        // Byte 35 is the paging latch itself, which is the whole of a
        // 128K's memory state.
        st.port7ffd = h[35];
        spec_set_paging(st.port7ffd);

        for (;;) {
            const int l0 = rd(&r), l1 = rd(&r), id = rd(&r);
            if (l0 < 0 || l1 < 0 || id < 0) break;
            const uint16_t len = (uint16_t)(l0 | (l1 << 8));

            // 3..10 are RAM banks 0..7. Anything else is a ROM block,
            // which is ours rather than the snapshot's -- so it is read
            // past rather than stored, to keep the stream in step.
            uint8_t *page = (id >= 3 && id <= 10) ? spec_ram[id - 3] : NULL;

            if (len == 0xFFFF) {
                if (page) raw(&r, page, 0, SPEC_BANK_SIZE);
                else for (uint16_t i = 0; i < SPEC_BANK_SIZE; i++) if (rd(&r) < 0) break;
            } else {
                if (page) unpack(&r, len, page, 0, SPEC_BANK_SIZE);
                else for (uint16_t i = 0; i < len; i++) if (rd(&r) < 0) break;
            }
        }
    }

    if (!spec_launch(&st))
        FAIL("the CPU would not leave the restore code");

    snprintf(detail, detail_len, "%s .z80 v%u running from %04X%s",
             spec_machine_name(), ver, st.pc, exact ? "" : " (machine approximated)");
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
/* Which of the three tape formats is in the file. */
typedef enum { TAPE_RAW = 0, TAPE_TZX, TAPE_PZX } tape_format_t;

static bool load_tap(FIL *f, const uint32_t size, const tape_format_t fmt,
                     char *detail, const unsigned detail_len) {
    if (size > TAPE_MAX_BYTES)
        FAIL("that tape is %luK; the limit is %uK",
             (unsigned long)(size / 1024), TAPE_MAX_BYTES / 1024);

    // Straight into the tape's own buffer: 64 KB is too much to have two of.
    UINT got = 0;
    if (f_read(f, tape_image, size, &got) != FR_OK || got != size)
        FAIL("could not read the whole tape");

    // A .TZX or a .PZX is converted to a .TAP in place before it is
    // mounted; see src/spectrum/tape.c for what survives that and what
    // does not.
    const bool ok = (fmt == TAPE_TZX) ? tape_mount_tzx(size, detail, detail_len)
                  : (fmt == TAPE_PZX) ? tape_mount_pzx(size, detail, detail_len)
                  : tape_mount(tape_image, size, detail, detail_len);
    if (!ok) return false;

    // Keep the message tape_mount() wrote, and add to it below.
    char mounted[64];
    snprintf(mounted, sizeof mounted, "%s", detail);

    if (!settings.tape_autostart) {
        snprintf(detail, detail_len, "%s -- type LOAD \"\"", mounted);
        return true;
    }

    spec_set_machine(spec_machine);      // cold start, tape still mounted

    // ENTER picks "128 BASIC" off the menu; the 48K has no menu to pick from.
    spec_type(spec_machine == MACHINE_48K ? "j\"\"\r" : "\rload \"\"\r", 100);

    snprintf(detail, detail_len, "%s -- loading", mounted);
    return true;
}

// ---------------------------------------------------------------------------
// .TRD
// ---------------------------------------------------------------------------

/*
 * Type the disk's own start-up command, if it needs one.
 *
 * The string has to outlive this call, because spec_type() feeds it one
 * character every few frames and does not copy it. The delay is TR-DOS's
 * own start-up: it prints its banner, decides there is no boot file and
 * puts up a prompt, and typing into it before then loses the first
 * letter.
 */
static char autorun[24];

static void disk_autorun(void) {
    if (settings.tape_autostart && beta_autorun(autorun, sizeof autorun))
        spec_type(autorun, 160);
}

/*
 * A raw disk, straight into the drive.
 *
 * There is no format to parse: a .trd is the sectors in order, which is
 * why its size alone says how many tracks it has. What there is to do is
 * turn the interface on if it is off -- a .trd is meaningless without it
 * -- and start the machine with the TR-DOS ROM in the bottom window, the
 * way a Beta Disk 128 comes up, so that a disk with a `boot` file on it
 * starts by itself.
 */
/*
 * A .dsk goes in the +3's drive, and switches the machine to a +3.
 *
 * A disk image is only meaningful on the machine whose controller can
 * read it, so mounting one on a 48K would be a mount nothing could ever
 * use. The .trd path above makes the same decision the other way, by
 * turning the Beta Disk on.
 */
/*
 * A .dck goes in the Timex's dock, and switches the machine to a Timex.
 *
 * Like the .trd and the .dsk above: a cartridge is only meaningful on
 * the machine that has a dock to put it in.
 *
 * This one takes the path rather than a handle, because the format is
 * walked with seeks -- the block headers are scattered through the file
 * and reading it into a buffer first would need the buffer to be the
 * size of the file.
 */
/*
 * A .mmc or .hdf goes in DivMMC's card slot, and turns the interface on.
 *
 * Like the .trd and the .dsk: an image is only meaningful on a machine
 * with the interface that reads it. The Beta Disk goes off at the same
 * time -- both claim a fetch at #3D00-#3DFF, so with the two on they
 * fight over the bottom of the address space.
 */
static bool load_mmc(const char *path, char *detail, const unsigned detail_len) {
    if (!divmmc_mount(path, detail, detail_len)) return false;

    char mounted[80];
    snprintf(mounted, sizeof mounted, "%s", detail);
    settings.esxdos = 1;
    settings.betadisk = 0;
    spec_set_machine(spec_machine);      /* cold start with esxDOS in */

    snprintf(detail, detail_len, "%s -- esxDOS on", mounted);
    return true;
}

static bool load_dck(const char *path, char *detail, const unsigned detail_len) {
    if (!dck_mount(path, detail, detail_len)) return false;

    char mounted[80];
    snprintf(mounted, sizeof mounted, "%s", detail);
    spec_set_machine(MACHINE_TIMEX);
    snprintf(detail, detail_len, "%s -- in the dock", mounted);
    return true;
}

static bool load_dsk(FIL *f, const uint32_t size, const char *path,
                     char *detail, const unsigned detail_len) {
    if (size > DSK_MAX_BYTES)
        FAIL("that .dsk is %luK; the limit is %uK",
             (unsigned long)(size / 1024), DSK_MAX_BYTES / 1024);

    UINT got = 0;
    if (f_read(f, dsk_image, size, &got) != FR_OK || got != size)
        FAIL("could not read the whole .dsk");

    if (!fdc_mount(size, path, detail, detail_len)) return false;

    char mounted[80];
    snprintf(mounted, sizeof mounted, "%s", detail);

    /* The +3 boots from the drive by itself when there is a disk in it. */
    spec_set_machine(MACHINE_PLUS3);

    snprintf(detail, detail_len, "%s -- booting", mounted);
    return true;
}

static bool load_trd(FIL *f, const uint32_t size, const char *path,
                     char *detail, const unsigned detail_len) {
    if (size > TRD_MAX_BYTES)
        FAIL("that .trd is %luK; the limit is %uK",
             (unsigned long)(size / 1024), TRD_MAX_BYTES / 1024);

    UINT got = 0;
    if (f_read(f, trd_image, size, &got) != FR_OK || got != size)
        FAIL("could not read the whole .trd");

    if (!beta_mount(size, path, detail, detail_len)) return false;

    char mounted[80];
    snprintf(mounted, sizeof mounted, "%s", detail);

    settings.betadisk = 1;
    beta_boot_next_reset();
    spec_set_machine((machine_t)spec_machine);
    disk_autorun();

    snprintf(detail, detail_len, "%s -- booting", mounted);
    return true;
}

// ---------------------------------------------------------------------------
// .SCL
// ---------------------------------------------------------------------------
/*
 * The same disk with the empty space left out.
 *
 * A .trd is every sector of an 80-track double-sided disk whether it
 * holds anything or not, which is 640 KB for a game that is forty. A
 * .scl is the other way round: a signature, a count, one fourteen-byte
 * directory entry per file and then the files' sectors end to end. It
 * is how TR-DOS software actually travelled, and half of what is on this
 * card is in it.
 *
 * Converting one into the other is a matter of putting the sectors back
 * where TR-DOS expects them and writing the two things a .scl leaves
 * out: the catalogue, which is the directory entries plus the place each
 * file ended up, and the volume descriptor in sector 9 that says how
 * many files there are and where the free space starts. Without that
 * descriptor TR-DOS calls the disk unformatted.
 */
#define SCL_ENTRY   14          // 8 name, 1 extension, 2 start, 2 length, 1 sectors
#define SCL_MAX_FILES 128
#define TRD_SECTOR  256
#define TRD_SPT     16
#define TRD_LTRACKS 160         // 80 cylinders, two sides
#define TRD_SIZE    ((uint32_t)TRD_LTRACKS * TRD_SPT * TRD_SECTOR)

static bool load_scl(FIL *f, char *detail, const unsigned detail_len) {
    uint8_t head[9];
    UINT got = 0;
    if (f_read(f, head, sizeof head, &got) != FR_OK || got != sizeof head)
        FAIL("could not read the .scl header");
    if (memcmp(head, "SINCLAIR", 8) != 0) FAIL("that is not a .scl");

    const unsigned files = head[8];
    if (files == 0 || files > SCL_MAX_FILES) FAIL(".scl says it holds %u files", files);

    // 1792 bytes at most, and core 0's stack is not the place for it.
    static uint8_t dir[SCL_MAX_FILES * SCL_ENTRY];
    const UINT dirlen = (UINT)(files * SCL_ENTRY);
    if (f_read(f, dir, dirlen, &got) != FR_OK || got != dirlen)
        FAIL(".scl directory is short");

    if (TRD_SIZE > TRD_MAX_BYTES) FAIL("the disk buffer is too small");
    memset(trd_image, 0, TRD_SIZE);

    /*
     * Files go down one after another from the start of track 1, which
     * is where TR-DOS puts them: track 0 is the catalogue and the
     * descriptor, and nothing else ever lives there.
     */
    unsigned at = TRD_SPT;                      // in sectors, from the start
    for (unsigned i = 0; i < files; i++) {
        const uint8_t *e = &dir[i * SCL_ENTRY];
        const unsigned len = e[13];             // the file's length in sectors

        if (at + len > TRD_LTRACKS * TRD_SPT)
            FAIL(".scl holds more than a disk does");

        uint8_t *cat = &trd_image[i * 16];
        memcpy(cat, e, SCL_ENTRY);
        cat[14] = (uint8_t)(at % TRD_SPT);      // where it starts: sector
        cat[15] = (uint8_t)(at / TRD_SPT);      // ...and logical track

        const UINT bytes = (UINT)len * TRD_SECTOR;
        if (bytes && (f_read(f, &trd_image[(uint32_t)at * TRD_SECTOR], bytes, &got) != FR_OK
                      || got != bytes))
            FAIL(".scl is short in file %u of %u", i + 1, files);

        at += len;
    }

    /*
     * Sector 9: the volume descriptor, which is the only part of a
     * TR-DOS disk that is not either a catalogue entry or a file.
     */
    uint8_t *const vol = &trd_image[8 * TRD_SECTOR];
    vol[0xE1] = (uint8_t)(at % TRD_SPT);        // first free sector
    vol[0xE2] = (uint8_t)(at / TRD_SPT);        // first free track
    vol[0xE3] = 0x16;                           // 80 tracks, double sided
    vol[0xE4] = (uint8_t)files;
    const unsigned free_sectors = TRD_LTRACKS * TRD_SPT - at;
    vol[0xE5] = (uint8_t)(free_sectors & 0xFF);
    vol[0xE6] = (uint8_t)(free_sectors >> 8);
    vol[0xE7] = 0x10;                           // "this is a TR-DOS disk"
    vol[0xF4] = 0;                              // no deleted files
    memset(&vol[0xF5], ' ', 8);                 // the label, which a .scl has not got

    /*
     * NULL, not the .scl's own path: what is in trd_image now is a .trd
     * built from it, and writing that back over the .scl would replace a
     * 100 KB archive with a 640 KB disk under the same name.
     */
    if (!beta_mount(TRD_SIZE, NULL, detail, detail_len)) return false;

    char mounted[80];
    snprintf(mounted, sizeof mounted, "%s", detail);

    settings.betadisk = 1;
    beta_boot_next_reset();
    spec_set_machine((machine_t)spec_machine);
    disk_autorun();

    snprintf(detail, detail_len, "%s -- booting", mounted);
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

        case FILE_TAP: ok = load_tap(&f, (uint32_t)fi.fsize, TAPE_RAW, detail, detail_len); break;
        case FILE_TZX: ok = load_tap(&f, (uint32_t)fi.fsize, TAPE_TZX, detail, detail_len); break;
        case FILE_PZX: ok = load_tap(&f, (uint32_t)fi.fsize, TAPE_PZX, detail, detail_len); break;
        case FILE_Z80: ok = load_z80(&f, (uint32_t)fi.fsize, detail, detail_len); break;
        case FILE_TRD: ok = load_trd(&f, (uint32_t)fi.fsize, path, detail, detail_len); break;
        case FILE_SCL: ok = load_scl(&f, detail, detail_len); break;
        case FILE_DSK: ok = load_dsk(&f, (uint32_t)fi.fsize, path, detail, detail_len); break;
        case FILE_DCK: ok = load_dck(path, detail, detail_len); break;
        case FILE_MMC: ok = load_mmc(path, detail, detail_len); break;
        default:       snprintf(detail, detail_len, "nothing here knows what that file is"); ok = false; break;
    }

    f_close(&f);
    return ok;
}
