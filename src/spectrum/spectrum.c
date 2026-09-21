/*
 * frank-dino — an RP2350B acting as the whole machine around a real Z80
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-dino
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "spectrum.h"

#include <string.h>
#include <pico/time.h>
#include <hardware/gpio.h>

#include "audio.h"
#include "ay.h"
#include "beta.h"
#include "dck.h"
#include "divmmc.h"
#include "fdc.h"
#include "dino.h"
#include "settings.h"
#include "tape.h"
#include "z80_bus.h"

// ============================================================================
// The ROMs
// ============================================================================
//
// Welded into the binary rather than read off the card: the board has a
// microSD slot that nothing reads yet, and a machine that cannot start
// without a file on a card is a machine that cannot be tested by a
// script. See roms/README.md for where these two came from.
/*
 * The ROM masters live in flash; what the CPU reads is a copy in SRAM.
 *
 * The bus engine reads the bottom window on every opcode fetch, and an
 * XIP miss there stretches a clock phase by a microsecond -- so the
 * pages that are mapped have to be in SRAM. But only the running
 * machine's are mapped, and ten images at sixteen kilobytes is a hundred
 * and sixty kilobytes of a part with five hundred and twelve.
 *
 * So these stay in flash and rom_window[] below holds the set the
 * current machine can page. Filling it costs a memcpy on a machine
 * switch, which already resets the machine, and it means adding a
 * machine costs flash rather than SRAM.
 */
/* Overridable so the host tests can supply a Mach-O form of the same
 * thing and link the real ROM images rather than a stand-in. */
#ifndef IMPORT_BIN
#define IMPORT_BIN(file, sym) asm (\
    ".section .rodata." #sym "\n" \
    ".balign 4\n"                 \
    ".global " #sym "\n"          \
    #sym ":\n"                    \
    ".incbin \"" file "\"\n"      \
    ".balign 4\n"                 \
    ".section \".text\"\n")
#endif

IMPORT_BIN("roms/128-0.rom", spec_rom0);
IMPORT_BIN("roms/128-1.rom", spec_rom1);
IMPORT_BIN("roms/48.rom",    spec_rom48);
IMPORT_BIN("roms/plus2.rom", spec_romp2);
IMPORT_BIN("roms/plus3-0.rom", spec_romp3_0);
IMPORT_BIN("roms/plus3-1.rom", spec_romp3_1);
IMPORT_BIN("roms/plus3-2.rom", spec_romp3_2);
IMPORT_BIN("roms/plus3-3.rom", spec_romp3_3);
IMPORT_BIN("roms/scorpion-0.rom", spec_romsc_0);
IMPORT_BIN("roms/scorpion-1.rom", spec_romsc_1);
IMPORT_BIN("roms/scorpion-2.rom", spec_romsc_2);
IMPORT_BIN("roms/scorpion-3.rom", spec_romsc_3);
IMPORT_BIN("roms/profi-0.rom", spec_rompr_0);
IMPORT_BIN("roms/profi-1.rom", spec_rompr_1);
IMPORT_BIN("roms/profi-2.rom", spec_rompr_2);
IMPORT_BIN("roms/profi-3.rom", spec_rompr_3);
IMPORT_BIN("roms/timex-home.rom", spec_romtx_home);
IMPORT_BIN("roms/timex-exrom.rom", spec_romtx_ex);
IMPORT_BIN("roms/esxdos.rom", spec_romesx);
IMPORT_BIN("roms/pentagon-0.rom", spec_rompent);
IMPORT_BIN("roms/trdos.rom", spec_romtrd);
extern const uint8_t spec_rom0[SPEC_BANK_SIZE];
extern const uint8_t spec_rom1[SPEC_BANK_SIZE];
extern const uint8_t spec_rom48[SPEC_BANK_SIZE];
extern const uint8_t spec_romp2[SPEC_BANK_SIZE];
extern const uint8_t spec_romp3_0[SPEC_BANK_SIZE];
extern const uint8_t spec_romp3_1[SPEC_BANK_SIZE];
extern const uint8_t spec_romp3_2[SPEC_BANK_SIZE];
extern const uint8_t spec_romp3_3[SPEC_BANK_SIZE];
extern const uint8_t spec_romsc_0[SPEC_BANK_SIZE];
extern const uint8_t spec_romsc_1[SPEC_BANK_SIZE];
extern const uint8_t spec_romsc_2[SPEC_BANK_SIZE];
extern const uint8_t spec_romsc_3[SPEC_BANK_SIZE];
extern const uint8_t spec_rompr_0[SPEC_BANK_SIZE];
extern const uint8_t spec_rompr_1[SPEC_BANK_SIZE];
extern const uint8_t spec_rompr_2[SPEC_BANK_SIZE];
extern const uint8_t spec_rompr_3[SPEC_BANK_SIZE];
extern const uint8_t spec_romtx_home[SPEC_BANK_SIZE];
extern const uint8_t spec_romtx_ex[8192];
extern const uint8_t spec_romesx[8192];
extern const uint8_t spec_rompent[SPEC_BANK_SIZE];
extern const uint8_t spec_romtrd[SPEC_BANK_SIZE];

/*
 * The pages the running machine can put in the bottom window.
 *
 * Four ROM slots because that is what the widest machine here pages --
 * a +3 selects one of four with #7FFD bit 4 and #1FFD bit 2 -- plus the
 * Beta Disk's, which can replace any of them. Machines with fewer ROMs
 * leave the spare slots holding whatever was copied last; nothing maps
 * them, so nothing reads them.
 */
enum { ROMW_0 = 0, ROMW_1, ROMW_2, ROMW_3, ROMW_TRDOS, ROMW_SLOTS };
static uint8_t rom_window[ROMW_SLOTS][SPEC_BANK_SIZE] __attribute__((aligned(4)));

/*
 * Which window slot holds the 48K BASIC ROM on this machine.
 *
 * It is the one with LD-BYTES in it, so it is the one the tape trap
 * patches -- and it moves: a 48K has it as its only ROM, the 128K
 * family as ROM 1, and a +3 as ROM 3.
 */
static unsigned basic_slot(const machine_t m) {
    if (m == MACHINE_48K)   return ROMW_0;
    if (m == MACHINE_PLUS3) return ROMW_3;
    if (m == MACHINE_TIMEX) return ROMW_0;      /* not a Sinclair ROM; no trap lands */
    return ROMW_1;
}

uint8_t *spec_basic_slot(void) { return rom_window[basic_slot(spec_machine)]; }

/* The flash master a slot is filled from, per machine. */
static void rom_load_window(const machine_t m) {
    if (m == MACHINE_SCORPION || m == MACHINE_PROFI) {
        /*
         * Four each: 0 BASIC-128, 1 BASIC-48, 2 the service monitor and
         * 3 TR-DOS. The Scorpion's ROM number is built from three
         * latches and the Profi's from two, but the slots mean the same
         * thing in both.
         */
        const uint8_t *const *r = (m == MACHINE_SCORPION)
            ? (const uint8_t *const[]){ spec_romsc_0, spec_romsc_1, spec_romsc_2, spec_romsc_3 }
            : (const uint8_t *const[]){ spec_rompr_0, spec_rompr_1, spec_rompr_2, spec_rompr_3 };
        for (int i = 0; i < 4; i++) memcpy(rom_window[i], r[i], SPEC_BANK_SIZE);
        memcpy(rom_window[ROMW_TRDOS], spec_romtrd, SPEC_BANK_SIZE);
        return;
    }

    if (m == MACHINE_TIMEX) {
        /* One 16 KB home ROM, and the 8 KB EX-ROM in the slot above it. */
        memcpy(rom_window[ROMW_0], spec_romtx_home, SPEC_BANK_SIZE);
        memcpy(rom_window[ROMW_1], spec_romtx_home, SPEC_BANK_SIZE);
        memcpy(rom_window[ROMW_2], spec_romtx_ex, 8192);
        memcpy(rom_window[ROMW_TRDOS], spec_romtrd, SPEC_BANK_SIZE);
        return;
    }

    if (m == MACHINE_PLUS3) {
        /* Four of its own: 0 editor, 1 syntax, 2 +3DOS, 3 48K BASIC. */
        memcpy(rom_window[ROMW_0], spec_romp3_0, SPEC_BANK_SIZE);
        memcpy(rom_window[ROMW_1], spec_romp3_1, SPEC_BANK_SIZE);
        memcpy(rom_window[ROMW_2], spec_romp3_2, SPEC_BANK_SIZE);
        memcpy(rom_window[ROMW_3], spec_romp3_3, SPEC_BANK_SIZE);
        memcpy(rom_window[ROMW_TRDOS], spec_romtrd, SPEC_BANK_SIZE);
        return;
    }

    /*
     * The +2's image is a 48 BASIC, so it is that machine's ROM 1.
     *
     * It has F5 at 0x0038 -- the real interrupt handler -- where an
     * editor ROM has the E5 four-push stub that jumps to the SWAP
     * routine. Loading it as ROM 0 gave a +2 that came up in 48 BASIC
     * with its own menu unreachable, which looks enough like a working
     * machine to be missed.
     *
     * The editor underneath it is the 128K's. A real +2's differs from
     * it in the copyright line and little else, and pico-speccy does
     * not carry a separate one either.
     */
    const uint8_t *r0 = (m == MACHINE_PENTAGON) ? spec_rompent
                      : (m == MACHINE_48K)      ? spec_rom48
                                                : spec_rom0;
    const uint8_t *r1 = (m == MACHINE_48K)   ? spec_rom48
                      : (m == MACHINE_PLUS2) ? spec_romp2
                                             : spec_rom1;

    memcpy(rom_window[ROMW_0], r0, SPEC_BANK_SIZE);
    memcpy(rom_window[ROMW_1], r1, SPEC_BANK_SIZE);
    memcpy(rom_window[ROMW_TRDOS], spec_romtrd, SPEC_BANK_SIZE);
}

machine_t spec_machine = MACHINE_128K;

const char *spec_machine_name(void) {
    switch (spec_machine) {
        case MACHINE_48K:      return "ZX Spectrum 48K";
        case MACHINE_PLUS2:    return "ZX Spectrum +2";
        case MACHINE_PLUS3:    return "ZX Spectrum +3";
        case MACHINE_SCORPION: return "Scorpion ZS-256";
        case MACHINE_PROFI:    return "Profi 1024";
        case MACHINE_TIMEX:    return "Timex TC2068";
        case MACHINE_PENTAGON: return "Pentagon 128";
        default:               return "ZX Spectrum 128K";
    }
}

/*
 * Overclocking a real Z80, in halves.
 *
 * The clock is generated here, edge by edge, and the part in the socket
 * is a Z84C00-10 -- so a faster Z80 is not a trick, it is the CPU
 * actually running faster. turbo_num() is the multiplier over two: 2 is
 * 3.5 MHz, 3 is 5.3, 4 is 7.
 *
 * What changes is how much work fits between two interrupts. The frame
 * is still a fiftieth of a second, so a faster CPU means more T-states
 * in it -- which is why the frame length is a function rather than a
 * constant, and why the AY has to be told about it: the chip has its own
 * 1.7734 MHz and must keep it, or every tune plays sharp.
 */
static inline uint32_t turbo_num(void) { return 2u + settings.turbo; }

uint32_t spec_frame_tstates(void) {
    uint32_t base;
    switch (spec_machine) {
        case MACHINE_48K:      base = SPEC_TSTATES_PER_FRAME_48;   break;
        case MACHINE_PENTAGON: base = SPEC_TSTATES_PER_FRAME_PENT; break;
        default:               base = SPEC_TSTATES_PER_FRAME;      break;
    }
    return base * turbo_num() / 2;
}

// ============================================================================
// State
// ============================================================================
uint8_t spec_ram[SPEC_RAM_BANKS][SPEC_BANK_SIZE] __attribute__((aligned(4)));

/* The banks above the first eight, and the one of them that is mapped. */
static uint8_t spec_ram_ext[SPEC_MAX_BANKS - SPEC_RAM_BANKS][SPEC_BANK_SIZE]
    __attribute__((section(".psram")));
static uint8_t bank_stage[SPEC_BANK_SIZE] __attribute__((aligned(4)));
static int     staged_bank = -1;

void spec_bank_flush(void) {
    if (staged_bank < (int)SPEC_RAM_BANKS) return;
    memcpy(spec_ram_ext[staged_bank - SPEC_RAM_BANKS], bank_stage, SPEC_BANK_SIZE);
    staged_bank = -1;
}

/*
 * This runs inside an OUT, on the clock path.
 *
 * A switch between two high banks is two 16 KB PSRAM transfers, a few
 * hundred microseconds, and for that long the Z80's clock stops
 * advancing. The CPU is static so nothing is lost, but the frame it
 * happens in is late -- which is the price of a 1 MB machine on a part
 * with 512 KB, and the reason the early return below matters: the cost
 * is per actual change of the mapped high bank, not per paging write.
 */
uint8_t *spec_bank_ptr(const unsigned bank) {
    if (bank < SPEC_RAM_BANKS) return spec_ram[bank];
    if (bank >= SPEC_MAX_BANKS) return spec_ram[bank & 7u];   /* nothing there */
    if (staged_bank == (int)bank) return bank_stage;

    /*
     * The outgoing bank has to go back first. A program that pages one
     * high bank out, writes to another and pages the first back in
     * would otherwise find its own writes in the wrong place -- which
     * is the bug this whole arrangement exists to avoid and the only
     * one it can introduce.
     */
    spec_bank_flush();
    memcpy(bank_stage, spec_ram_ext[bank - SPEC_RAM_BANKS], SPEC_BANK_SIZE);
    staged_bank = (int)bank;
    return bank_stage;
}

volatile uint8_t  spec_screen_bank = 5;
volatile uint8_t  spec_port_1ffd   = 0;   /* the +3's and Scorpion's second latch */
volatile uint8_t  spec_port_dffd   = 0;   /* the Profi's third */
volatile uint8_t  spec_timex_hsr   = 0;   /* the Timex's 8 KB chunk select */
volatile bool     spec_timex_exrom = false;
volatile uint8_t  spec_border      = 7;
volatile bool     spec_flash       = false;

/* ULA+, see spectrum.h. */
volatile bool    spec_ulaplus_on;
volatile uint8_t spec_ulaplus_reg;
uint8_t          spec_ulaplus_pal[64];
volatile bool    spec_ulaplus_dirty;

/* Timex SCLD, see spectrum.h. */
volatile uint8_t spec_timex_mode;
volatile uint8_t spec_timex_ink;
volatile uint32_t spec_frames      = 0;
volatile uint8_t  spec_port_7ffd   = 0;

// Once 0x7FFD bit 5 is set the machine cannot page again until it is
// reset. That is how a 128K pretends to be a 48K, and how the 48K BASIC
// mode in the menu works — so it has to be honoured, not ignored.
static bool paging_locked = false;

// Keyboard: bit set = key up. Five keys per half-row, bits 0-4.
static volatile uint8_t kbd[8];

/*
 * The matrix, as the ULA sees it.
 *
 * Row order is address-line order — row 0 is A8 — and within a row bit 0
 * is the key nearest the outside of the keyboard. Both halves run inward
 * from the edges, which is why 1-5 and 0-6 look reversed with respect to
 * each other and are not a typo.
 */
enum { R_CSZXCV = 0, R_ASDFG, R_QWERT, R_12345, R_09876, R_POIUY, R_ENTER, R_SPACE };

// ---------------------------------------------------------------------------
// Sound
// ---------------------------------------------------------------------------
volatile bool     spec_beeper;
volatile uint32_t spec_ay_writes;

/*
 * Does this machine have an AY at all?
 *
 * A 128K and a +2 always do. A 48K never did from Sinclair, but half the
 * clones and a good many add-on boards put one on the same two ports,
 * and a fair amount of 48K music expects to find it -- so it is an
 * option rather than a fact about the machine.
 */
static inline bool have_ay(void) {
    return spec_machine != MACHINE_48K || settings.ay48;
}

static ay_t    ay[2];
static uint8_t ay_sel;          // which chip the ports are talking to

// The last byte written to the ULA's port, which the EAR bit reads back.
static volatile uint8_t last_fe;

/*
 * Two ways of bolting a real DAC onto a machine that has none.
 *
 * Covox is one 8-bit latch on port #FB, and SounDrive (the "Covox quad")
 * is four of them, two per side. Both are just a byte that goes straight
 * to the speaker, so there is nothing to model: the mixer adds the
 * latched value in and the guest does the rest. Off by default, because
 * a machine that never had one must not have writes to those ports turn
 * into noise.
 */
static volatile uint8_t covox_level;
static volatile uint8_t sd_level[4];

// ============================================================================
// Paging
// ============================================================================

/*
 * A stand-in for the 48K BASIC ROM, or NULL.
 *
 * src/spectrum/tape.c installs a patched copy of that ROM here so a call
 * to LD-BYTES lands in its own code instead. It goes through the paging
 * function rather than being mapped directly because the CPU can page the
 * ROM out and back at any moment, and the patch has to survive that: a
 * 128K game loading from tape sits in ROM 1 while it loads and returns to
 * ROM 0 afterwards.
 */

static void apply_paging(void);

/*
 * The 48K BASIC ROM as the CPU sees it.
 *
 * The window slot itself, patched in place by the tape trap rather than
 * copied into a private buffer first. There used to be a second 16 KB
 * of SRAM holding that copy, which was the same image twice over -- and
 * worse, a copy that could be of the WRONG machine's ROM, because a
 * 48K's BASIC and a 128K's ROM 1 are different images.
 *
 * Patching the window cannot go stale: rom_load_window() refills it
 * from flash on every reset, and spec_init() asks the tape to put its
 * stub back afterwards.
 */
static const uint8_t *basic_rom(void) {
    return rom_window[basic_slot(spec_machine)];
}

static void apply_machine_paging(void);
static void apply_divmmc(void);

/*
 * The map, then what is laid over it.
 *
 * apply_machine_paging() has a return for every machine, so an overlay
 * cannot be a few lines at the bottom of it -- it has to be here, after
 * whichever arm ran. The Beta Disk is handled inside because its ROM
 * replaces a whole 16 KB window and every arm needs to know; DivMMC is
 * two 8 KB windows on top of any of them.
 */
static void __not_in_flash_func(apply_paging)(void) {
    apply_machine_paging();
    apply_divmmc();
}

void apply_paging_hook(void) { apply_paging(); }

/*
 * The interface overlay: DivMMC replaces the bottom two 8 KB windows.
 *
 * It sits on top of whatever the machine's own paging worked out, the
 * way the Beta Disk's ROM does -- so the body below decides the map and
 * this has the last word on the first sixteen kilobytes of it.
 */
static void __not_in_flash_func(apply_divmmc)(void) {
    if (!divmmc_mapped()) return;
    uint8_t *lo = divmmc_page_lo();
    z80_map_page8(0, lo, divmmc_lo_writable() ? lo : NULL);
    uint8_t *hi = divmmc_page_hi();
    z80_map_page8(1, hi, hi);
}

static void __not_in_flash_func(apply_machine_paging)(void) {
    const uint8_t v = spec_port_7ffd;

    /*
     * The Beta Disk's ROM sits on top of everything.
     *
     * When the interface has decided its ROM is in -- see spec_opfetch()
     * for the rule, which is about where the CPU is fetching from and
     * nothing else -- it replaces whatever the bottom window would
     * otherwise hold, on every machine. So this runs first and the rest
     * of the function only decides what is underneath it.
     */
    if (unlikely(beta_rom_paged())) {
        z80_map_page(0, rom_window[ROMW_TRDOS], NULL);
        if (spec_machine == MACHINE_48K) {
            z80_map_page(1, spec_ram[5], spec_ram[5]);
            z80_map_page(2, spec_ram[2], spec_ram[2]);
            z80_map_page(3, spec_ram[0], spec_ram[0]);
            spec_screen_bank = 5;
        } else {
            z80_map_page(1, spec_ram[5], spec_ram[5]);
            z80_map_page(2, spec_ram[2], spec_ram[2]);
            z80_map_page(3, spec_ram[v & 7], spec_ram[v & 7]);
            spec_screen_bank = (v & 0x08) ? 7 : 5;
        }
        return;
    }

    /*
     * A 48K has one ROM and no paging at all: whatever is in the latch,
     * the map is fixed. Modelling that as "the latch exists but is
     * ignored" rather than as a separate code path keeps one function
     * responsible for the memory map.
     */
    if (spec_machine == MACHINE_48K) {
        z80_map_page(0, (uint8_t *)basic_rom(), NULL);
        z80_map_page(1, spec_ram[5], spec_ram[5]);
        z80_map_page(2, spec_ram[2], spec_ram[2]);
        z80_map_page(3, spec_ram[0], spec_ram[0]);
        spec_screen_bank = 5;
        return;
    }

    if (spec_machine == MACHINE_TIMEX) {
        /*
         * The Timex pages in eight 8 KB chunks, which is why the bus
         * engine has eight windows.
         *
         * A clear bit in the HSR means HOME -- the ordinary machine, a
         * 16 KB ROM and 48 KB of RAM. A set bit means the chunk comes
         * from the cartridge dock or the EX-ROM instead, chosen for the
         * WHOLE map by bit 7 of the SCLD's port.
         *
         * The EX-ROM is one 8 KB image and appears in every slot the HSR
         * selects, which is what Fuse and MAME both do. The dock holds
         * whatever .dck is mounted, chunk by chunk, and a chunk the
         * cartridge does not fill reads as an open bus.
         */
        uint8_t *home_rd[8] = {
            rom_window[ROMW_0], rom_window[ROMW_0] + 8192,
            spec_ram[5], spec_ram[5] + 8192,
            spec_ram[2], spec_ram[2] + 8192,
            spec_ram[0], spec_ram[0] + 8192,
        };
        uint8_t *home_wr[8] = {
            NULL, NULL,
            spec_ram[5], spec_ram[5] + 8192,
            spec_ram[2], spec_ram[2] + 8192,
            spec_ram[0], spec_ram[0] + 8192,
        };

        for (unsigned i = 0; i < 8; i++) {
            if (!(spec_timex_hsr & (1u << i))) {
                z80_map_page8(i, home_rd[i], home_wr[i]);
            } else if (spec_timex_exrom) {
                z80_map_page8(i, rom_window[ROMW_2], NULL);
            } else {
                /* The dock. A chunk the cartridge did not fill holds
                 * 0xFF, which is what an empty socket reads. */
                uint8_t *c = dck_chunk(i);
                z80_map_page8(i, c, dck_writable(i) ? c : NULL);
            }
        }
        spec_screen_bank = 5;
        return;
    }

    if (spec_machine == MACHINE_SCORPION) {
        const uint8_t w = spec_port_1ffd;

        /*
         * The ROM number, from MAME's hardware-derived function:
         *
         *     rom = 1FFD D1 ? 2 : ((dos << 1) | 7FFD D4)
         *
         * and the quirk that IS the hardware comes with it -- in DOS
         * with the 128 ROM selected the SERVICE page appears, not
         * TR-DOS. Normal TR-DOS software always runs with D4 set, which
         * is why nobody trips over it.
         */
        const bool dos = beta_rom_paged();
        const unsigned rn = (w & 0x02u) ? 2u
                          : ((dos ? 2u : 0u) | ((v & 0x10u) ? 1u : 0u));

        /* D0 puts RAM bank 0 at #0000 instead of any ROM. */
        if (w & 0x01u) z80_map_page(0, spec_ram[0], spec_ram[0]);
        else           z80_map_page(0, (uint8_t *)((rn == 1) ? basic_rom()
                                                            : rom_window[rn]), NULL);

        z80_map_page(1, spec_ram[5], spec_ram[5]);
        z80_map_page(2, spec_ram[2], spec_ram[2]);

        /* 256 KB: #1FFD D4 is the fourth bit of the #C000 page. */
        const unsigned bank = (v & 0x07u) | ((w & 0x10u) >> 1);
        uint8_t *p = spec_bank_ptr(bank);
        z80_map_page(3, p, p);

        spec_screen_bank = (v & 0x08) ? 7 : 5;
        return;
    }

    if (spec_machine == MACHINE_PROFI) {
        const uint8_t d = spec_port_dffd;

        /* D4 maps RAM page 0 at #0000; otherwise the ordinary pair. */
        if (d & 0x10u) z80_map_page(0, spec_ram[0], spec_ram[0]);
        else           z80_map_page(0, (uint8_t *)((v & 0x10) ? basic_rom()
                                                             : rom_window[ROMW_0]), NULL);

        z80_map_page(1, spec_ram[5], spec_ram[5]);

        /* D6 (SCR) puts page 6 at #8000 in place of page 2. */
        const unsigned b2 = (d & 0x40u) ? 6u : 2u;
        z80_map_page(2, spec_ram[b2], spec_ram[b2]);

        /*
         * 1 MB: #7FFD's three bits are the page within a group and
         * #DFFD's three are the group, so eight groups of eight.
         */
        const unsigned bank = (v & 0x07u) | ((d & 0x07u) << 3);
        uint8_t *p = spec_bank_ptr(bank);
        z80_map_page(3, p, p);

        spec_screen_bank = (v & 0x08) ? 7 : 5;
        return;
    }

    if (spec_machine == MACHINE_PLUS3) {
        const uint8_t w = spec_port_1ffd;

        if (w & 0x01u) {
            /*
             * Special mode: no ROM at all, four banks of RAM straight
             * through, chosen by bits 2-1. This is how +3DOS gets a
             * contiguous 64K to work in, and it is the reason the +3
             * needs a second latch rather than another bit in #7FFD.
             */
            static const uint8_t cfg[4][4] = {
                { 0, 1, 2, 3 },
                { 4, 5, 6, 7 },
                { 4, 5, 6, 3 },
                { 4, 7, 6, 3 },
            };
            const uint8_t *map = cfg[(w >> 1) & 3u];
            for (int i = 0; i < 4; i++)
                z80_map_page(i, spec_ram[map[i]], spec_ram[map[i]]);
            /* The screen still follows #7FFD bit 3, whatever is paged. */
            spec_screen_bank = (v & 0x08) ? 7 : 5;
            return;
        }

        /*
         * Ordinary mode: one of FOUR ROMs, numbered by #1FFD bit 2 as
         * the high bit and #7FFD bit 4 as the low one.
         *
         * ROM 3 is the 48K BASIC, so it is the one the tape trap's
         * patched copy stands in for -- the other three have no
         * LD-BYTES to trap.
         */
        const unsigned rn = (unsigned)((v & 0x10) ? 1u : 0u) | ((w & 0x04u) ? 2u : 0u);
        const uint8_t *rom = (rn == 3) ? basic_rom() : rom_window[rn];
        z80_map_page(0, (uint8_t *)rom, NULL);

        z80_map_page(1, spec_ram[5], spec_ram[5]);
        z80_map_page(2, spec_ram[2], spec_ram[2]);
        const uint8_t b3 = v & 0x07;
        z80_map_page(3, spec_ram[b3], spec_ram[b3]);
        spec_screen_bank = (v & 0x08) ? 7 : 5;
        return;
    }

    // Bottom window: ROM 1 (48K BASIC) when bit 4 is set, ROM 0 otherwise.
    // Read-only, which here means "its write pointer goes to the bucket".
    z80_map_page(0, (uint8_t *)((v & 0x10) ? basic_rom() : rom_window[ROMW_0]), NULL);

    // The two fixed windows never move. Mapped every time anyway, because
    // the cost is two stores on a path taken a few hundred times a second
    // and the alternative is a special case that can be got wrong.
    z80_map_page(1, spec_ram[5], spec_ram[5]);
    z80_map_page(2, spec_ram[2], spec_ram[2]);

    const uint8_t bank = v & 0x07;
    z80_map_page(3, spec_ram[bank], spec_ram[bank]);

    spec_screen_bank = (v & 0x08) ? 7 : 5;
}

// ---------------------------------------------------------------------------
// Launching
// ---------------------------------------------------------------------------
static uint16_t tramp_jp;
static bool     tramp_seen_jp;
static bool     tramp_active;

void spec_set_paging(const uint8_t value) {
    spec_port_7ffd = value;
    apply_paging();
}

static void update_opfetch_hook(void);

/*
 * Everything that has to watch where the CPU is fetching from.
 *
 * Two things do, and they are unrelated except in wanting the same
 * event. The trampoline needs the instruction after its jump, so it can
 * put the real ROM back at the exact moment nothing of it is needed any
 * more. The Beta Disk needs every fetch, because its ROM appearing and
 * disappearing is a function of the address being fetched and of nothing
 * else.
 *
 * This is on the hottest path there is -- once per opcode, a few million
 * times a second -- so it is not installed at all unless one of the two
 * actually wants it. That is what update_opfetch_hook() is for.
 */
static void __not_in_flash_func(spec_opfetch)(const uint16_t addr) {
    if (settings.esxdos) divmmc_opfetch(addr);

    if (unlikely(tramp_active)) {
        /*
         * The first opcode fetch after the trampoline's jump is the
         * destination, and this runs before that fetch is answered — so
         * putting the real ROM back here means the destination's first
         * byte comes from the ROM even when the destination is itself
         * inside the bottom 16K, which a few snapshots' program counters
         * are.
         */
        if (!tramp_seen_jp) {
            if (addr == tramp_jp) tramp_seen_jp = true;
            return;
        }
        apply_paging();             // the real ROM, per the latch
        tramp_active = false;
        update_opfetch_hook();
        return;
    }

    if (!settings.betadisk) return;

    /*
     * The Beta Disk's paging rule, which is the whole of its address
     * decoding: the TR-DOS ROM appears when an opcode is fetched from
     * 0x3Dxx with the 48K BASIC ROM selected, and goes away on the first
     * opcode fetch at 0x4000 or above.
     *
     * That is why entering TR-DOS is spelled RANDOMIZE USR 15616. 15616
     * is 0x3D00, and calling it is the entire mechanism -- there is no
     * port and no latch, just a comparator on the address bus.
     */
    if (addr >= 0x4000) {
        if (unlikely(beta_rom_paged())) {
            beta_set_rom_paged(false);
            apply_paging();
        }
        return;
    }

    if (beta_rom_paged()) return;

    // On a 128K the rule only applies while ROM 1 is selected; the
    // editor ROM has its own use for that address.
    const bool basic48 = spec_machine == MACHINE_48K || (spec_port_7ffd & 0x10);
    if ((addr & 0xFF00) == 0x3D00 && basic48) {
        beta_set_rom_paged(true);
        apply_paging();
    }
}

static void update_opfetch_hook(void) {
    /*
     * The hook costs a load and a predicted branch on every opcode
     * fetch, so it is installed only when something needs it: a
     * snapshot trampoline waiting to unmap itself, the Beta Disk, or
     * DivMMC -- the last two because both decide where they are paged
     * by watching where the CPU is fetching from.
     */
    z80_opfetch_cb = (tramp_active || settings.betadisk || settings.esxdos)
                         ? spec_opfetch : NULL;
}

void spec_begin_launch(uint8_t *page, const uint16_t jp_addr) {
    tramp_jp       = jp_addr;
    tramp_seen_jp  = false;
    tramp_active   = true;
    z80_map_page(0, page, NULL);    // read-only, like the ROM it replaces
    update_opfetch_hook();
}

bool spec_launch_pending(void) { return tramp_active; }

// ============================================================================
// Ports
// ============================================================================
/*
 * Two ports, and the decode is the interesting part of both.
 *
 * Nothing on a Spectrum decodes an address fully. The ULA answers every
 * port with A0 low — 0xFE is merely the one the ROM happens to use — and
 * the 128K's paging latch answers every port with A15 and A1 both low.
 * Programs rely on this: half the loaders in existence write the border
 * through 0xFF or 0x7E rather than 0xFE.
 *
 * The port argument is the whole 16-bit address, which matters twice
 * over. The keyboard is read by pulling one of A8-A15 low and seeing
 * which of the five bits comes back clear, and the paging latch is
 * selected by A15/A1. Neither is visible in the low byte alone.
 */
static void __not_in_flash_func(spec_io_out)(const uint16_t port, const uint8_t value) {
    /*
     * ULA+ first: both its ports are odd, so an ordinary ULA write would
     * claim them before anything else got a look. Theirs decodes the
     * whole sixteen bits, which is what keeps #FF3B away from the
     * paging port it shares a low byte with.
     */
    /*
     * The SCLD's one port, decoded as pico-speccy's Ports.cpp decodes it.
     *
     * The LOW BYTE alone, because that is what a real Timex answers
     * (MAME's tc2048_io and ts2068_io both map 0xFF mirrored across the
     * high byte, and Fuse's periph mask is 0x00FF). Testing the whole
     * sixteen bits would almost never match: `OUT (#FF),A` puts **A** on
     * the high address byte, so the address is A<<8 | #FF and not #00FF.
     *
     * The A8=0 qualifier stays, though, because here the SCLD is a CARD
     * on an ordinary Sinclair machine rather than a Timex -- and on that
     * machine the SAA1099 shares the #FF family, at #00FF for data and
     * #01FF for the register. Their note says the qualifier is exactly
     * what keeps the two apart.
     *
     * And nothing in the family answers while the TR-DOS ROM is paged.
     */
    if ((settings.timex || spec_machine == MACHINE_TIMEX)
        && (port & 0xFFu) == 0xFFu
        && (spec_machine == MACHINE_TIMEX || !(port & 0x0100u))
        && !beta_rom_paged()) {
        spec_timex_mode = (uint8_t)(value & 0x07u);
        spec_timex_ink  = (uint8_t)((value >> 3) & 0x07u);
        /*
         * Bit 7 picks what the #F4 window shows for the whole map --
         * the dock (0) or the EX-ROM (1). It lives in the video port
         * because the SCLD is one chip doing both jobs.
         */
        const bool ex = (value & 0x80u) != 0;
        if (spec_machine == MACHINE_TIMEX && ex != spec_timex_exrom) {
            spec_timex_exrom = ex;
            apply_paging();
        }
        return;
    }

    if (settings.ulaplus) {
        if (port == 0xBF3Bu) { spec_ulaplus_reg = value; return; }
        if (port == 0xFF3Bu) {
            const uint8_t reg = spec_ulaplus_reg;
            if ((reg & 0xC0u) == 0x00u) {
                spec_ulaplus_pal[reg & 0x3Fu] = value;
                spec_ulaplus_dirty = true;
            } else if ((reg & 0xC0u) == 0x40u) {
                const bool on = (value & 1u) != 0;
                if (on != spec_ulaplus_on) {
                    spec_ulaplus_on = on;
                    /* The attribute's top two bits become the palette
                     * group, so there is no FLASH bit left to honour. */
                    if (on) spec_flash = false;
                    spec_ulaplus_dirty = true;
                }
            }
            return;
        }
    }

    /*
     * The tape trap first, and it only ever answers when a patched ROM
     * is mapped -- nothing else on the machine can reach the port it
     * uses, because the only code that knows about it is code this
     * firmware wrote into the ROM window itself.
     */
    if (tape_port_out(port, value)) return;
    if (beta_port_out(port, value)) return;

    /*
     * The +3's disk controller. Only a +3 has one, and fdc_port_out()
     * says so itself -- the ports it answers, #2FFD and #3FFD, are in
     * the same #FD family the AY and the paging latches live in, and
     * letting them through on a 128K would take writes the AY wanted.
     */
    if (fdc_port_out(port, value)) return;
    if (divmmc_port_out(port, value)) return;

    if ((port & 0x0001) == 0) {                 // the ULA
        spec_border = value & 0x07;

        /*
         * The beeper. Bit 4 of this port is the speaker on a real
         * Spectrum, driven from the ULA's latch.
         *
         * This used to be a gpio_put() straight to GP46 — the CPU's own
         * edges reaching the transducer with nothing in between, which
         * is exactly what 1-bit audio is. The AY ended that: the pin is
         * a PWM DAC now and cannot also be a logic output, so the beeper
         * becomes a level that the mixer samples 44100 times a second
         * along with everything else.
         *
         * The loss is real but small. Core 1 runs the frame in
         * sample-sized slices and reads this between them, so an edge
         * still lands in the sample it happened in; what is gone is
         * sub-sample detail, which for a beeper mostly means the very
         * top of its range.
         */
        spec_beeper = (value & 0x10) != 0;
        last_fe     = value;
    }

    /*
     * Covox on #FB and SounDrive on #0F/#1F/#4F/#5F.
     *
     * Decoded on the low byte alone, which is how the cards themselves
     * did it. #1F is also where a Kempston joystick answers -- but that
     * is a read and this is a write, so the two never meet.
     */
    if (settings.covox && (port & 0xFF) == 0xFB) covox_level = value;
    if (settings.soundrive) {
        switch (port & 0xFF) {
            case 0x0F: sd_level[0] = value; break;
            case 0x1F: sd_level[1] = value; break;
            case 0x4F: sd_level[2] = value; break;
            case 0x5F: sd_level[3] = value; break;
            default: break;
        }
    }

    /*
     * The Profi's #DFFD, decoded on all sixteen bits as theirs is.
     *
     * Above the AY, and that order is the whole point: #DFFD is A15
     * high with A14 high and A1 low, which is also the AY's register
     * select -- so with the AY first a Profi's third latch is never
     * reachable, and a Profi has an AY always. Their Ports::output
     * takes #DFFD before its AY arms for the same reason.
     *
     * Not gated by the paging lock, and D4 clears the lock outright --
     * which is how a Profi gets back out of a locked 128K map.
     */
    if (spec_machine == MACHINE_PROFI && port == 0xDFFDu) {
        spec_port_dffd = value;
        if (value & 0x10u) paging_locked = false;
        apply_paging();
        return;
    }

    /*
     * The AY, on two ports that are both "A15 high".
     *
     * Register select is A15 and A14 both high (0xFFFD); data is A15 high
     * with A14 low (0xBFFD). As everywhere else on this machine the
     * decode is partial, and as everywhere else programs rely on it.
     */
    if (have_ay() && (port & 0xC002) == 0xC000) {   // select
        /*
         * TurboSound steals the top of the register range.
         *
         * 0xFF and 0xFE are not register numbers there, they pick which
         * of the two chips the next data writes reach. A single-chip
         * machine must treat them as ordinary selects of a register that
         * does not exist, which is why this only acts when the second
         * chip has been turned on.
         */
        if (settings.turbosound && (value & 0xFE) == 0xFE) {
            ay_sel = (uint8_t)(value & 1 ? 0 : 1);   // 0xFF = first, 0xFE = second
        } else {
            ay_select(&ay[ay_sel], value);
        }
        return;
    }
    if (have_ay() && (port & 0xC002) == 0x8000) {   // data
        ay_write(&ay[ay_sel], value);
        spec_ay_writes++;
        return;
    }

    /*
     * The Scorpion's #1FFD: A15=A14=0, A1=0, and A5=1.
     *
     * Their decode, and the A5 is the part worth keeping: without it an
     * OUT (n),A with n below 0x20 lands here and repages the machine.
     * Never gated by the paging lock either -- #7FFD D5 freezes only
     * #7FFD on real hardware, and #1FFD stays live until reset.
     */
    if (spec_machine == MACHINE_SCORPION && (port & 0xC002u) == 0
        && (port & 0x0020u)) {
        spec_port_1ffd = value;
        apply_paging();
        return;
    }

    /*
     * The Timex's horizontal select register, #F4: one bit per 8 KB
     * chunk. Decoded on the low byte, like everything else in that
     * machine's port map.
     */
    if (spec_machine == MACHINE_TIMEX && (port & 0xFFu) == 0xF4u) {
        spec_timex_hsr = value;
        apply_paging();
        return;
    }

    /*
     * The +3's second latch, #1FFD: A15=0, A14=0, A13=0, A12=1, A1=0.
     *
     * Decoded as (port & 0xF002) == 0x1000, which is the mask the +3's
     * own hardware uses -- the same shape as #7FFD's (port & 0x8002) == 0
     * one bit narrower. The paging lock covers it too: once #7FFD bit 5
     * is set the machine cannot page again by any route.
     */
    if (spec_machine == MACHINE_PLUS3 && (port & 0xF002u) == 0x1000u
        && !paging_locked) {
        spec_port_1ffd = value;
        apply_paging();
        return;
    }

    /*
     * ...and #7FFD last, because its decode is the loosest of them.
     *
     * (port & 0x8002) == 0 is A15=0 and A1=0, which is also true of
     * #1FFD -- so a Scorpion or a +3 writing its second latch would be
     * taken as writing its first if this arm came first. Their Ports.cpp
     * solves it the same way, by order: the tight decodes above get
     * their ports, and whatever is left over and looks like #7FFD is
     * #7FFD.
     */
    if (spec_machine != MACHINE_48K && (port & 0x8002) == 0 && !paging_locked) {
        spec_port_7ffd = value;
        if (value & 0x20) paging_locked = true;
        apply_paging();
        return;
    }
}

// ---------------------------------------------------------------------------
// Joysticks
// ---------------------------------------------------------------------------
/*
 * One set of five bits, read five different ways.
 *
 * Nothing about a Spectrum joystick is standard, because the machine has
 * no joystick port: every interface is somebody's add-on and they agree
 * about nothing. Kempston put it on a port of its own, active high.
 * Sinclair wired two of them straight onto keyboard rows -- so a
 * Sinclair joystick IS keys 6-0 and 1-5, and a game reading it is
 * reading the keyboard. The Cursor interface picked a third set of keys.
 * Fuller used yet another port, with the bits in a different order and
 * inverted.
 *
 * So the state is held once, in joy_state, and each interface is a way
 * of presenting it. The Sinclairs and the Cursor are folded into the
 * matrix at read time rather than pressed into it, which keeps a real
 * key and a joystick direction from cancelling each other out when they
 * happen to be the same key.
 */
static volatile uint8_t joy_state;      // bits per JOY_BIT_*

void spec_joy_set(const uint8_t bit, const bool pressed) {
    if (bit > JOY_BIT_FIRE) return;
    if (pressed) joy_state |=  (uint8_t)(1u << bit);
    else         joy_state &= (uint8_t)~(1u << bit);
}

uint8_t spec_joy_state(void) { return joy_state; }

#define JOY(b) (joy_state & (1u << (b)))

/* Which bits of half-row `row` a keyboard-wired joystick is holding down. */
static uint8_t joy_row_mask(const unsigned row) {
    uint8_t m = 0;
    switch (settings.joystick) {
        case JOY_SINCLAIR1:             // keys 6 7 8 9 0, on half-row 4
            if (row != R_09876) break;
            if (JOY(JOY_BIT_LEFT))  m |= 1u << 4;   // 6
            if (JOY(JOY_BIT_RIGHT)) m |= 1u << 3;   // 7
            if (JOY(JOY_BIT_DOWN))  m |= 1u << 2;   // 8
            if (JOY(JOY_BIT_UP))    m |= 1u << 1;   // 9
            if (JOY(JOY_BIT_FIRE))  m |= 1u << 0;   // 0
            break;

        case JOY_SINCLAIR2:             // keys 1 2 3 4 5, on half-row 3
            if (row != R_12345) break;
            if (JOY(JOY_BIT_LEFT))  m |= 1u << 0;   // 1
            if (JOY(JOY_BIT_RIGHT)) m |= 1u << 1;   // 2
            if (JOY(JOY_BIT_DOWN))  m |= 1u << 2;   // 3
            if (JOY(JOY_BIT_UP))    m |= 1u << 3;   // 4
            if (JOY(JOY_BIT_FIRE))  m |= 1u << 4;   // 5
            break;

        case JOY_CURSOR:                // 5 6 7 8 and 0 -- the ROM's own arrows
            if (row == R_12345 && JOY(JOY_BIT_LEFT))  m |= 1u << 4;   // 5
            if (row == R_09876) {
                if (JOY(JOY_BIT_DOWN))  m |= 1u << 4;   // 6
                if (JOY(JOY_BIT_UP))    m |= 1u << 3;   // 7
                if (JOY(JOY_BIT_RIGHT)) m |= 1u << 2;   // 8
                if (JOY(JOY_BIT_FIRE))  m |= 1u << 0;   // 0
            }
            break;

        default: break;                 // Kempston and Fuller have their own ports
    }
    return m;
}

/*
 * Kempston: its own port, active high, and only five bits of it.
 *
 * The decode is A5 low and nothing else, which is why #1F is the usual
 * name for it and why it collides with a good deal else. Some interfaces
 * sat on #DF instead; a few games only find one or the other.
 */
static uint8_t kempston_byte(void) {
    uint8_t v = 0;
    if (JOY(JOY_BIT_RIGHT)) v |= 0x01;
    if (JOY(JOY_BIT_LEFT))  v |= 0x02;
    if (JOY(JOY_BIT_DOWN))  v |= 0x04;
    if (JOY(JOY_BIT_UP))    v |= 0x08;
    if (JOY(JOY_BIT_FIRE))  v |= 0x10;
    return v;
}

/* Fuller: port #7F, inverted, and in a different order again. */
static uint8_t fuller_byte(void) {
    uint8_t v = 0;
    if (JOY(JOY_BIT_UP))    v |= 0x01;
    if (JOY(JOY_BIT_DOWN))  v |= 0x02;
    if (JOY(JOY_BIT_LEFT))  v |= 0x04;
    if (JOY(JOY_BIT_RIGHT)) v |= 0x08;
    if (JOY(JOY_BIT_FIRE))  v |= 0x80;
    return (uint8_t)~v;
}

// ---------------------------------------------------------------------------

static uint8_t __not_in_flash_func(spec_io_in)(const uint16_t port) {
    if (settings.ulaplus && port == 0xFF3Bu) {
        const uint8_t reg = spec_ulaplus_reg;
        if ((reg & 0xC0u) == 0x00u) return spec_ulaplus_pal[reg & 0x3Fu];
        if ((reg & 0xC0u) == 0x40u) return spec_ulaplus_on ? 1u : 0u;
        return 0xFFu;
    }

    uint8_t v;
    if (tape_port_in(port, &v)) return v;
    if (beta_port_in(port, &v)) return v;
    if (fdc_port_in(port, &v)) return v;
    if (divmmc_port_in(port, &v)) return v;

    if (settings.joystick == JOY_KEMPSTON &&
        (port & 0xFF) == (settings.kempston_port ? 0xDF : 0x1F))
        return kempston_byte();

    if (settings.joystick == JOY_FULLER && (port & 0xFF) == 0x7F)
        return fuller_byte();

    if ((port & 0x0001) == 0) {                 // the ULA: keyboard
        uint8_t result = 0x1F;                  // five keys, all up

        // A half-row is selected by its address line being LOW, and more
        // than one can be low at once — which is not a corner case, it is
        // how the ROM scans the whole keyboard with a single IN from
        // 0x00FE. So this ANDs every selected row rather than picking one.
        for (unsigned row = 0; row < 8; row++)
            if (!(port & (0x0100u << row))) result &= (uint8_t)(kbd[row] & ~joy_row_mask(row));

        /*
         * Bit 6 is the EAR input, and what it reads with no tape
         * attached is the difference between an issue 2 board and an
         * issue 3 one.
         *
         * Both feed the last value written to port 0xFE back into it. An
         * issue 3 looks at bit 4 alone -- the speaker; an issue 2 looks
         * at bits 3 and 4 together, so setting the MIC bit also lifts
         * EAR. A handful of 48K games test this to tell the two boards
         * apart, and one written for an issue 2 reads the key rows as
         * permanently pressed on an issue 3.
         */
        const uint8_t ear = settings.issue2 ? ((last_fe & 0x18) ? 0x40 : 0)
                                            : ((last_fe & 0x10) ? 0x40 : 0);
        return (uint8_t)(result | 0xA0 | ear);
    }

    // The AY's read-back port is the select port, not the data one.
    if (have_ay() && (port & 0xC002) == 0xC000)
        return ay_read(&ay[ay_sel]);

    /*
     * Everything else is the floating bus, and 0xFF is the honest answer
     * for it here. On real hardware an unclaimed IN returns whatever the
     * ULA happened to be fetching, which a handful of effects depend on;
     * reproducing that needs the video and the CPU to share a clock, and
     * they do not on this board.
     */
    return 0xFF;
}

/*
 * A known tone, for measuring the sound path rather than listening to it.
 *
 * Generated here and not by the AY, which matters: the 128K ROM's
 * interrupt routine writes the chip's registers fifty times a second
 * whether anything is playing or not, so a tone programmed into the AY
 * from outside survives about twenty milliseconds. What came out was a
 * dense inharmonic mess that looked like terrible distortion and was
 * nothing of the kind.
 *
 * 441 Hz because 44100 divides by it exactly: a hundred samples to the
 * cycle, so the square wave is the same every cycle and anything in the
 * spectrum that is not 441 Hz and its odd harmonics was put there by
 * this firmware or by the board.
 */
#define TEST_TONE_HZ      441u
#define TEST_TONE_SAMPLES (AUDIO_RATE_HZ / TEST_TONE_HZ)   /* exactly 100 */

/*
 * A sine and not a square, because this is a measuring instrument.
 *
 * A square wave's harmonics run past Nyquist and fold back, so a
 * spectrum of one is dense whatever the DAC does and there is no way to
 * tell the folding from the distortion. A sine puts everything in one
 * bin and makes every other bin evidence.
 *
 * Amplitude 8000 of 32767, which is -12 dBFS: loud enough to measure,
 * quiet enough that nothing downstream clips.
 */
static const int16_t test_tone_wave[TEST_TONE_SAMPLES] = {
         0,    502,   1003,   1499,   1990,   2472,   2945,   3406,   3854,   4287,
      4702,   5099,   5476,   5832,   6164,   6472,   6755,   7010,   7239,   7438,
      7608,   7749,   7858,   7937,   7984,   8000,   7984,   7937,   7858,   7749,
      7608,   7438,   7239,   7010,   6755,   6472,   6164,   5832,   5476,   5099,
      4702,   4287,   3854,   3406,   2945,   2472,   1990,   1499,   1003,    502,
         0,   -502,  -1003,  -1499,  -1990,  -2472,  -2945,  -3406,  -3854,  -4287,
     -4702,  -5099,  -5476,  -5832,  -6164,  -6472,  -6755,  -7010,  -7239,  -7438,
     -7608,  -7749,  -7858,  -7937,  -7984,  -8000,  -7984,  -7937,  -7858,  -7749,
     -7608,  -7438,  -7239,  -7010,  -6755,  -6472,  -6164,  -5832,  -5476,  -5099,
     -4702,  -4287,  -3854,  -3406,  -2945,  -2472,  -1990,  -1499,  -1003,   -502,
};

static volatile bool test_tone_on;
static uint32_t      test_tone_phase;

void spec_test_tone(const bool on) {
    test_tone_on    = on;
    test_tone_phase = 0;
}

bool spec_test_tone_is_on(void) { return test_tone_on; }

// ============================================================================
// Keyboard
// ============================================================================

void spec_key_set(const uint8_t row, const uint8_t bit, const bool pressed) {
    if (row > 7 || bit > 4) return;
    if (pressed) kbd[row] &= ~(1u << bit);
    else         kbd[row] |=  (1u << bit);
}

typedef struct { uint8_t row, bit; } key_t;

static const key_t key_letters[26] = {
    /* A */ {R_ASDFG, 0}, /* B */ {R_SPACE, 4}, /* C */ {R_CSZXCV, 3},
    /* D */ {R_ASDFG, 2}, /* E */ {R_QWERT, 2}, /* F */ {R_ASDFG, 3},
    /* G */ {R_ASDFG, 4}, /* H */ {R_ENTER, 4}, /* I */ {R_POIUY, 2},
    /* J */ {R_ENTER, 3}, /* K */ {R_ENTER, 2}, /* L */ {R_ENTER, 1},
    /* M */ {R_SPACE, 2}, /* N */ {R_SPACE, 3}, /* O */ {R_POIUY, 1},
    /* P */ {R_POIUY, 0}, /* Q */ {R_QWERT, 0}, /* R */ {R_QWERT, 3},
    /* S */ {R_ASDFG, 1}, /* T */ {R_QWERT, 4}, /* U */ {R_POIUY, 3},
    /* V */ {R_CSZXCV, 4}, /* W */ {R_QWERT, 1}, /* X */ {R_CSZXCV, 2},
    /* Y */ {R_POIUY, 4}, /* Z */ {R_CSZXCV, 1},
};

static const key_t key_digits[10] = {
    /* 0 */ {R_09876, 0}, /* 1 */ {R_12345, 0}, /* 2 */ {R_12345, 1},
    /* 3 */ {R_12345, 2}, /* 4 */ {R_12345, 3}, /* 5 */ {R_12345, 4},
    /* 6 */ {R_09876, 4}, /* 7 */ {R_09876, 3}, /* 8 */ {R_09876, 2},
    /* 9 */ {R_09876, 1},
};

// How long a console keystroke is held. The ROM scans the keyboard once
// per frame and debounces across frames, so a press that lasts one frame
// is a press it can miss entirely.
#define KEY_HOLD_FRAMES 3

static struct { uint8_t row, bit; } held[4];
static unsigned held_count;
static unsigned held_frames;

// Joystick directions held by a console keystroke, released on the same
// timer as the matrix keys above.
static uint8_t held_joy;

#define SYM() hold(R_SPACE, 1)      // SYMBOL SHIFT, which most symbols need

static void hold(const uint8_t row, const uint8_t bit) {
    if (held_count >= count_of(held)) return;
    held[held_count].row = row;
    held[held_count].bit = bit;
    held_count++;
    spec_key_set(row, bit, true);
}

/*
 * Cursor keys arrive as three bytes, so this needs one byte of memory.
 *
 * A terminal sends ESC [ A for up. The Spectrum has no cursor keys at
 * all — they are CAPS SHIFT plus 5, 6, 7 and 8, which is what the 128K
 * menu reads — so the sequence is reassembled here and pressed as the
 * shifted digit. Two bytes of state rather than a parser, because there
 * is exactly one sequence shape worth recognising.
 */
static uint8_t esc_state;   // 0 = idle, 1 = seen ESC, 2 = seen ESC [

void spec_key_ascii(char c) {
    if (esc_state == 1) { esc_state = (c == '[') ? 2 : 0; return; }
    if (esc_state == 2) {
        esc_state = 0;
        if (held_count || held_joy) return;

        /*
         * An arrow is the joystick when there is one to be.
         *
         * Otherwise it is CAPS SHIFT plus a digit, which is what the
         * Spectrum's own cursor keys are and what the 128K menu reads.
         */
        if (settings.joystick != JOY_NONE && settings.cursor_as_joy) {
            held_frames = KEY_HOLD_FRAMES;
            switch (c) {
                case 'A': held_joy = 1u << JOY_BIT_UP;    break;
                case 'B': held_joy = 1u << JOY_BIT_DOWN;  break;
                case 'C': held_joy = 1u << JOY_BIT_RIGHT; break;
                case 'D': held_joy = 1u << JOY_BIT_LEFT;  break;
                default:  held_frames = 0; return;
            }
            joy_state |= held_joy;
            return;
        }

        held_frames = KEY_HOLD_FRAMES;
        hold(R_CSZXCV, 0);                      // CAPS SHIFT
        switch (c) {
            case 'A': hold(R_09876, 3); break;  // up    = CAPS + 7
            case 'B': hold(R_09876, 4); break;  // down  = CAPS + 6
            case 'C': hold(R_09876, 2); break;  // right = CAPS + 8
            case 'D': hold(R_12345, 4); break;  // left  = CAPS + 5
            default:  held_count = 0; held_frames = 0; break;
        }
        return;
    }
    if (c == 0x1B) { esc_state = 1; return; }

    if (held_count || held_joy) return;         // one keystroke at a time

    held_frames = KEY_HOLD_FRAMES;

    if (c >= 'a' && c <= 'z') { hold(key_letters[c - 'a'].row, key_letters[c - 'a'].bit); return; }
    if (c >= 'A' && c <= 'Z') {
        hold(R_CSZXCV, 0);                      // CAPS SHIFT
        hold(key_letters[c - 'A'].row, key_letters[c - 'A'].bit);
        return;
    }
    if (c >= '0' && c <= '9') { hold(key_digits[c - '0'].row, key_digits[c - '0'].bit); return; }

    /*
     * Everything else is a chord.
     *
     * A Spectrum keyboard has forty keys and no punctuation at all: every
     * symbol is SYMBOL SHIFT with a letter or a digit, and the pairings
     * are printed on the keys rather than following any pattern. This is
     * that table. Without it a BASIC line containing a colon or a bracket
     * cannot be typed, which rules out most of the useful ones.
     */
    switch (c) {
        case '\r': case '\n': hold(R_ENTER,  0); break;
        case ' ':              hold(R_SPACE,  0); break;
        case 0x7F: case '\b':  hold(R_CSZXCV, 0); hold(R_09876, 0); break;  // DELETE

        case '!': SYM(); hold(R_12345, 0); break;
        case '@': SYM(); hold(R_12345, 1); break;
        case '#': SYM(); hold(R_12345, 2); break;
        case '$': SYM(); hold(R_12345, 3); break;
        case '%': SYM(); hold(R_12345, 4); break;
        case '&': SYM(); hold(R_09876, 4); break;
        case '\'':SYM(); hold(R_09876, 3); break;
        case '(': SYM(); hold(R_09876, 2); break;
        case ')': SYM(); hold(R_09876, 1); break;
        case '_': SYM(); hold(R_09876, 0); break;

        case '<': SYM(); hold(R_QWERT, 3); break;   // R
        case '>': SYM(); hold(R_QWERT, 4); break;   // T
        case ';': SYM(); hold(R_POIUY, 1); break;   // O
        case '"': SYM(); hold(R_POIUY, 0); break;   // P
        case '-': SYM(); hold(R_ENTER, 3); break;   // J
        case '+': SYM(); hold(R_ENTER, 2); break;   // K
        case '=': SYM(); hold(R_ENTER, 1); break;   // L
        case '^': SYM(); hold(R_ENTER, 4); break;   // H
        case ':': SYM(); hold(R_CSZXCV, 1); break;  // Z
        case '?': SYM(); hold(R_CSZXCV, 3); break;  // C
        case '/': SYM(); hold(R_CSZXCV, 4); break;  // V
        case '*': SYM(); hold(R_SPACE, 4); break;   // B
        case ',': SYM(); hold(R_SPACE, 3); break;   // N
        case '.': SYM(); hold(R_SPACE, 2); break;   // M

        default: held_frames = 0; break;
    }
}

// ---------------------------------------------------------------------------
// Typing a whole line
// ---------------------------------------------------------------------------
/*
 * One character per few frames, with a gap between them.
 *
 * The gap is the point. spec_key_ascii() presses a key and holds it for
 * three frames, and the ROM's debounce will not register a second press
 * until it has seen the key released -- so a string fed straight in
 * arrives as one character. Feeding it on a timer, with the release
 * visible in between, is what makes LOAD "" typeable from here.
 *
 * The initial delay exists because the thing that wants this is a tape
 * being mounted, which cold-starts the machine first: the ROM needs a
 * second to clear memory and put up a menu before it is listening.
 */
static const char *type_str;
static unsigned    type_wait;

#define TYPE_GAP_FRAMES   4

void spec_type(const char *s, const unsigned delay_frames) {
    type_str  = s;
    type_wait = delay_frames;
}

void spec_type_cancel(void) { type_str = NULL; }

static void type_tick(void) {
    if (!type_str) return;
    if (type_wait) { type_wait--; return; }
    if (held_count || held_joy) return; // still finishing the last one

    if (!*type_str) { type_str = NULL; return; }
    spec_key_ascii(*type_str++);
    type_wait = TYPE_GAP_FRAMES;
}

void spec_key_tick(void) {
    if (!held_count && !held_joy) { type_tick(); return; }
    if (held_frames) { held_frames--; return; }

    for (unsigned i = 0; i < held_count; i++)
        spec_key_set(held[i].row, held[i].bit, false);
    held_count = 0;

    joy_state &= (uint8_t)~held_joy;
    held_joy = 0;
}

// ============================================================================
// Bring-up and the frame loop
// ============================================================================

void spec_init(void) {
    /*
     * Fill the ROM window before anything can page out of it.
     *
     * Here rather than in spec_set_machine(), because main.c calls
     * spec_init() directly at boot and an unfilled window is sixteen
     * kilobytes of zeroes for the CPU to execute -- which looks exactly
     * like a dead Z80.
     *
     * Eighty kilobytes of memcpy out of flash, a millisecond or two, on
     * a path that is already resetting the machine.
     */
    rom_load_window(spec_machine);
    /*
     * ...and the tape trap goes back over LD-BYTES, because the refill
     * above just replaced the page it was patched into.
     */
    tape_repatch_rom();

    memset(spec_ram, 0, sizeof spec_ram);
    memset((void *)kbd, 0x1F, sizeof kbd);      // every key up
    held_count = 0;

    ay_reset(&ay[0]);
    ay_reset(&ay[1]);
    ay_sel         = 0;
    spec_beeper    = false;
    last_fe        = 0;
    covox_level    = 0x80;
    memset((void *)sd_level, 0x80, sizeof sd_level);
    joy_state      = 0;
    spec_ay_writes = 0;

    esc_state      = 0;
    held_joy       = 0;
    type_str       = NULL;
    paging_locked  = false;

    spec_port_7ffd = 0;
    spec_port_1ffd = 0;
    spec_port_dffd = 0;
    spec_timex_hsr = 0;
    spec_timex_exrom = false;
    spec_bank_flush();
    spec_border    = 7;
    spec_frames    = 0;

    /*
     * A Beta Disk 128 comes up with its own ROM at 0x0000 and lets
     * TR-DOS decide what happens next, which is how a disk with a `boot`
     * file on it starts by itself. Only after a .trd has been mounted,
     * though: on an empty drive that start-up is a wait for a disk that
     * is not there.
     *
     * On a 128K, a +2 or a Pentagon there is a second thing to do.
     * TR-DOS is a 48K program: it expects the 48 BASIC ROM underneath it
     * and its own paging rule only fires while that ROM is selected, so
     * a machine that comes up in the editor ROM pages TR-DOS out on its
     * first fetch above 0x4000 and can never page it back in. Selecting
     * ROM 1 first -- without the lock bit, so a 128K program on the disk
     * still has its banks -- is what a Beta Disk 128 is.
     */
    beta_reset();
    fdc_reset();
    divmmc_reset();
    const bool beta_boot = settings.betadisk && beta_take_boot_request();
    if (beta_boot && spec_machine != MACHINE_48K) spec_port_7ffd = 0x10;
    beta_set_rom_paged(beta_boot);

    apply_paging();

    z80_io_in  = spec_io_in;
    z80_io_out = spec_io_out;
    update_opfetch_hook();

    // The stage-0 wiring watch, if a quick check left it on. A machine
    // never wants it and it is expensive: see z80_bus.h.
    z80_edge_watch_stop();

    z80_reset();
}

/*
 * One audio sample's worth of Z80, then one audio sample.
 *
 * A frame is 70908 T-states and 882 samples, which is 80.394 T-states
 * each — so the slice length alternates between 80 and 81 under a
 * fractional accumulator rather than being rounded, because rounding
 * 0.394 away 882 times a frame is 347 T-states of drift every 20 ms.
 */
static uint32_t slice_acc;

static void run_frame(void);

/*
 * How many samples a frame is cut into.
 *
 * It was 882, on the reasoning that a 70908-T-state frame and 882
 * samples at 44100 Hz are both exactly 20 ms. They are -- on a machine
 * that runs at exactly 3.5469 MHz. This one runs at whatever the clock
 * tuner could reach, which is about one and a half per cent short, so a
 * frame really takes 20.3 ms and 882 samples is not enough to fill it.
 *
 * What that sounds like: the DAC drains the ring 635 times a second
 * faster than the machine fills it, and audio_task() holds the last
 * sample each time rather than going silent. So one sample in seventy is
 * a repeat -- which stretches the waveform, drops the pitch by the same
 * one and a half per cent, and adds a roughness that is hard to name and
 * easy to hear.
 *
 * So the count follows the measured frame rate instead: enough samples
 * to fill however long a frame actually takes. See spec_tune_clock().
 */
static uint32_t samples_per_frame = AUDIO_SAMPLES_PER_FRAME;

uint32_t spec_samples_per_frame(void) { return samples_per_frame; }

/*
 * AY cycles per sample, in 16.16 fixed point.
 *
 * 1773400 / 44100 is 40.213, and the fraction is the point. The first
 * version took the slice's T-states and halved them -- which truncates,
 * because a slice is 80 or 81 T-states and 81/2 is 40 -- so the chip lost
 * half a cycle on two slices in five and played half a per cent flat,
 * for ever, in a way no amount of listening would explain.
 *
 * It is also deliberately not derived from T-states at all any more. A
 * sample is a fixed slice of *time*, and what the chip does in that time
 * does not depend on how fast the CPU beside it is being clocked -- so
 * this survives the turbo settings, and a machine running slightly slow
 * still plays in tune.
 */
#define AY_TICKS_Q16 ((uint32_t)(((uint64_t)AY_CLOCK_HZ << 16) / AUDIO_RATE_HZ))

static uint32_t ay_tick_frac;

__force_inline static uint32_t ay_ticks_for_one_sample(void) {
    ay_tick_frac += AY_TICKS_Q16;
    const uint32_t t = ay_tick_frac >> 16;
    ay_tick_frac &= 0xFFFFu;
    return t;
}

__force_inline static void run_slice(const uint32_t tstates, const uint32_t num) {
    (void)num;
    z80_run(tstates);

    /*
     * The mix, once per sample.
     *
     * The beeper is read here rather than pushed by the port handler, so
     * its level is whatever it was at the end of the slice — which is
     * the sample it belongs in. The AYs are advanced by exactly the
     * cycles this slice covered, so nothing drifts against anything.
     */
    int32_t s = 0;

    if (likely(have_ay())) {
        const uint32_t ticks = ay_ticks_for_one_sample();
        int32_t a = ay_render(&ay[0], ticks);
        if (settings.turbosound) a += ay_render(&ay[1], ticks);
        s += a * settings.ay_vol / 10;
    }

    /*
     * The beeper at about a quarter of full scale.
     *
     * On a real Spectrum the beeper is considerably louder than the AY,
     * and reproducing that faithfully makes every 128K game's music sit
     * under its own sound effects. This is the compromise every emulator
     * makes; it is a mix decision, not a measurement.
     */
    s += (spec_beeper ? 6000 : -6000) * settings.beeper_vol / 10;

    // The extra DACs, if the machine has been told it has them. Each is a
    // byte centred on 0x80, at a quarter scale so four of them together
    // still leave room for everything else.
    if (unlikely(settings.covox)) s += ((int32_t)covox_level - 128) * 64;
    if (unlikely(settings.soundrive))
        for (unsigned i = 0; i < 4; i++) s += ((int32_t)sd_level[i] - 128) * 32;

    /*
     * The test tone replaces the mix rather than joining it, so what is
     * measured is the path and not the path plus whatever the machine
     * happened to be doing.
     */
    if (unlikely(test_tone_on)) {
        if (++test_tone_phase >= TEST_TONE_SAMPLES) test_tone_phase = 0;
        s = test_tone_wave[test_tone_phase];
    }

    /*
     * Master volume and the output's own headroom, in one step.
     *
     * The second divisor is what the board does not have: there is no
     * attenuator between GP46 and the MS9288A, so a full-scale mix
     * arrives at its line input about three times too big and everything
     * sounds distorted. The jack and the buzzer want it full scale.
     * Which of the two it is, is a switch nothing can read -- see
     * output_t in settings.h.
     */
    static const uint8_t out_scale[OUTPUT_COUNT] = { 10, 3 };   // over ten
    const uint32_t scale = settings.volume *
                           out_scale[settings.output < OUTPUT_COUNT ? settings.output : 0];
    audio_push(s * (int32_t)scale / 100);
}

static uint32_t tune_once(void) {
    // Muted, because the machine is being run at deliberately wrong
    // speeds and nobody needs to hear that.
    audio_set_enabled(false);

    /*
     * Let the machine get on with something typical first.
     *
     * How long a frame takes depends on what the CPU is doing in it: an
     * I/O cycle costs more engine time than a memory one, and the ROM's
     * power-on RAM test is almost nothing but memory. Tuning against
     * that -- which is exactly what happens when this is called straight
     * after a reset -- picks a pace that is six per cent too fast for
     * the machine the user then sits in front of, and six per cent is a
     * semitone on everything it plays.
     *
     * A second and a half is enough for a 128K to finish testing its
     * memory and reach its menu.
     */
    for (int f = 0; f < 75; f++) run_frame();

    // What five frames should take, in microseconds.
    const int64_t want = 5ll * 1000000ll * SPEC_TSTATES_PER_FRAME / SPEC_CPU_HZ;
    const int64_t tol  = want / 400;        // a quarter of a per cent

    int32_t pace = (int32_t)z80_get_pace();
    int64_t us   = want;

    /*
     * Proportional, then fine.
     *
     * Stepping by one and measuring five frames each time is 100 ms an
     * iteration, and the starting point comes from z80_calibrate(), which
     * timed the bus engine on its own and therefore lands a long way off
     * once two AY chips are in the loop. The first version walked ±1 and
     * ran out of its iteration budget mid-descent, leaving the machine
     * 1.9 % fast with the tuner reporting success.
     *
     * One pace unit is about two per cent of a frame, so the error in per
     * mille divided by twenty is roughly how many units to move. That
     * converges in three or four measurements; the ±1 fallback at the end
     * takes care of the last step, where the estimate rounds to nothing.
     */
    for (int iter = 0; iter < 16; iter++) {
        const uint64_t t0 = time_us_64();
        for (int f = 0; f < 5; f++) run_frame();
        us = (int64_t)(time_us_64() - t0);

        const int64_t err = us - want;                  // >0 means too slow
        if (err > -tol && err < tol) break;

        int32_t step = (int32_t)((err * 1000 / want) / 20);
        if (step == 0) step = err > 0 ? 1 : -1;

        /*
         * Down to the floor and no further.
         *
         * Padding is the only thing this can trade, and below
         * Z80_PACE_MIN the engine samples the CPU's pins before they are
         * valid -- so a frame that is still too long at the floor means
         * the machine cannot run at 50 Hz, not that it should be driven
         * harder. The alternative is a Spectrum that keeps perfect time
         * and computes nonsense, which is what happened.
         */
        pace -= step;                                   // slower -> less padding
        if (pace < (int32_t)Z80_PACE_MIN) pace = (int32_t)Z80_PACE_MIN;
        z80_set_pace((uint32_t)pace);
        if (pace == (int32_t)Z80_PACE_MIN && err > 0) break;
    }

    /*
     * One more measurement, so the number returned is the one that is
     * true now.
     *
     * The loop's last `us` was taken *before* its last adjustment, which
     * made the report disagree with the machine — it announced 51.5 Hz
     * while the board was actually running 49.75. A figure that is wrong
     * in the direction of looking worse is still wrong, and this one is
     * printed on the boot screen where it will be believed.
     */
    const uint64_t t0 = time_us_64();
    for (int f = 0; f < 5; f++) run_frame();
    us = (int64_t)(time_us_64() - t0);

    const uint32_t fps = (uint32_t)((5ll * 1000000000ll) / (us ? us : 1));   // x1000

    /*
     * And cut the frame into however many samples it is long.
     *
     * The DAC runs at 44100 Hz whatever the machine does, so a frame has
     * to hand it 44100/fps samples or the ring runs dry and audio_task()
     * starts repeating the last one. Getting this wrong is not silence,
     * it is a tone a per cent or two flat with a roughness on it.
     *
     * Bounded because a wild measurement here would be a wild number of
     * slices, and the loop that runs them is the machine.
     */
    uint32_t slices = fps ? AUDIO_RATE_HZ * 1000u / fps : AUDIO_SAMPLES_PER_FRAME;
    if (slices < 600u)  slices = 600u;
    if (slices > 1400u) slices = 1400u;
    samples_per_frame = slices;

    audio_set_enabled(true);
    return fps;
}

/*
 * Twice, because the two answers depend on each other.
 *
 * Cutting a frame into more slices makes the frame a little longer --
 * each slice costs a mix and a push -- and a longer frame wants more
 * slices again. One pass leaves the pace tuned for a frame that no
 * longer exists; two lands inside the granularity of the pace itself,
 * and a third would be measuring noise.
 */
uint32_t spec_tune_clock(void) {
    tune_once();
    return tune_once();
}

static volatile bool pause_req, paused;

void spec_pause(void) {
    // Silence rather than a held sample: a machine that has stopped
    // should not leave its last twenty microseconds droning under the
    // menu, and it makes the underrun counter mean something.
    audio_set_enabled(false);
    pause_req = true;
    // A machine that is not running cannot acknowledge, and one that has
    // not started yet never will -- so this waits, but not for ever.
    const absolute_time_t deadline = make_timeout_time_ms(200);
    while (!paused && absolute_time_diff_us(get_absolute_time(), deadline) > 0)
        tight_loop_contents();
}

void spec_resume(void) {
    // The counters are about the running machine; see audio.h.
    audio_reset_stats();
    audio_set_enabled(true);
    pause_req = false;
}
bool spec_is_paused(void) { return paused; }

static void pause_point(void) {
    if (!pause_req) return;
    paused = true;
    while (pause_req) tight_loop_contents();
    paused = false;
}

void spec_set_machine(const machine_t m) {
    if (m >= MACHINE_COUNT) return;
    /*
     * Leaving the Timex ejects whatever is in its dock.
     *
     * The cartridge lives in the RAM banks only that machine leaves
     * idle -- see dck.c -- so on any other machine those banks are the
     * guest's and the cartridge is already gone. Saying so is better
     * than letting it come back as noise.
     */
    if (spec_machine == MACHINE_TIMEX && m != MACHINE_TIMEX) dck_eject();
    /* ...and arriving at one with no cartridge fills the dock with 0xFF. */
    if (m == MACHINE_TIMEX && !dck_mounted()) dck_eject();
    spec_machine = m;
    // A 48K's BASIC ROM and a 128K's ROM 1 are different images, so a
    // patched copy of one is not a patched copy of the other. Anything
    // mounted has to be re-patched before the new machine is started.
    tape_machine_changed();
    spec_init();
}

void spec_next_machine(void) {
    spec_set_machine((machine_t)((spec_machine + 1) % MACHINE_COUNT));
}

/*
 * One frame, without the pause check.
 *
 * Split out because the clock tuner runs frames too, and it runs them
 * from core 0 with the machine already paused -- so a pause point inside
 * them would park the core that is holding the pause, which is a
 * deadlock and was one: changing the Z80 speed from the settings page
 * hung the firmware with the page still on the screen.
 */
/*
 * Keep the ring half full, one sample per frame at a time.
 *
 * How many samples a frame is worth depends on how long the frame takes,
 * and that depends on what the CPU is doing in it: an I/O cycle costs
 * the bus engine more than a memory cycle, so a program hammering the AY
 * runs slower than the ROM sitting at its menu. spec_tune_clock()
 * measures one workload once, and a fixed count from it was out by three
 * per cent on a music player -- twelve hundred samples a second that the
 * DAC asked for and the machine did not have, each one a repeat of the
 * last. That is not a gap; it is a waveform stretched in little steps,
 * and it smears the pitch and sounds like distortion.
 *
 * So it is a feedback loop instead. The ring is the error signal: if it
 * is draining the frame gets cut finer, if it is filling the frame gets
 * cut coarser. One sample of correction per frame is fifty a second,
 * which catches any drift a workload change can produce and is far too
 * slow to hear.
 */
static void track_ring(void) {
    const uint32_t level  = audio_ring_level();
    const uint32_t target = AUDIO_RING_SIZE / 2u;

    if (level < target - target / 4u) {
        if (samples_per_frame < 1400u) samples_per_frame++;
    } else if (level > target + target / 4u) {
        if (samples_per_frame > 600u) samples_per_frame--;
    }
}

static void run_frame(void) {
    track_ring();

    /*
     * The whole main loop of the machine, sliced.
     *
     * An emulator spends a frame stepping a CPU and counting contention.
     * This spends it letting a Z84C00 be a Z80, stopping once per sample
     * to take one. The only thing the ULA owes the CPU is an interrupt
     * every 70908 T-states, and the only thing that has to be right is
     * the count.
     */
    const uint32_t num    = turbo_num();
    const uint32_t total  = spec_frame_tstates();
    const uint32_t slices = samples_per_frame;
    const uint32_t per    = total / slices;
    const uint32_t rem    = total % slices;

    // The ULA holds ~INT low for about ten microseconds, which is 36
    // T-states at 3.5 MHz and proportionally more at any other speed --
    // it is a length of time, not a number of instructions.
    const uint32_t held = SPEC_INT_TSTATES * num / 2;

    uint32_t budget = total - held;

    for (uint32_t i = 0; i < slices; i++) {
        uint32_t slice = per;
        slice_acc += rem;
        if (slice_acc >= slices) {
            slice_acc -= slices;
            slice++;
        }

        if (slice > budget) slice = budget;
        budget -= slice;
        run_slice(slice, num);
        if (!budget) break;
    }

    if (budget) run_slice(budget, num);

    z80_set_int(true);
    run_slice(held, num);
    z80_set_int(false);

    const uint32_t f = spec_frames + 1;
    spec_frames = f;

    // The ULA inverts FLASH cells every 16 frames, so the full cycle is
    // 32 and works out at about two-thirds of a second.
    spec_flash = (f & 0x10) != 0;

    spec_key_tick();
}

void spec_run_frame(void) {
    pause_point();
    run_frame();
}
