/*
 * Every machine's memory map, driven through the real port handler.
 *
 * The bug this exists for: a brace slip nested the #7FFD arm of
 * spec_io_out() inside the +3's #1FFD arm, so on a 128K every
 * OUT (#7FFD),A was discarded. The machine then looped forever in ROM
 * 0's interrupt stub, eight bytes of stack per turn, until SP walked
 * into the screen. A 48K was immune -- it ignores #7FFD by design --
 * which is exactly why a per-machine test and not a smoke test.
 *
 * Pages are identified by CONTENT, against the same flash masters the
 * firmware embeds, because that is what the Z80 actually sees; a
 * pointer comparison would have passed while the window held the wrong
 * image.
 */
#include <stdio.h>
#include <string.h>

#include "spectrum.h"
#include "z80_bus.h"
#include "settings.h"
#include "tap.h"

extern const uint8_t spec_rom0[], spec_rom1[], spec_rom48[], spec_romp2[];
extern const uint8_t spec_romp3_0[], spec_romp3_1[], spec_romp3_2[], spec_romp3_3[];
extern const uint8_t spec_romsc_0[], spec_romsc_1[], spec_romsc_2[], spec_romsc_3[];
extern const uint8_t spec_rompr_0[], spec_rompr_1[], spec_rompr_2[], spec_rompr_3[];
extern const uint8_t spec_romtx_home[], spec_romtx_ex[];
extern const uint8_t spec_rompent[], spec_romtrd[];

extern bool stub_beta_paged, stub_beta_boot, stub_divmmc_mapped;
extern uint8_t stub_dock[8][8192];

/* ------------------------------------------------------------------ */
/* Naming whatever is in a window, so a failure says what it found.    */
/* ------------------------------------------------------------------ */
static const struct { const char *name; const uint8_t *img; unsigned len; } MASTERS[] = {
    { "128-0",    spec_rom0,      16384 }, { "128-1",   spec_rom1,    16384 },
    { "48",       spec_rom48,     16384 }, { "plus2",   spec_romp2,   16384 },
    { "p3-0",     spec_romp3_0,   16384 }, { "p3-1",    spec_romp3_1, 16384 },
    { "p3-2",     spec_romp3_2,   16384 }, { "p3-3",    spec_romp3_3, 16384 },
    { "sc-0",     spec_romsc_0,   16384 }, { "sc-1",    spec_romsc_1, 16384 },
    { "sc-2",     spec_romsc_2,   16384 }, { "sc-3",    spec_romsc_3, 16384 },
    /* pentagon-0.rom and the Profi's BASIC-128 are the same image, so
     * the Pentagon's name has to be claimed first or its ROM reads as
     * the Profi's. */
    { "pentagon", spec_rompent,   16384 },
    { "pr-0",     spec_rompr_0,   16384 }, { "pr-1",    spec_rompr_1, 16384 },
    { "pr-2",     spec_rompr_2,   16384 }, { "pr-3",    spec_rompr_3, 16384 },
    { "tx-home",  spec_romtx_home,16384 }, { "trdos",   spec_romtrd,  16384 },
    { "tx-ex",    spec_romtx_ex,   8192 },
};

/*
 * What is in 8 KB slot `s`: a ROM image by name, a RAM bank, or "?".
 *
 * A 16 KB master is matched over its whole length before its low half
 * is claimed, because several of these images share their first 8 KB --
 * a Pentagon's ROM 0 and a 128K's are the same up to byte 10118 -- and
 * naming one of them the other would make a real mispage look right.
 *
 * Four buffers, cycled: map4() prints four of these in one call, and a
 * single static buffer would hand it the same string four times.
 */
static const char *slot(const unsigned s) {
    static char bufs[4][32];
    static unsigned turn;
    char *buf = bufs[turn++ & 3];
    const uint8_t *p = z80_rpage[s];
    if (!p) return "NULL";
    for (unsigned b = 0; b < SPEC_RAM_BANKS; b++) {
        if (p == spec_ram[b])        { snprintf(buf, 32, "ram%u.lo", b); return buf; }
        if (p == spec_ram[b] + 8192) { snprintf(buf, 32, "ram%u.hi", b); return buf; }
    }
    for (unsigned m = 0; m < count_of(MASTERS); m++) {
        if (MASTERS[m].len == 16384 && !memcmp(p, MASTERS[m].img, 16384)) {
            snprintf(buf, 32, "%s.lo", MASTERS[m].name); return buf;
        }
        if (!memcmp(p, MASTERS[m].img + MASTERS[m].len - 8192, 8192)) {
            snprintf(buf, 32, "%s.%s", MASTERS[m].name,
                     MASTERS[m].len == 16384 ? "hi" : "lo"); return buf;
        }
    }
    return "?";
}

/*
 * Is slot s read-only -- does a write through it reach the read page?
 *
 * The probe byte has to be read back through z80_rpage: on a ROM the
 * write pointer is the shared bitbucket, so reading the "before" value
 * through z80_wpage would compare the bucket against the ROM and call
 * every page writable.
 */
static bool ro(const unsigned s) {
    const uint8_t before = z80_rpage[s][0x100];
    z80_wpage[s][0x100] = (uint8_t)~before;
    const bool discarded = (z80_rpage[s][0x100] == before);
    z80_wpage[s][0x100] = before;
    return discarded;
}

static void out(const uint16_t port, const uint8_t v) { z80_io_out(port, v, 0); }

static void boot(const machine_t m) {
    stub_beta_paged = false;
    stub_beta_boot  = false;
    stub_divmmc_mapped = false;
    memset(&settings, 0, sizeof settings);
    spec_machine = m;
    spec_init();
}

/* The four 16 KB windows, as names. */
static void map4(char *o, const size_t n) {
    snprintf(o, n, "%s %s %s %s", slot(0), slot(2), slot(4), slot(6));
}

int main(void) {
    char m4[128];

    /* ---------------------------------------------------------------- */
    GROUP("48K");
    boot(MACHINE_48K);
    map4(m4, sizeof m4);
    STREQ(m4, "48.lo ram5.lo ram2.lo ram0.lo", "initial map");
    CHECK(ro(0) && ro(1), "ROM must be read-only");
    CHECK(!ro(2), "#4000 must be writable");
    EQ(spec_screen_bank, 5, "screen bank");
    /* A 48K has no latch: the map must not move, whatever is written. */
    out(0x7FFD, 0x10); out(0x7FFD, 0x07); out(0x7FFD, 0x08);
    map4(m4, sizeof m4);
    STREQ(m4, "48.lo ram5.lo ram2.lo ram0.lo", "map after #7FFD writes");
    EQ(spec_port_7ffd, 0, "#7FFD latch stays clear");
    EQ(spec_screen_bank, 5, "screen bank after bit 3");

    /* ---------------------------------------------------------------- */
    GROUP("128K");
    boot(MACHINE_128K);
    map4(m4, sizeof m4);
    STREQ(m4, "128-0.lo ram5.lo ram2.lo ram0.lo", "initial map");
    /* The regression, stated as the hardware states it: bit 4 selects
     * the 48K BASIC ROM, whose 0x0038 is the real handler (F5), not
     * ROM 0's four-push stub (E5). */
    EQ(z80_rpage[0][0x38], 0xE5, "ROM 0 holds the interrupt stub");
    out(0x7FFD, 0x10);
    STREQ(slot(0), "128-1.lo", "#7FFD bit 4 pages ROM 1");
    EQ(z80_rpage[0][0x38], 0xF5, "ROM 1 holds the 48K handler");
    out(0x7FFD, 0x00);
    STREQ(slot(0), "128-0.lo", "bit 4 clear pages ROM 0 back");
    CHECK(ro(0), "ROM stays read-only after paging");
    /* All eight banks reachable at #C000. */
    for (unsigned b = 0; b < 8; b++) {
        char want[16]; snprintf(want, sizeof want, "ram%u.lo", b);
        out(0x7FFD, (uint8_t)b);
        STREQ(slot(6), want, "#C000 bank %u", b);
    }
    /* Bit 3 is the shadow screen. */
    out(0x7FFD, 0x00); EQ(spec_screen_bank, 5, "screen normal");
    out(0x7FFD, 0x08); EQ(spec_screen_bank, 7, "screen shadow");
    /* Bit 5 locks paging until reset. */
    out(0x7FFD, 0x20);
    STREQ(slot(0), "128-0.lo", "lock write selects ROM 0");
    out(0x7FFD, 0x10);
    STREQ(slot(0), "128-0.lo", "locked: #7FFD bit 4 ignored");
    out(0x7FFD, 0x03);
    STREQ(slot(6), "ram0.lo", "locked: bank select ignored");
    boot(MACHINE_128K);
    STREQ(slot(0), "128-0.lo", "reset clears the lock");
    out(0x7FFD, 0x10);
    STREQ(slot(0), "128-1.lo", "paging works again after reset");
    /* Partial decode: anything with A15=0 and A1=0 is #7FFD. */
    out(0x7FFD, 0x00);
    out(0x5FFD, 0x10);
    STREQ(slot(0), "128-1.lo", "#5FFD decodes as #7FFD");
    out(0x7FFD, 0x00);
    out(0x7FFF, 0x10);   /* A1 set -> not #7FFD */
    STREQ(slot(0), "128-0.lo", "A1 high is not #7FFD");

    /* ---------------------------------------------------------------- */
    GROUP("+2");
    boot(MACHINE_PLUS2);
    /*
     * plus2.rom is a 48 BASIC -- F5 at 0x0038, not the editor's E5
     * four-push stub -- so it is this machine's ROM 1, and the editor
     * underneath it is the 128K's. Loaded the other way round the +2
     * came up in 48 BASIC with its own menu unreachable.
     */
    STREQ(slot(0), "128-0.lo", "ROM 0 is the 128K editor");
    EQ(z80_rpage[0][0x38], 0xE5, "and it holds the interrupt stub");
    out(0x7FFD, 0x10);
    STREQ(slot(0), "plus2.lo", "ROM 1 is the +2's own 48 BASIC");
    EQ(z80_rpage[0][0x38], 0xF5, "and it holds the real handler");
    out(0x7FFD, 0x05);
    STREQ(slot(6), "ram5.lo", "#C000 bank 5");

    /* ---------------------------------------------------------------- */
    GROUP("Pentagon");
    boot(MACHINE_PENTAGON);
    STREQ(slot(0), "pentagon.lo", "ROM 0 is the Pentagon's");
    out(0x7FFD, 0x10);
    STREQ(slot(0), "128-1.lo", "ROM 1 is the 48K BASIC");
    /* TR-DOS sits on top of whatever is underneath. */
    stub_beta_paged = true; apply_paging_hook();
    STREQ(slot(0), "trdos.lo", "Beta Disk ROM overrides");
    CHECK(ro(0), "TR-DOS ROM is read-only");
    stub_beta_paged = false; apply_paging_hook();
    STREQ(slot(0), "128-1.lo", "and steps back off");

    /* ---------------------------------------------------------------- */
    GROUP("+3");
    boot(MACHINE_PLUS3);
    map4(m4, sizeof m4);
    STREQ(m4, "p3-0.lo ram5.lo ram2.lo ram0.lo", "initial map");
    /* The other half of the same regression: #1FFD must reach its OWN
     * latch. (0x1FFD & 0x8002) == 0 is also true, so an ordering or
     * nesting slip sends it to #7FFD instead. */
    out(0x1FFD, 0x04);
    EQ(spec_port_1ffd, 0x04, "#1FFD reaches the second latch");
    EQ(spec_port_7ffd, 0x00, "#1FFD does not touch #7FFD");
    STREQ(slot(0), "p3-2.lo", "#1FFD bit 2 is the ROM number's high bit");
    /* Four ROMs from the two latches. */
    static const char *p3rom[4] = { "p3-0.lo", "p3-1.lo", "p3-2.lo", "p3-3.lo" };
    for (unsigned rn = 0; rn < 4; rn++) {
        out(0x1FFD, (uint8_t)((rn & 2) ? 0x04 : 0x00));
        out(0x7FFD, (uint8_t)((rn & 1) ? 0x10 : 0x00));
        STREQ(slot(0), p3rom[rn], "ROM %u", rn);
        CHECK(ro(0), "ROM %u read-only", rn);
    }
    /* Special mode: no ROM, four banks straight through. */
    static const char *cfg[4] = {
        "ram0.lo ram1.lo ram2.lo ram3.lo",
        "ram4.lo ram5.lo ram6.lo ram7.lo",
        "ram4.lo ram5.lo ram6.lo ram3.lo",
        "ram4.lo ram7.lo ram6.lo ram3.lo",
    };
    for (unsigned c = 0; c < 4; c++) {
        out(0x1FFD, (uint8_t)(0x01 | (c << 1)));
        map4(m4, sizeof m4);
        STREQ(m4, cfg[c], "special mode config %u", c);
        CHECK(!ro(0), "special mode: #0000 is RAM, writable");
    }
    out(0x1FFD, 0x00);
    out(0x7FFD, 0x00);
    STREQ(slot(0), "p3-0.lo", "back out of special mode");

    /* ---------------------------------------------------------------- */
    GROUP("Scorpion");
    boot(MACHINE_SCORPION);
    map4(m4, sizeof m4);
    STREQ(m4, "sc-0.lo ram5.lo ram2.lo ram0.lo", "initial map");
    out(0x7FFD, 0x10);
    STREQ(slot(0), "sc-1.lo", "#7FFD bit 4 -> ROM 1");
    /* Its #1FFD needs A5, or an OUT (n),A below 0x20 would repage it. */
    out(0x1FFD, 0x02);
    EQ(spec_port_1ffd, 0x02, "#1FFD with A5 set is the second latch");
    STREQ(slot(0), "sc-2.lo", "#1FFD bit 1 forces the service ROM");
    out(0x0002, 0x00);                       /* A5 clear: not #1FFD */
    EQ(spec_port_1ffd, 0x02, "#1FFD needs A5");
    out(0x1FFD, 0x01);
    STREQ(slot(0), "ram0.lo", "#1FFD bit 0 puts RAM at #0000");
    CHECK(!ro(0), "that RAM is writable");
    out(0x1FFD, 0x00);
    /* 256 KB: #1FFD bit 4 is the fourth page bit. */
    out(0x7FFD, 0x01); out(0x1FFD, 0x10);
    CHECK(z80_rpage[6] != spec_ram[1], "bank 9 is not bank 1");
    out(0x1FFD, 0x00);
    STREQ(slot(6), "ram1.lo", "and back to bank 1");

    /* ---------------------------------------------------------------- */
    GROUP("Profi");
    boot(MACHINE_PROFI);
    map4(m4, sizeof m4);
    /*
     * Its BASIC-128 is byte-for-byte the Pentagon's image, so that is
     * the name this reports. The files arrived in the order service,
     * TR-DOS, BASIC-128, BASIC-48 and were loaded 0,1,2,3, which put
     * the service monitor at #0000 and left the machine polling a disk
     * interface that was not there.
     */
    STREQ(m4, "pentagon.lo ram5.lo ram2.lo ram0.lo", "initial map");
    EQ(z80_rpage[0][0x38], 0xE5, "ROM 0 is an editor ROM, not the monitor");
    out(0xDFFD, 0x40);
    EQ(spec_port_dffd, 0x40, "#DFFD is the third latch");
    STREQ(slot(4), "ram6.lo", "#DFFD bit 6 puts page 6 at #8000");
    out(0xDFFD, 0x10);
    STREQ(slot(0), "ram0.lo", "#DFFD bit 4 puts RAM at #0000");
    /* #DFFD is decoded on all sixteen bits. */
    out(0xDFFC, 0x00);
    EQ(spec_port_dffd, 0x10, "#DFFD is a full 16-bit decode");
    /* D4 also breaks the paging lock, which is the point of it. */
    out(0x7FFD, 0x20);
    out(0x7FFD, 0x10);
    STREQ(slot(0), "ram0.lo", "locked, and RAM still at #0000");
    out(0xDFFD, 0x10);          /* D4 set: clears the lock */
    out(0xDFFD, 0x00);
    out(0x7FFD, 0x10);
    STREQ(slot(0), "pr-1.lo", "#DFFD D4 released the lock");

    /* ---------------------------------------------------------------- */
    GROUP("Timex");
    boot(MACHINE_TIMEX);
    map4(m4, sizeof m4);
    STREQ(m4, "tx-home.lo ram5.lo ram2.lo ram0.lo", "initial HOME map");
    CHECK(ro(0) && ro(1), "home ROM read-only");
    CHECK(!ro(2), "#4000 writable");
    /* The HSR is one bit per 8 KB chunk -- the reason the bus engine
     * has eight windows rather than four. */
    spec_timex_exrom = true;
    out(0x00F4, 0x01);
    EQ(spec_timex_hsr, 0x01, "#F4 is the horizontal select register");
    STREQ(slot(0), "tx-ex.lo", "chunk 0 from the EX-ROM");
    STREQ(slot(1), "tx-home.hi", "chunk 1 still HOME");
    out(0x00F4, 0xFF);
    for (unsigned i = 0; i < 8; i++)
        STREQ(slot(i), "tx-ex.lo", "chunk %u from the EX-ROM", i);
    /* #F4 is decoded on the low byte only. */
    out(0x12F4, 0x00);
    EQ(spec_timex_hsr, 0x00, "#F4 decodes on the low byte");
    map4(m4, sizeof m4);
    STREQ(m4, "tx-home.lo ram5.lo ram2.lo ram0.lo", "back to HOME");
    /* With no EX-ROM the selected chunks come from the dock. */
    spec_timex_exrom = false;
    memset(stub_dock[3], 0xA5, 8192);
    out(0x00F4, 0x08);
    EQ(z80_rpage[3][0], 0xA5, "chunk 3 from the cartridge dock");
    CHECK(ro(3), "an unwritable dock chunk is read-only");

    /* ---------------------------------------------------------------- */
    /*
     * The banks above the first eight are in PSRAM behind one 16 KB
     * staging page, so two of them are the same pointer and only their
     * contents differ. A switch has to write the outgoing bank back
     * first; without that, a program that pages one high bank out,
     * touches another and pages the first back in finds its own writes
     * in the wrong place.
     */
    GROUP("PSRAM bank staging");
    boot(MACHINE_PROFI);
    /* #DFFD's three bits are the group and #7FFD's three the page
     * within it: eight groups of eight, 1 MB. */
    for (unsigned g = 0; g < 8; g++) {
        for (unsigned p = 0; p < 8; p++) {
            out(0xDFFD, (uint8_t)g);
            out(0x7FFD, (uint8_t)p);
            z80_wpage[6][0]    = (uint8_t)(g * 8 + p);
            z80_wpage[7][8191] = (uint8_t)~(g * 8 + p);
        }
    }
    unsigned wrong = 0;
    for (unsigned g = 0; g < 8; g++) {
        for (unsigned p = 0; p < 8; p++) {
            out(0xDFFD, (uint8_t)g);
            out(0x7FFD, (uint8_t)p);
            const uint8_t want = (uint8_t)(g * 8 + p);
            if (z80_rpage[6][0] != want)            wrong++;
            if (z80_rpage[7][8191] != (uint8_t)~want) wrong++;
        }
    }
    EQ(wrong, 0, "all 64 banks keep their contents across switches");
    /* The interleaved case, which is the one a missing flush breaks. */
    out(0xDFFD, 5); out(0x7FFD, 2); z80_wpage[6][0x20] = 0x5A;
    out(0xDFFD, 1); out(0x7FFD, 6); z80_wpage[6][0x20] = 0xA5;
    out(0xDFFD, 5); out(0x7FFD, 2); EQ(z80_rpage[6][0x20], 0x5A, "bank 42 after a detour");
    out(0xDFFD, 1); out(0x7FFD, 6); EQ(z80_rpage[6][0x20], 0xA5, "bank 14 after a detour");

    /* The Scorpion's 256 KB: #1FFD D4 is the fourth page bit. */
    boot(MACHINE_SCORPION);
    for (unsigned b = 0; b < 16; b++) {
        out(0x1FFD, (uint8_t)((b & 8) ? 0x10 : 0x00));
        out(0x7FFD, (uint8_t)(b & 7));
        z80_wpage[6][0x40] = (uint8_t)(0xC0 + b);
    }
    wrong = 0;
    for (unsigned b = 0; b < 16; b++) {
        out(0x1FFD, (uint8_t)((b & 8) ? 0x10 : 0x00));
        out(0x7FFD, (uint8_t)(b & 7));
        if (z80_rpage[6][0x40] != (uint8_t)(0xC0 + b)) wrong++;
    }
    EQ(wrong, 0, "all 16 Scorpion banks keep their contents");
    /* The low eight are resident, so they are never staged and must
     * still be the very pages the rest of the machine sees. */
    out(0x1FFD, 0x00); out(0x7FFD, 0x03);
    STREQ(slot(6), "ram3.lo", "a low bank is the real page, not a stage");

    /* ---------------------------------------------------------------- */
    GROUP("the 48K family");
    boot(MACHINE_16K);
    EQ(z80_rpage[4][0], 0xFF, "a 16K has nothing at #8000");
    EQ(z80_rpage[7][8191], 0xFF, "...or at #FFFF");
    z80_wpage[4][0] = 0x12;
    EQ(z80_rpage[4][0], 0xFF, "and writing there changes nothing");
    STREQ(slot(2), "ram5.lo", "but its 16 KB is where a 48K's screen is");
    boot(MACHINE_TC2048);
    STREQ(slot(2), "ram5.lo", "a TC2048 is a 48K underneath");
    STREQ(slot(6), "ram0.lo", "...all the way up");
    boot(MACHINE_LENINGRAD1);
    out(0x7FFD, 0x07);
    STREQ(slot(6), "ram0.lo", "a clone has no #7FFD to page with");
    boot(MACHINE_PLUS2A);
    out(0x1FFD, 0x01);
    STREQ(slot(0), "ram0.lo", "a +2A has the +3's all-RAM modes");
    out(0x1FFD, 0x07);
    STREQ(slot(0), "ram4.lo", "...all four of them");

    /* ---------------------------------------------------------------- */
    GROUP("Pentagon 512 and 1024SL");
    boot(MACHINE_PENT512);
    out(0x7FFD, 0xC3);                            // 3 + 8 + 16
    z80_wpage[6][0] = 0x5B;
    out(0x7FFD, 0x03);
    CHECK(z80_rpage[6][0] != 0x5B, "bank 27 is not bank 3");
    out(0x7FFD, 0xC3);
    EQ(z80_rpage[6][0], 0x5B, "and comes back");
    boot(MACHINE_PENT1024);
    out(0x7FFD, 0x23);                            // bit 5 is a page bit here
    z80_wpage[6][1] = 0x6C;
    out(0x7FFD, 0x03);
    EQ(z80_rpage[6][1] == 0x6C, 0, "bank 35 is not bank 3");
    out(0x7FFD, 0x23);
    EQ(z80_rpage[6][1], 0x6C, "a 1024SL pages bank 35 without locking");
    out(0xEFF7, 0x04);                            // 128K mode: bit 5 locks again
    out(0x7FFD, 0x20);
    out(0x7FFD, 0x01);
    STREQ(slot(6), "ram0.lo", "...until #EFF7 bit 2 makes it a lock");
    boot(MACHINE_PENT1024);
    out(0xEFF7, 0x01);
    EQ(spec_pent16c, true, "#EFF7 bit 0 is the 16-colour mode");
    z80_io_in(0x00FB, 0);
    CHECK(z80_wpage[0] != NULL && z80_rpage[0] == z80_wpage[0], "IN #FB puts writable cache at #0000");
    z80_io_in(0x007B, 0);
    STREQ(slot(0), "pentagon.lo", "IN #7B puts the ROM back");

    /* ---------------------------------------------------------------- */
    GROUP("Scorpion family");
    {
        extern const uint8_t spec_romprof[], spec_romkay1024[];
        boot(MACHINE_KAY1024);
        out(0x0001, 0x80);                            // 1FFD D7: +16
        out(0x7FFD, 0x81);                            // 7FFD D7: +32, low bits 1
        z80_wpage[6][2] = 0x77;
        out(0x0001, 0x00);
        out(0x7FFD, 0x01);
        CHECK(z80_rpage[6][2] != 0x77, "bank 49 is not bank 1");
        out(0x0001, 0x80);
        out(0x7FFD, 0x81);
        EQ(z80_rpage[6][2], 0x77, "a KAY-1024 reaches bank 49");
        out(0x0001, 0x00);
        out(0x7FFD, 0x10);
        CHECK(!memcmp(z80_rpage[0], spec_romkay1024 + 16384, 16), "KAY: #7FFD D4 is its BASIC 48");
        out(0x0001, 0x08);
        CHECK(!memcmp(z80_rpage[0], spec_romkay1024 + 2 * 16384, 16), "...and #1FFD D3 swaps the pair (TR-DOS, page 2 of the image)");

        boot(MACHINE_ZS1024);
        out(0x1FFD, 0x02);                            // the service ROM, which arms the tap
        CHECK(!memcmp(z80_rpage[0], spec_romprof + 2 * 16384, 16), "ProfROM plane 0, service bank");
        CHECK(z80_rtap_cb != NULL && z80_rtap_base == 0x0100, "and the plane switch is watching reads of #0100-#010F");
        z80_rtap_cb(0x0101, 0);                       // row 0 keeps the plane
        CHECK(!memcmp(z80_rpage[0], spec_romprof + 2 * 16384, 16), "a read of #0101 stays on plane 0");
        z80_rtap_cb(0x010C, 0);                       // row 3, from plane 0 -> plane 1
        CHECK(!memcmp(z80_rpage[0], spec_romprof + (4 + 2) * 16384, 16), "a read of #010C is plane 1");
        out(0x1FFD, 0xC0);                            // D6,D7: +16, +32
        out(0x7FFD, 0x05);
        z80_wpage[6][3] = 0x99;
        out(0x1FFD, 0x00);
        CHECK(z80_rpage[6][3] != 0x99, "bank 53 is not bank 5");
        out(0x1FFD, 0xC0);
        EQ(z80_rpage[6][3], 0x99, "a ZS-1024 reaches bank 53");
    }

    /* ---------------------------------------------------------------- */
    GROUP("Profi serial mouse");
    {
        extern uint8_t z80_int_vector;
        boot(MACHINE_PROFI);
        out(0x00F3, 0x04);                            // RxE: the receiver on
        spec_mouse_buttons = 1;                       // left
        spec_mouse_dx = 20; spec_mouse_dy = -6;      // right 10, up 3 at half rate
        EQ(z80_io_in(0x00F3, 0), 0x07, "#F3 says a byte waits (TxRDY+TxE+RxRDY)");
        EQ(z80_io_in(0x00D3, 0), 0x40 | 0x20 | 0x0C | 0x00, "byte 1: sync, left, dy high bits");
        EQ(z80_io_in(0x00D3, 0), 10, "byte 2: dx");
        EQ(z80_io_in(0x00D3, 0), (uint8_t)(-3) & 0x3F, "byte 3: dy");
        EQ(z80_io_in(0x00F3, 0), 0x05, "and nothing waits after the packet");
        out(0x7FFD, 0x00);
        out(0xDFFD, 0x20);                            // CP/M
        out(0x0093, 0x01);                            // the interrupt on
        spec_mouse_dx = 4;
        z80_io_in(0x00F3, 0);
        EQ(spec_port_dffd, 0x20, "CP/M is on (#DFFD D5)");
        EQ(z80_int_vector, 0xE7, "a waiting byte in CP/M puts RST #20 on the bus");
        z80_io_in(0x00D3, 0); z80_io_in(0x00D3, 0); z80_io_in(0x00D3, 0);
        EQ(z80_int_vector, 0xFF, "and the packet read gives the bus back");
        out(0xDFFD, 0x00);
    }

    GROUP("ATM-Turbo 3, 4 MB");
    {
        boot(MACHINE_ATM3);
        EQ(spec_ram_banks(), 256, "the ATM-Turbo 3 has 256 banks");
        out(0x0177, 0x03);                            // the manager on; /CPM down: shadow ports open
        out(0xFFF7, 0x40 | (~0u & 0x3F));             // window 3: RAM, page from the register
        out(0xF7F7, (uint8_t)~255u);                  // #x7F7 (A11=0): the 8-bit page, 255
        z80_wpage[6][5] = 0xA5;
        EQ(z80_rpage[6][5], 0xA5, "page 255 is writable");
        out(0xF7F7, (uint8_t)~63u);                   // page 63 -- what 255 used to alias to
        CHECK(z80_rpage[6][5] != 0xA5, "page 63 is not page 255");
        out(0xF7F7, (uint8_t)~255u);
        EQ(z80_rpage[6][5], 0xA5, "and page 255 kept its byte");
        boot(MACHINE_ATM2P);
        EQ(spec_ram_banks(), 64, "an ATM-Turbo 2+ is back to 64");
    }

    GROUP("ATM-Turbo");
    {
        extern const uint8_t atm_rom2_107[];
        boot(MACHINE_ATM2P);
        CHECK(!memcmp(z80_rpage[0], atm_rom2_107 + 3 * 16384, 16), "reset: the BIOS at #0000");
        CHECK(!memcmp(z80_rpage[6], atm_rom2_107 + 3 * 16384, 16), "...and in every window");
        out(0x0177, 0x03);                            // #77 with A8: the manager on, ZX mode
        out(0xC0F7, (uint8_t)(0x40 | (~10 & 0x3F)));  // window 3, set 0: RAM page 10 (D7=0: the page from here)
        out(0x40F7, (uint8_t)(0x40 | (~5 & 0x3F)));   // window 1: RAM 5
        z80_wpage[6][4] = 0x3C;
        EQ(z80_rpage[6][4], 0x3C, "RAM page 10 at #C000 is writable");
        out(0xC0F7, (uint8_t)(0x40 | (~2 & 0x3F)));   // now page 2 there
        CHECK(z80_rpage[6][4] != 0x3C || z80_rpage[6] == spec_ram[2], "a different page");
        STREQ(slot(6), "ram2.lo", "page 2 is the SRAM bank");
        out(0xC0F7, (uint8_t)(0x40 | (~10 & 0x3F)));
        EQ(z80_rpage[6][4], 0x3C, "and page 10 kept its byte");
        STREQ(slot(2), "ram5.lo", "window 1 follows its register");
    }

    /* ---------------------------------------------------------------- */
    GROUP("contention and the floating bus");
    boot(MACHINE_48K);
    settings.contention = 1;
    spec_init();
    EQ(z80_contend.on, true, "a 48K contends");
    EQ(z80_contend.mask, 0x02, "only the window at 0x4000");
    EQ(z80_contend.t0, 14335u, "from Fuse's first contended T-state");
    spec_ram[5][0x0000] = 0x81;                   // bitmap, top-left
    spec_ram[5][0x1800] = 0x47;                   // its attribute
    spec_ram[5][0x0001] = 0x82;
    spec_ram[5][0x1801] = 0x46;
    EQ(z80_io_in(0x40FF, 14335), 0x81, "the bitmap byte under the beam");
    EQ(z80_io_in(0x40FF, 14336), 0x47, "then its attribute");
    EQ(z80_io_in(0x40FF, 14337), 0x82, "then the next cell's bitmap");
    EQ(z80_io_in(0x40FF, 14338), 0x46, "and its attribute");
    EQ(z80_io_in(0x40FF, 14339), 0xFF, "idle for the rest of the eight");
    EQ(z80_io_in(0x40FF, 14335 + 130), 0xFF, "the right border floats high");
    EQ(z80_io_in(0x40FF, 100), 0xFF, "and so does the top border");

    boot(MACHINE_128K);
    settings.contention = 1;
    spec_init();
    out(0x7FFD, 0x03);
    EQ(z80_contend.mask, 0x0A, "a 128K with bank 3 paged in contends the top window too");
    out(0x7FFD, 0x02);
    EQ(z80_contend.mask, 0x02, "...and not with bank 2");

    boot(MACHINE_PENTAGON);
    settings.contention = 1;
    spec_init();
    EQ(z80_contend.on, false, "a Pentagon does not contend");
    EQ(z80_io_in(0x40FF, 14335), 0xFF, "and its floating bus is high");

    return tap_done("paging");
}
