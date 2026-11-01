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
#include "launch.h"
#include "saa.h"
#include "sound.h"
#include "tapein.h"
#include "tapeplay.h"
#include "ula_frame.h"
#include "pulse.h"
#include "dino.h"
#include "settings.h"
#include "tape.h"
#include "z80_bus.h"
#include "atm.h"
#include "rtc.h"
#include "smuc.h"
#include "customrom.h"
#include "arena.h"
#include "ide.h"

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
IMPORT_BIN("roms/tc2048.rom", spec_romtc2048);
IMPORT_BIN("roms/alf.rom", spec_romalf);
IMPORT_BIN("roms/trdos-503.rom", spec_romtrd503);
IMPORT_BIN("roms/trdos-504tm.rom", spec_romtrd504tm);
IMPORT_BIN("roms/trdos-505d.rom", spec_romtrd505d);
IMPORT_BIN("roms/trdos-611e.rom", spec_romtrd611e);
IMPORT_BIN("roms/gama89.rom", spec_romgama);
IMPORT_BIN("roms/byte48.rom", spec_rombyte);
IMPORT_BIN("roms/es48.rom", spec_romes48);
IMPORT_BIN("roms/profrom.rom", spec_romprof);
IMPORT_BIN("roms/scorpevo.rom", spec_romsevo);
IMPORT_BIN("roms/kay256.rom", spec_romkay256);
IMPORT_BIN("roms/kay1024.rom", spec_romkay1024);
IMPORT_BIN("roms/kay1024las.rom", spec_romkay1024b);
IMPORT_BIN("roms/phoenix.rom", spec_romphoenix);
IMPORT_BIN("roms/es128-0.rom", spec_romes0);
IMPORT_BIN("roms/es128-1.rom", spec_romes1);
IMPORT_BIN("roms/plus2es-0.rom", spec_romp2es0);
IMPORT_BIN("roms/plus2es-1.rom", spec_romp2es1);
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
extern const uint8_t spec_romtc2048[SPEC_BANK_SIZE];
extern const uint8_t spec_romalf[2 * SPEC_BANK_SIZE];
extern const uint8_t spec_romtrd503[SPEC_BANK_SIZE], spec_romtrd504tm[SPEC_BANK_SIZE];
extern const uint8_t spec_romtrd505d[SPEC_BANK_SIZE], spec_romtrd611e[SPEC_BANK_SIZE];

/*
 * Which TR-DOS the Beta Disk carries: 5.04T is the common one and the
 * default; 5.03, 5.04TM, 5.05D and 6.11e as pico-speccy offers them.
 */
static const uint8_t *trdos_image(void) {
    switch (settings.trdos_rom) {
        case 1:  return spec_romtrd503;
        case 2:  return spec_romtrd504tm;
        case 3:  return spec_romtrd505d;
        case 4:  return spec_romtrd611e;
        default: return spec_romtrd;
    }
}
extern const uint8_t spec_romgama[SPEC_BANK_SIZE];
extern const uint8_t spec_rombyte[SPEC_BANK_SIZE];
extern const uint8_t spec_romes48[SPEC_BANK_SIZE];
extern const uint8_t spec_romprof[16 * SPEC_BANK_SIZE];
extern const uint8_t spec_romsevo[16 * SPEC_BANK_SIZE];
extern const uint8_t spec_romkay256[4 * SPEC_BANK_SIZE];
extern const uint8_t spec_romkay1024[4 * SPEC_BANK_SIZE];
extern const uint8_t spec_romkay1024b[4 * SPEC_BANK_SIZE];
extern const uint8_t spec_romphoenix[4 * SPEC_BANK_SIZE];
extern const uint8_t spec_romes0[SPEC_BANK_SIZE];
extern const uint8_t spec_romes1[SPEC_BANK_SIZE];
extern const uint8_t spec_romp2es0[SPEC_BANK_SIZE];
extern const uint8_t spec_romp2es1[SPEC_BANK_SIZE];

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
    if (spec_is_48k_class(m))   return ROMW_0;
    if (spec_is_plus3_class(m)) return ROMW_3;
    if (m == MACHINE_TIMEX) return ROMW_0;      /* not a Sinclair ROM; no trap lands */
    return ROMW_1;
}

uint8_t *spec_basic_slot(void) { return rom_window[basic_slot(spec_machine)]; }

/*
 * An Interface 2 cartridge, in the PSRAM until it is pulled out.
 *
 * Sinclair's ROM cartridge replaces the 48K's ROM outright -- the
 * interface pulls the ROM's chip-select and puts its own 16 KB there --
 * so a mounted one simply takes the ROM's place when the window is
 * filled, on a 48K and on nothing else. After pico-speccy's If2.cpp.
 */
static uint8_t if2_image[SPEC_BANK_SIZE] __attribute__((section(".psram")));
static bool    if2_in;

void spec_if2_insert(const uint8_t *img, const uint32_t len) {
    memset(if2_image, 0xFF, sizeof if2_image);
    memcpy(if2_image, img, len < sizeof if2_image ? len : sizeof if2_image);
    if2_in = true;
}
void spec_if2_eject(void) { if2_in = false; }
bool spec_if2_present(void) { return if2_in; }

/*
 * The ProfROM's plane and the KAY's extra #7FFD bit.
 *
 * ProfROM is four planes of the usual four 16 KB ROMs; the service code
 * switches plane by jumping through #0100-#010F (see spec_opfetch). The
 * four SRAM slots hold the current plane, refilled from flash on a
 * switch -- a millisecond's stall, at boot and in the service menu.
 */
static uint8_t  prof_plane;
static uint8_t  kay_7ffd_d7;

static const uint8_t *scorpion_image(const machine_t m) {
    switch (m) {
        case MACHINE_ZS1024:   return spec_romprof;
        case MACHINE_SCORPEVO: return spec_romsevo;
        case MACHINE_KAY256:   return spec_romkay256;
        case MACHINE_KAY1024:  return spec_romkay1024;
        case MACHINE_KAY1024B: return spec_romkay1024b;
        case MACHINE_PHOENIX:  return spec_romphoenix;
        default:               return NULL;
    }
}

static void load_plane(const machine_t m, const unsigned plane) {
    const uint8_t *img = scorpion_image(m);
    if (!img) return;
    /*
     * The slots are roles -- 128, 48, service, TR-DOS -- and the KAY and
     * Phoenix images keep their pages in their own order (rom_pack.py's
     * KAY_ROMSETS): the KAY-256/1024 swap the last two, the Phoenix BIOS
     * starts with its empty service page and TR-DOS.
     */
    static const uint8_t ident[4] = {0, 1, 2, 3}, kay[4] = {0, 1, 3, 2}, phx[4] = {2, 3, 0, 1};
    const uint8_t *page = (m == MACHINE_KAY256 || m == MACHINE_KAY1024) ? kay
                        : m == MACHINE_PHOENIX ? phx : ident;
    for (unsigned i = 0; i < 4; i++)
        memcpy(rom_window[i], img + ((plane * 4u + page[i]) * SPEC_BANK_SIZE), SPEC_BANK_SIZE);
}

/*
 * The four ROM slots as a cache, for the ATM: which flash page each one
 * holds, and when it was last asked for.
 */
static const uint8_t *rom_slot_src[4];
static uint32_t       rom_slot_used[4];
static uint32_t       rom_slot_clock;

uint8_t *spec_rom_page(const uint8_t *src) {
    rom_slot_clock++;
    unsigned victim = 0;
    for (unsigned i = 0; i < 4; i++) {
        if (rom_slot_src[i] == src) { rom_slot_used[i] = rom_slot_clock; return rom_window[i]; }
        if (rom_slot_used[i] < rom_slot_used[victim]) victim = i;
    }
    memcpy(rom_window[victim], src, SPEC_BANK_SIZE);
    rom_slot_src[victim]  = src;
    rom_slot_used[victim] = rom_slot_clock;
    return rom_window[victim];
}

/*
 * The ALF TV Game, after pico-speccy's Alf.cpp. OUT with A7=0 and A0=1
 * picks the ROM in the bottom window: bits 0-5 the bank, bit 7 the
 * cartridge rather than the 32 KB system ROM. IN #1D puts a RAM page
 * there instead and IN #1F takes it away. The cartridge sits in the
 * PSRAM and the selected bank is copied into ROM slot 2.
 */
#define ALF_MAX (512u * 1024u)
#define alf_cart (psram_arena + ARENA_ALF_OFF)   // see arena.h
static uint8_t  alf_ram[SPEC_BANK_SIZE] __attribute__((section(".psram")));
static uint32_t alf_banks;
static uint8_t  alf_sel;
static bool     alf_sram;

bool spec_alf_insert(const uint8_t *img, const uint32_t len) {
    const uint32_t n = len < ALF_MAX ? len : ALF_MAX;
    memcpy(alf_cart, img, n);
    if (n % SPEC_BANK_SIZE) memset(alf_cart + n, 0, SPEC_BANK_SIZE - n % SPEC_BANK_SIZE);
    alf_banks = (n + SPEC_BANK_SIZE - 1u) / SPEC_BANK_SIZE;
    return alf_banks != 0;
}

static void alf_select(const uint8_t v) {
    alf_sel = v;
    const unsigned b = (v & 0x7Fu) % 64u;
    if ((v & 0x80u) && b < alf_banks)
        memcpy(rom_window[ROMW_2], alf_cart + b * SPEC_BANK_SIZE, SPEC_BANK_SIZE);
}

/* The flash master a slot is filled from, per machine. */
static void rom_load_flash(const machine_t m);

/*
 * Flash first, then the card's /roms over it where there is a ROM for
 * this machine there (customrom.c) -- read on every reset, so taking the
 * file away puts the stock ROM back. A cartridge in the 48K wins.
 */
static void rom_load_window(const machine_t m) {
    rom_load_flash(m);
    unsigned banks;
    customrom_active = false;
    if (customrom_tag(m, &banks) && !(m == MACHINE_48K && if2_in)) {
        uint8_t *const slots[4] = { rom_window[ROMW_0], rom_window[ROMW_1],
                                    rom_window[ROMW_2], rom_window[ROMW_3] };
        if (customrom_load(m, slots, banks)) {
            customrom_active = true;
            if (banks == 1) memcpy(rom_window[ROMW_1], rom_window[ROMW_0], SPEC_BANK_SIZE);
        }
    }
    customrom_trdos_active = customrom_trdos(rom_window[ROMW_TRDOS]);
}

static void rom_load_flash(const machine_t m) {
    if (m == MACHINE_ALF) {
        memcpy(rom_window[ROMW_0], spec_romalf, SPEC_BANK_SIZE);
        memcpy(rom_window[ROMW_1], spec_romalf + SPEC_BANK_SIZE, SPEC_BANK_SIZE);
        memset(rom_window[ROMW_3], 0x00, SPEC_BANK_SIZE);    // an empty bank reads zeros
        memcpy(rom_window[ROMW_TRDOS], trdos_image(), SPEC_BANK_SIZE);
        alf_sel = 0;
        alf_sram = false;
        return;
    }
    if (spec_is_atm(m)) {
        for (unsigned i = 0; i < 4; i++) { rom_slot_src[i] = NULL; rom_slot_used[i] = 0; }
        memcpy(rom_window[ROMW_TRDOS], trdos_image(), SPEC_BANK_SIZE);
        return;
    }
    if (scorpion_image(m)) {
        prof_plane = 0;
        load_plane(m, 0);
        memcpy(rom_window[ROMW_TRDOS], trdos_image(), SPEC_BANK_SIZE);
        return;
    }
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
        memcpy(rom_window[ROMW_TRDOS], trdos_image(), SPEC_BANK_SIZE);
        return;
    }

    if (m == MACHINE_TIMEX) {
        /* One 16 KB home ROM, and the 8 KB EX-ROM in the slot above it. */
        memcpy(rom_window[ROMW_0], spec_romtx_home, SPEC_BANK_SIZE);
        memcpy(rom_window[ROMW_1], spec_romtx_home, SPEC_BANK_SIZE);
        memcpy(rom_window[ROMW_2], spec_romtx_ex, 8192);
        memcpy(rom_window[ROMW_TRDOS], trdos_image(), SPEC_BANK_SIZE);
        return;
    }

    if (spec_is_48k_class(m)) {
        /*
         * One ROM, whichever this clone has; the 48K's for the ones that
         * kept Sinclair's. ROMW_2 is spare on all of them, so the 16K
         * borrows it as the page of 0xFF its missing RAM reads as.
         */
        const uint8_t *r = (m == MACHINE_TC2048) ? spec_romtc2048
                         : (m == MACHINE_GAMA)   ? spec_romgama
                         : (m == MACHINE_BYTE)   ? spec_rombyte
                         : (m == MACHINE_48K && if2_in) ? if2_image
                         : (m == MACHINE_48K && settings.rom_spanish) ? spec_romes48
                                                 : spec_rom48;
        memcpy(rom_window[ROMW_0], r, SPEC_BANK_SIZE);
        memcpy(rom_window[ROMW_1], r, SPEC_BANK_SIZE);
        if (m == MACHINE_16K) memset(rom_window[ROMW_2], 0xFF, SPEC_BANK_SIZE);
        memcpy(rom_window[ROMW_TRDOS], trdos_image(), SPEC_BANK_SIZE);
        return;
    }

    if (spec_is_plus3_class(m)) {
        /* Four of its own: 0 editor, 1 syntax, 2 +3DOS, 3 48K BASIC. */
        memcpy(rom_window[ROMW_0], spec_romp3_0, SPEC_BANK_SIZE);
        memcpy(rom_window[ROMW_1], spec_romp3_1, SPEC_BANK_SIZE);
        memcpy(rom_window[ROMW_2], spec_romp3_2, SPEC_BANK_SIZE);
        memcpy(rom_window[ROMW_3], spec_romp3_3, SPEC_BANK_SIZE);
        memcpy(rom_window[ROMW_TRDOS], trdos_image(), SPEC_BANK_SIZE);
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
    /*
     * The Spanish 128K and +2 -- Investronica's -- have their own
     * editor and a translated BASIC; flattened from pico-speccy's
     * overlays, see roms/README.md.
     */
    const bool es = settings.rom_spanish != 0;
    const uint8_t *r0 = spec_is_pentagon_class(m) ? spec_rompent
                      : (m == MACHINE_48K)       ? spec_rom48
                      : (es && m == MACHINE_128K) ? spec_romes0
                      : (es && m == MACHINE_PLUS2) ? spec_romp2es0
                                                 : spec_rom0;
    const uint8_t *r1 = (m == MACHINE_48K)       ? spec_rom48
                      : (es && m == MACHINE_128K) ? spec_romes1
                      : (es && m == MACHINE_PLUS2) ? spec_romp2es1
                      : (m == MACHINE_PLUS2)     ? spec_romp2
                                                 : spec_rom1;

    memcpy(rom_window[ROMW_0], r0, SPEC_BANK_SIZE);
    memcpy(rom_window[ROMW_1], r1, SPEC_BANK_SIZE);
    memcpy(rom_window[ROMW_TRDOS], trdos_image(), SPEC_BANK_SIZE);
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
        case MACHINE_16K:      return "ZX Spectrum 16K";
        case MACHINE_PLUS2A:   return "ZX Spectrum +2A";
        case MACHINE_TC2048:   return "Timex TC2048";
        case MACHINE_GAMA:     return "Didaktik Gama 89";
        case MACHINE_LENINGRAD1: return "Leningrad-1";
        case MACHINE_LENINGRAD2: return "Leningrad-2";
        case MACHINE_BYTE:     return "Byte";
        case MACHINE_PENT512:  return "Pentagon 512";
        case MACHINE_PENT1024: return "Pentagon 1024SL";
        case MACHINE_ZS1024:   return "Scorpion ZS-1024";
        case MACHINE_SCORPEVO: return "ScorpEvo";
        case MACHINE_KAY256:   return "Nemo KAY-256";
        case MACHINE_KAY1024:  return "Nemo KAY-1024";
        case MACHINE_KAY1024B: return "Nemo KAY-1024 (2010)";
        case MACHINE_PHOENIX:  return "ZXM-Phoenix";
        case MACHINE_ATM1:     return "ATM-Turbo 1";
        case MACHINE_ATM2:     return "ATM-Turbo 2";
        case MACHINE_ATM2P:    return "ATM-Turbo 2+";
        case MACHINE_ATM2X:    return "ATM-Turbo 2+ xBIOS";
        case MACHINE_ATM3:     return "ATM-Turbo 3";
        case MACHINE_ALF:      return "ALF TV Game";
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
    if (spec_is_48k_class(spec_machine))        base = SPEC_TSTATES_PER_FRAME_48;
    else if (spec_is_pentagon_class(spec_machine) || spec_machine == MACHINE_SCORPEVO)
                                                base = SPEC_TSTATES_PER_FRAME_PENT;
    else if (spec_machine == MACHINE_ZS1024)    base = 70784u;     // the Green board
    else if (spec_kay_level(spec_machine))      base = 69888u;     // KAY: a 48K's raster
    // ATM: the 48K frame, but BIOS 1.06.02 times its frame with a loop that
    // only works with 316 lines.
    else if (spec_machine == MACHINE_ATM2)      base = 316u * 224u;
    else if (spec_is_atm(spec_machine))         base = 69888u;
    else                                        base = SPEC_TSTATES_PER_FRAME;
    return base * turbo_num() / 2;
}

/*
 * How long ~INT is held, in T-states at 3.5 MHz. The Sinclair ULA holds
 * it 32; the 128K's 36; the Leningrad-1's home-made logic only 15, which
 * is short enough that a few programs that sit in a long instruction
 * miss it -- as they did on the real one.
 */
static uint32_t int_tstates(void) {
    switch (spec_machine) {
        case MACHINE_LENINGRAD1: return 15u;
        case MACHINE_PENTAGON:
        case MACHINE_PENT512:
        case MACHINE_PENT1024:   return 32u;
        default: return spec_is_48k_class(spec_machine) ? 32u : SPEC_INT_TSTATES;
    }
}

// ============================================================================
// State
// ============================================================================
uint8_t spec_ram[SPEC_RAM_BANKS][SPEC_BANK_SIZE] __attribute__((aligned(4)));

/* The banks above the first eight, in the PSRAM arena (arena.h), and the
 * one of them that is mapped. */
#define spec_ram_ext ((uint8_t (*)[SPEC_BANK_SIZE])(psram_arena + ARENA_RAM_OFF))

/* How many banks this machine has: 4 MB on the ATM-Turbo 3, 1 MB on the rest. */
unsigned spec_ram_banks(void) { return spec_machine == MACHINE_ATM3 ? SPEC_MAX_BANKS_BIG : SPEC_MAX_BANKS; }
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
uint8_t *spec_ram_page(const unsigned page) {
    if (page < SPEC_RAM_BANKS) return spec_ram[page];
    if (page >= spec_ram_banks()) return spec_ram[page & 7u];
    // A page that is staged has its newest bytes in the stage; put them back.
    if (staged_bank == (int)page) spec_bank_flush();
    return spec_ram_ext[page - SPEC_RAM_BANKS];
}

const uint8_t *spec_bank_host(const unsigned bank) {
    if (bank < SPEC_RAM_BANKS) return spec_ram[bank];
    if (bank >= spec_ram_banks()) return spec_ram[bank & 7u];
    if (staged_bank == (int)bank) return bank_stage;
    return spec_ram_ext[bank - SPEC_RAM_BANKS];
}

uint8_t *spec_bank_ptr(const unsigned bank) {
    if (bank < SPEC_RAM_BANKS) return spec_ram[bank];
    if (bank >= spec_ram_banks()) return spec_ram[bank & 7u];   /* nothing there */
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
volatile uint8_t  spec_port_eff7   = 0;   /* the Pentagon 1024SL's */
volatile bool     spec_pent16c;
static bool       pent_cache_in;          /* IN #FB put the cache page at #0000 */

/*
 * The Pentagon's cache: 16 KB per ROM latch that can sit where the ROM
 * does. In the PSRAM and mapped straight through the XIP cache -- it is
 * used for a few drivers' workspace, rarely enough that the stall on a
 * miss does not matter, and it is not worth 32 KB of SRAM.
 */
static uint8_t pent_cache[2][SPEC_BANK_SIZE] __attribute__((section(".psram")));
volatile uint8_t  spec_timex_hsr   = 0;   /* the Timex's 8 KB chunk select */
static uint8_t    timex_ff         = 0;   /* the whole byte last sent to the SCLD's #FF */
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

/*
 * The Kempston mouse, as the USB driver leaves it: free-running 8-bit X
 * and Y counters, the buttons (bit 0 left, 1 right, 2 middle, pressed =
 * 1), and the wheel's counter, starting at 15 so an untouched wheel
 * mouse reads exactly like a two-button one.
 */
volatile uint8_t spec_mouse_x, spec_mouse_y, spec_mouse_buttons;
volatile uint8_t spec_mouse_wheel = 0x0F;
volatile int16_t spec_mouse_dx, spec_mouse_dy;

/*
 * The Profi's serial mouse: a Microsoft mouse on the K580VV51 (an 8251),
 * after pico-speccy's Profi.cpp and the FPGA's serial_mouse.vhd.
 *
 *   #F3  read: status -- TxRDY+TxE always, RxRDY when a byte waits;
 *        write: command, D2 (RxE) arms the receiver
 *   #D3  read: the waiting byte, and the next one comes up
 *   #B3 / #93  write D0: the interrupt enable
 *
 * A packet is three bytes, 01LRyyxx 00xxxxxx 00yyyyyy, built from the
 * USB deltas at half rate with the remainder kept. With the interrupt
 * enabled, in CP/M, a waiting byte holds INT and puts #E7 -- RST #20 --
 * on the bus at the acknowledge, as the board's hw_int.vhd does.
 */
static uint8_t sm_ctl, sm_inten, sm_last_btns;
static uint8_t sm_pkt[3], sm_pos = 3;
static bool    sm_rxrdy;
static bool    frame_int_on;           // the frame's own interrupt is up

static void sm_build(void) {
    if (sm_pos < 3 || !(sm_ctl & 0x04u)) return;
    const uint8_t b = spec_mouse_buttons;
    const uint8_t btns = (uint8_t)(((b & 1u) ? 0x20u : 0) | ((b & 2u) ? 0x10u : 0));
    int dx = spec_mouse_dx / 2, dy = spec_mouse_dy / 2;
    if (dx > 127) dx = 127; else if (dx < -128) dx = -128;
    if (dy > 127) dy = 127; else if (dy < -128) dy = -128;
    if (!dx && !dy && btns == sm_last_btns) return;
    spec_mouse_dx = (int16_t)(spec_mouse_dx - dx * 2);
    spec_mouse_dy = (int16_t)(spec_mouse_dy - dy * 2);
    sm_last_btns = btns;
    sm_pkt[0] = (uint8_t)(0x40u | btns | (((uint8_t)dy >> 4) & 0x0Cu) | (((uint8_t)dx >> 6) & 0x03u));
    sm_pkt[1] = (uint8_t)dx & 0x3Fu;
    sm_pkt[2] = (uint8_t)dy & 0x3Fu;
    sm_pos = 0;
    sm_rxrdy = true;
}

static inline bool sm_int(void) {
    return sm_rxrdy && sm_inten && (spec_port_dffd & 0x20u);
}

/* The INT line, from the frame's interrupt and the mouse's. */
static void sm_update_int(void) {
    const bool m = sm_int();
    z80_int_vector = m ? 0xE7u : 0xFFu;
    z80_set_int(frame_int_on || m);
}
volatile bool    spec_mouse_seen;

/*
 * Gigascreen "Auto": a program that flips the displayed bank on every
 * frame is showing two pictures meant to be seen at once. Three frames
 * in a row with a flip in them turn the blend on; a second without one
 * turns it off -- pico-speccy's gigascreenAutoFlip(), by another route.
 */
static uint32_t   giga_flips;
static uint32_t   giga_run, giga_quiet;
volatile bool     spec_giga_auto;
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
    return !spec_is_48k_class(spec_machine) || settings.ay48;
}

bool spec_have_ay(void) { return have_ay(); }

/*
 * Which AY the ports are talking to. The chips themselves are on core 0
 * now (see sound.h); this side only decodes, mirrors and posts.
 */
static uint8_t ay_sel;

/*
 * TurboSound FM. The YM2203 behind each AY takes the registers above
 * #0F through the same two ports; #F8-#FF written to #FFFD are the
 * board's pseudo-registers, %11111frc -- c the chip, r "IN #FFFD reads
 * the status" (0 = on), f the FM DAC (0 = on). After pico-speccy's
 * Ports.cpp and NedoPC's tfm-prg manual.
 */
static uint8_t tsfm_reg[2];             // the full register number, per chip
static bool    tsfm_status;

/*
 * The YMF262's timers, modelled here on core 1 because a driver finds
 * the chip by starting timer 1 and reading the status back -- and the
 * chip itself is on core 0, behind the queue, where a read cannot wait.
 */
static uint8_t  opl_addr;
static uint8_t  opl_t1, opl_t2, opl_ctrl;
static uint32_t opl_t1_start, opl_t2_start;   // absolute T-states
static uint8_t  opl_status;

static uint32_t abs_t(const uint32_t t) { return spec_frames * spec_frame_tstates() + t; }

static void opl_ctrl_write(const uint8_t v, const uint32_t t) {
    if (v & 0x80u) { opl_status = 0; return; }      // IRQ reset
    if ((v & 0x01u) && !(opl_ctrl & 0x01u)) opl_t1_start = abs_t(t);
    if ((v & 0x02u) && !(opl_ctrl & 0x02u)) opl_t2_start = abs_t(t);
    opl_ctrl = v;
}

static uint8_t opl_status_read(const uint32_t t) {
    const uint32_t now = abs_t(t);
    // Timer 1 ticks every 80 us, timer 2 every 320; 3.5 T-states a us.
    if ((opl_ctrl & 0x01u) && !(opl_ctrl & 0x40u)
        && now - opl_t1_start >= (256u - opl_t1) * 280u) opl_status |= 0xC0u;
    if ((opl_ctrl & 0x02u) && !(opl_ctrl & 0x20u)
        && now - opl_t2_start >= (256u - opl_t2) * 1120u) opl_status |= 0xA0u;
    return opl_status;
}

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
static void contend_remask(void);
static void contend_setup(void);
static uint8_t floating_bus(uint32_t t);

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
    // The screen may now be visible through different windows, and
    // which of them the ULA contends may have changed.
    if (unlikely(ula_beam)) ula_rewatch();
    if (unlikely(z80_contend.on)) contend_remask();
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

/*
 * Which bank is at #C000, on every machine that pages it.
 *
 * In one place because the Beta Disk's arm needs it too: TR-DOS changes
 * the bottom window and nothing else, and it used to put back only
 * #7FFD's three bits -- so a Scorpion or a Profi in TR-DOS with a high
 * bank selected was quietly looking at the wrong one.
 */
static unsigned c000_bank(void) {
    const uint8_t v = spec_port_7ffd;
    switch (spec_machine) {
        case MACHINE_SCORPION: return (v & 0x07u) | ((spec_port_1ffd & 0x10u) >> 1);
        case MACHINE_ZS1024:
        case MACHINE_SCORPEVO:
            // 1FFD D6,D7 are two more page bits above D4: 64 pages, 1 MB.
            return (v & 0x07u) | ((spec_port_1ffd & 0x10u) >> 1) | ((spec_port_1ffd & 0xC0u) >> 2);
        case MACHINE_KAY256:
            return (v & 0x07u) | ((spec_port_1ffd & 0x10u) >> 1);
        case MACHINE_KAY1024:
        case MACHINE_KAY1024B:
            return (v & 0x07u) | ((spec_port_1ffd & 0x10u) >> 1)
                 | ((spec_port_1ffd & 0x80u) >> 3) | ((kay_7ffd_d7 & 0x80u) >> 2);
        case MACHINE_PHOENIX: {
            // Its own order, and D6 makes 2 MB of which 1 MB is fitted here.
            const unsigned p = (v & 0x07u) | ((kay_7ffd_d7 & 0x80u) >> 4)
                             | (spec_port_1ffd & 0x50u) | ((spec_port_1ffd & 0x80u) >> 2);
            return p < SPEC_MAX_BANKS ? p : (v & 0x07u);
        }
        case MACHINE_PROFI:    return (v & 0x07u) | ((spec_port_dffd & 0x07u) << 3);
        case MACHINE_PENT512:
        case MACHINE_PENT1024: {
            unsigned b = v & 0x07u;
            if (spec_machine == MACHINE_PENT1024 && (spec_port_eff7 & 0x04u)) return b;
            if (v & 0x40u) b += 8u;
            if (v & 0x80u) b += 16u;
            if (spec_machine == MACHINE_PENT1024 && (v & 0x20u)) b += 32u;
            return b;
        }
        default:               return v & 0x07u;
    }
}

static void __not_in_flash_func(apply_machine_paging)(void) {
    const uint8_t v = spec_port_7ffd;

    // The Profi's CP/M mode opens the disk controller without TR-DOS.
    beta_ports_open = (spec_machine == MACHINE_PROFI) && (spec_port_dffd & 0x20u);

    // The ATM's memory manager owns every window. See atm.c.
    if (spec_is_atm(spec_machine)) { atm_remap(); return; }

    if (spec_machine == MACHINE_ALF) {
        const unsigned b = (alf_sel & 0x7Fu) % 64u;
        if (alf_sram)                                      z80_map_page(0, alf_ram, alf_ram);
        else if (alf_sel & 0x80u)                          z80_map_page(0, rom_window[b < alf_banks ? ROMW_2 : ROMW_3], NULL);
        else                                               z80_map_page(0, rom_window[b < 2u ? b : ROMW_3], NULL);
        z80_map_page(1, spec_ram[5], spec_ram[5]);
        z80_map_page(2, spec_ram[2], spec_ram[2]);
        z80_map_page(3, spec_ram[v & 7u], spec_ram[v & 7u]);
        spec_screen_bank = (v & 0x08) ? 7 : 5;
        return;
    }

    /*
     * The Beta Disk's ROM sits on top of everything.
     *
     * When the interface has decided its ROM is in -- see spec_opfetch()
     * for the rule, which is about where the CPU is fetching from and
     * nothing else -- it replaces whatever the bottom window would
     * otherwise hold, on every machine. So this runs first and the rest
     * of the function only decides what is underneath it.
     */
    if (unlikely(beta_rom_paged()) && !spec_is_scorpion_class(spec_machine)) {
        z80_map_page(0, rom_window[ROMW_TRDOS], NULL);
        if (spec_is_48k_class(spec_machine)) {
            z80_map_page(1, spec_ram[5], spec_ram[5]);
            z80_map_page(2, spec_ram[2], spec_ram[2]);
            z80_map_page(3, spec_ram[0], spec_ram[0]);
            spec_screen_bank = 5;
        } else {
            z80_map_page(1, spec_ram[5], spec_ram[5]);
            z80_map_page(2, spec_ram[2], spec_ram[2]);
            uint8_t *p = spec_bank_ptr(c000_bank());
            z80_map_page(3, p, p);
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
    if (spec_is_48k_class(spec_machine)) {
        z80_map_page(0, (uint8_t *)basic_rom(), NULL);
        z80_map_page(1, spec_ram[5], spec_ram[5]);
        if (spec_machine == MACHINE_16K) {
            // Nothing fitted above #8000: reads float high, writes vanish.
            z80_map_page(2, rom_window[ROMW_2], NULL);
            z80_map_page(3, rom_window[ROMW_2], NULL);
        } else {
            z80_map_page(2, spec_ram[2], spec_ram[2]);
            z80_map_page(3, spec_ram[0], spec_ram[0]);
        }
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

    if (spec_is_scorpion_class(spec_machine)) {
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
        const unsigned rom14 = (v & 0x10u) ? 1u : 0u;
        const unsigned kay = spec_kay_level(spec_machine);
        /*
         * The KAY's ROM A15 is #1FFD D3 XOR the DOS line, its A14 #7FFD
         * D4 -- the same four roles, but D3 swaps the pair rather than
         * D1 overriding it. Phoenix adds the Scorpion's D1 on top.
         * After pico-speccy's scorpionRomUpdate().
         */
        const unsigned rn = (kay == 4u && (w & 0x02u)) ? 2u
                          : kay ? ((((w & 0x08u) ? 2u : 0u) ^ (dos ? 2u : 0u)) | rom14)
                          : (w & 0x02u) ? 2u
                          : ((dos ? 2u : 0u) | rom14);

        /* D0 puts RAM bank 0 at #0000 instead of any ROM. */
        if (w & 0x01u) z80_map_page(0, spec_ram[0], spec_ram[0]);
        else           z80_map_page(0, (uint8_t *)((rn == 1) ? basic_rom()
                                                            : rom_window[rn]), NULL);

        z80_map_page(1, spec_ram[5], spec_ram[5]);
        z80_map_page(2, spec_ram[2], spec_ram[2]);

        /* 256 KB: #1FFD D4 is the fourth bit of the #C000 page. */
        uint8_t *p = spec_bank_ptr(c000_bank());
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
        uint8_t *p = spec_bank_ptr(c000_bank());
        z80_map_page(3, p, p);

        spec_screen_bank = (v & 0x08) ? 7 : 5;
        return;
    }

    if (spec_machine == MACHINE_PENT512 || spec_machine == MACHINE_PENT1024) {
        /*
         * After pico-speccy's Pentagon.cpp and Ports.cpp. #7FFD bits 6
         * and 7 are page bits 3 and 4; on the 1024SL bit 5 is bit 5 --
         * unless #EFF7 bit 2 has put it back to being the 128K lock.
         */
        const uint8_t e = spec_port_eff7;
        const bool is1024 = spec_machine == MACHINE_PENT1024;
        const unsigned bank = c000_bank();
        const unsigned latch = (v & 0x10u) ? 1u : 0u;
        if ((is1024 && (e & 0x08u)) || pent_cache_in) {
            uint8_t *c = pent_cache[latch];
            z80_map_page(0, c, c);
        } else {
            z80_map_page(0, (uint8_t *)(latch ? basic_rom() : rom_window[ROMW_0]), NULL);
        }
        z80_map_page(1, spec_ram[5], spec_ram[5]);
        z80_map_page(2, spec_ram[2], spec_ram[2]);
        uint8_t *p = spec_bank_ptr(bank);
        z80_map_page(3, p, p);
        spec_screen_bank = (v & 0x08) ? 7 : 5;
        spec_pent16c = is1024 && (e & 0x01u);
        return;
    }

    if (spec_is_plus3_class(spec_machine)) {
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
/*
 * ProfROM's plane switch: a jump into #0100-#010F from the service ROM
 * picks the plane from the address and the plane it came from. MAME's
 * prof_plane_map, applied over the whole sixteen bytes as ZXMAK2 and
 * pico-speccy do, because ProfROM itself jumps to the in-between ones.
 */
static const uint8_t prof_plane_map[16] = { 0, 1, 2, 3, 3, 3, 3, 2, 2, 2, 0, 1, 1, 0, 1, 0 };

static inline bool prof_tap_armed(void) {
    return (spec_machine == MACHINE_ZS1024 || spec_machine == MACHINE_SCORPEVO)
        && (spec_port_1ffd & 0x02u) && !(spec_port_1ffd & 0x01u);
}

/*
 * The tap itself, on reads as well as fetches and after the byte has
 * gone out -- ProfROM's trampolines switch with LD L,(HL), and with the
 * fetch half alone it ran its RAM test and fell into plane 0's 128 ROM,
 * which is the 48 BASIC ZS-1024 used to start in. pico-speccy found the
 * same (CPU.cpp, peek8: "the load-bearing one").
 */
static void __not_in_flash_func(prof_rtap)(const uint16_t addr, const uint32_t t) {
    (void)t;
    if (!prof_tap_armed()) return;
    const uint8_t plane = prof_plane_map[(addr & 0x0Cu) | (prof_plane & 0x03u)];
    if (plane != prof_plane) {
        prof_plane = plane;
        load_plane(spec_machine, plane);        // the windows already point at the slots
    }
}

static void __not_in_flash_func(spec_opfetch)(const uint16_t addr, const uint32_t t) {
    (void)t;
    if (settings.esxdos) divmmc_opfetch(addr);

    if (spec_is_atm(spec_machine) && !tramp_active) { atm_opfetch(addr); return; }

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
    const bool basic48 = spec_is_48k_class(spec_machine) || (spec_port_7ffd & 0x10);
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
    z80_opfetch_cb = (tramp_active || settings.betadisk || settings.esxdos
                      || spec_is_atm(spec_machine))
                         ? spec_opfetch : NULL;
    z80_rtap_base = 0x0100u;
    z80_rtap_cb   = prof_tap_armed() ? prof_rtap : NULL;
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
/*
 * Which machines carry Mr Gluk's clock: the Pentagons, the Scorpions
 * and KAYs, the Profi and the ATMs, as pico-speccy fits it.
 */
/*
 * The Profi's CP/M mode (#DFFD bit 5) moves the WD1793 to #83, #A3, #C3
 * and #E3 -- register = A6-A5, as UnrealSpeccy decodes it -- and its
 * system register to #BF. Mapped back here onto the ports the Beta Disk
 * already answers; anything else comes back unchanged.
 */
static uint16_t profi_cpm_port(const uint16_t port) {
    if ((port & 0x9Fu) == 0x83u) return (uint16_t)((port & 0xFF00u) | ((port & 0x60u) | 0x1Fu));
    if ((port & 0xFFu) == 0xBFu) return (uint16_t)((port & 0xFF00u) | 0xFFu);
    return port;
}


static bool has_gluk(void) {
    const machine_t m = spec_machine;
    // The ScorpEvo's clock is on the board, not a card (pico-speccy Ports.cpp:1400).
    if (m == MACHINE_SCORPEVO) return true;
    return settings.rtc && (spec_is_pentagon_class(m) || spec_is_scorpion_class(m)
                            || m == MACHINE_PROFI || spec_is_atm(m));
}

/*
 * Which even ports reach the ULA. A Spectrum looks at A0 alone; the
 * TC2068 and the Byte decode the whole low byte, so only #FE (their #F4,
 * #F6 and the Byte's OUT (0) markers must not repaint the border); the
 * ATM1 also wants A2, the ATM2/2+/3 A2 and A1 (atmdscr.htm, #FA is the
 * external bus). After pico-speccy's Ports.cpp.
 */
static inline bool ula_decodes(const uint16_t port) {
    if (port & 1u) return false;
    switch (spec_machine) {
        case MACHINE_TIMEX:
        case MACHINE_BYTE: return (port & 0xFFu) == 0xFEu;
        case MACHINE_ATM1: return (port & 0x04u) != 0;
        default:
            return !spec_is_atm(spec_machine) || (port & 0x06u) == 0x06u;
    }
}

static void __not_in_flash_func(spec_io_out)(const uint16_t port, const uint8_t value,
                                             const uint32_t t) {
    (void)t;
    // The ATM's ports come before everything: its #7FFD and #FF families
    // are decoded its own way. See atm.c.
    if (unlikely(spec_is_atm(spec_machine))) {
        if (ula_decodes(port)) atm_fe_write(port, value);
        if (atm_port_write(port, value)) return;
    }
    // The Profi's serial port, ahead of the Beta decode that #F3/#D3/#B3/#93
    // would otherwise alias onto.
    if (unlikely(spec_machine == MACHINE_PROFI)) {
        const uint8_t lo = (uint8_t)port;
        if (lo == 0xF3u) { sm_ctl = value; if (!(value & 0x04u)) { sm_rxrdy = false; sm_pos = 3; } sm_update_int(); return; }
        if (lo == 0xB3u || lo == 0x93u) { sm_inten = value & 0x01u; sm_update_int(); return; }
        if (lo == 0xD3u) return;                    // the transmit side: nothing listens
    }
    // The hard disk, ahead of the ULA: NEMO's registers are even ports.
    if (unlikely(spec_is_scorpion_class(spec_machine)) && smuc_port_out(port, value)) return;
    if (unlikely(settings.ide) && ide_port_out(port, value)) return;

    // The ALF's ROM-bank select: any OUT with A7 low and A0 high.
    if (unlikely(spec_machine == MACHINE_ALF) && !(port & 0x80u) && (port & 1u)) {
        alf_select(value);
        apply_paging();
        return;
    }

    // The Byte's 8253: #8E, #AE, #CE the counters, #EE the control word.
    if (unlikely(spec_machine == MACHINE_BYTE) && ((port & 0x9Fu) == 0x8Eu)) {
        snd_put(t, SD_PIT0 + ((port >> 5) & 3u), value);
        return;
    }

    if (unlikely(port == 0xDFF7u || port == 0xBFF7u) && has_gluk()) {
        if (port == 0xDFF7u) rtc_select(value); else rtc_write(value);
        return;
    }
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
    /*
     * The SAA1099, on the #FF family with A8 selecting what it is:
     * #00FF is the data port and #01FF the register select. That is the
     * Karabas/SAM convention and the one the Profi's extended periphery
     * uses.
     *
     * Ahead of the SCLD arm below, and never on a Timex. On a Timex the
     * SCLD owns the whole family and a TC2068 has no SAA; on anything
     * else, fitting the SAA means the Timex video CARD is not fitted --
     * which is true of the real machines too, since both answer the
     * same ports and only one card goes in the slot.
     *
     * Nothing in the family answers while the TR-DOS ROM is paged.
     */
    /*
     * The Creative Music System: two SAA1099s on the same #FF family, A9
     * picking the chip, A8 select or data. Ahead of the single SAA, which
     * it replaces when it is fitted.
     */
    if (unlikely(settings.cms) && spec_machine != MACHINE_TIMEX
        && (port & 0xFFu) == 0xFFu && !beta_rom_paged()) {
        const unsigned c = (port >> 9) & 1u;
        snd_put(t, (port & 0x0100u) ? (c ? SD_CMS1_SEL : SD_CMS0_SEL)
                                    : (c ? SD_CMS1_DAT : SD_CMS0_DAT), value);
        return;
    }

    if (unlikely(settings.saa) && spec_machine != MACHINE_TIMEX
        && (port & 0xFFu) == 0xFFu && !beta_rom_paged()) {
        if (port & 0x0100u) {
            snd_mirror.saa_sel = value;
            snd_put(t, SD_SAA_SEL, value);
        } else {
            snd_mirror.saa_reg[snd_mirror.saa_sel & 0x1Fu] = value;
            snd_put(t, SD_SAA_DAT, value);
        }
        return;
    }

    if ((settings.timex || spec_has_scld(spec_machine))
        && (port & 0xFFu) == 0xFFu
        && (spec_machine == MACHINE_TIMEX || !(port & 0x0100u))
        && !beta_rom_paged()) {
        timex_ff = value;
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
    if (unlikely(beta_ports_open)) {
        const uint16_t p = profi_cpm_port(port);
        if (p != port && beta_port_out(p, value)) return;
    }
    if (divmmc_port_out(port, value)) return;
    if (divmmc_zc_out(port, value)) return;

    /*
     * The Timex's horizontal select register, #F4: one bit per 8 KB
     * chunk. Ahead of the ULA -- #F4 is even, and only its full decode
     * keeps it off the border.
     */
    if (spec_machine == MACHINE_TIMEX && (port & 0xFFu) == 0xF4u) {
        spec_timex_hsr = value;
        apply_paging();
        return;
    }

    if (ula_decodes(port)) {                    // the ULA
        if (unlikely(ula_beam) && (value & 0x07) != spec_border) ula_border(t, value & 0x07);
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
        // Only a change is news: border-only writes are most of them.
        if ((value ^ last_fe) & 0x18u) snd_put(t, SD_FE, value);
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
    /*
     * The VGM card's chips (pico-speccy's decode): OPL3 on #C4-#C7, OPLL
     * on #C0/#C1, the SN pair on #C3 (#C9) and #C2. All write-only but
     * the OPL3's status.
     */
    {
        const uint8_t lo = (uint8_t)port;
        if (settings.opl3 && (lo & 0xFCu) == 0xC4u) {
            if (!(lo & 1u)) opl_addr = value;
            else if (!(lo & 2u)) {
                if (opl_addr == 2) opl_t1 = value;
                else if (opl_addr == 3) opl_t2 = value;
                else if (opl_addr == 4) opl_ctrl_write(value, t);
            }
            snd_put(t, SD_OPL0 + (lo & 3u), value);
            return;
        }
        if (settings.opll && (lo == 0xC0u || lo == 0xC1u)) {
            snd_put(t, lo == 0xC1u ? SD_OPLL_D : SD_OPLL_A, value);
            return;
        }
        if (settings.sn76489 && (lo == 0xC3u || lo == 0xC9u || lo == 0xC2u)) {
            snd_put(t, lo == 0xC2u ? SD_SN1 : SD_SN0, value);
            return;
        }
    }

    if (settings.covox && (port & 0xFF) == (settings.covox == COVOX_DD ? 0xDDu : 0xFBu))
        snd_put(t, SD_COVOX, value);
    if (settings.specdrum && (port & 0xFF) == 0xDF) snd_put(t, SD_SPECDRUM, value);
    if (settings.soundrive) {
        switch (port & 0xFF) {
            case 0x0F: snd_put(t, SD_SD0, value); break;
            case 0x1F: snd_put(t, SD_SD1, value); break;
            case 0x4F: snd_put(t, SD_SD2, value); break;
            case 0x5F: snd_put(t, SD_SD3, value); break;
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
        if ((settings.turbosound || settings.tsfm) && (value & 0xF8u) == 0xF8u
            && (value >= 0xFEu || settings.tsfm)) {
            ay_sel = (uint8_t)(value & 1 ? 0 : 1);   // 0xFF = first, 0xFE = second
            if (settings.tsfm) {
                tsfm_status = !(value & 0x02u);
                snd_put(t, SD_FM_ON, (value & 0x04u) ? 0 : 1);
            }
        } else if (settings.tsfm && (value & 0xFEu) == 0xF0u) {
            // AlexZor's VGM plugin: #F0/#F1 select a chip and the FM.
            ay_sel = value & 1u;
            tsfm_status = false;
            snd_put(t, SD_FM_ON, 1);
        } else {
            snd_mirror.latched[ay_sel] = value & 0x0Fu;
            tsfm_reg[ay_sel] = value;
            snd_put(t, ay_sel ? SD_AY_SEL1 : SD_AY_SEL0, value);
            if (settings.tsfm && value >= 0x10u) snd_put(t, ay_sel ? SD_OPN1_A : SD_OPN0_A, value);
        }
        return;
    }
    if (have_ay() && (port & 0xC002) == 0x8000) {   // data
        if (settings.tsfm && tsfm_reg[ay_sel] >= 0x10u) {
            snd_put(t, ay_sel ? SD_OPN1_D : SD_OPN0_D, value);
            return;
        }
        snd_mirror.reg[ay_sel][snd_mirror.latched[ay_sel]] = value;
        snd_put(t, ay_sel ? SD_AY_DAT1 : SD_AY_DAT0, value);
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
    if ((spec_machine == MACHINE_SCORPION || spec_machine == MACHINE_ZS1024
         || spec_machine == MACHINE_SCORPEVO)
        && (port & 0xC002u) == 0 && (port & 0x0020u)) {
        spec_port_1ffd = value;
        apply_paging();
        update_opfetch_hook();          // ProfROM's plane switch arms on D1
        return;
    }

    /* The KAY's #1FFD: A15=A14=0, A1=0, A0=1. */
    if (spec_kay_level(spec_machine) && (port & 0xC003u) == 0x0001u) {
        spec_port_1ffd = value;
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
    if (spec_is_plus3_class(spec_machine) && (port & 0xF002u) == 0x1000u
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
    /*
     * The 1024SL's #EFF7, decoded on A15-A12 and A3 as Ports.cpp has it
     * -- ahead of #7FFD, whose looser decode would otherwise take it.
     */
    if (spec_machine == MACHINE_PENT1024 && (port & 0xF008u) == 0xE000u) {
        if (!paging_locked) {
            spec_port_eff7 = value;
            apply_paging();
        }
        return;
    }

    if (!spec_is_48k_class(spec_machine) && (port & 0x8002) == 0 && !paging_locked) {
        if ((value ^ spec_port_7ffd) & 0x08u) giga_flips++;
        kay_7ffd_d7 = value & 0x80u;        // the KAY 1024's +32
        spec_port_7ffd = value;
        // On a 1024SL bit 5 is a page bit, and only locks in 128K mode.
        if ((value & 0x20) && (spec_machine != MACHINE_PENT1024 || (spec_port_eff7 & 0x04u)))
            paging_locked = true;
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
    if (bit > JOY_BIT_FIRE4) return;
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
 * Kempston: its own port, active high, five bits and three more.
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
    // Bits 5-7: the second and third buttons of the extended interfaces,
    // which the games that know them read and the rest mask off.
    if (JOY(JOY_BIT_FIRE2)) v |= 0x20;
    if (JOY(JOY_BIT_FIRE3)) v |= 0x40;
    if (JOY(JOY_BIT_FIRE4)) v |= 0x80;
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

static uint8_t __not_in_flash_func(spec_io_in)(const uint16_t port, const uint32_t t) {
    if (unlikely(spec_is_atm(spec_machine))) {
        uint8_t a;
        if (atm_port_read(port, &a)) return a;
    }
    if (unlikely(port == 0xBFF7u) && has_gluk()) return rtc_read();
    if (unlikely(spec_machine == MACHINE_PROFI)) {
        const uint8_t lo = (uint8_t)port;
        if (lo == 0xF3u) { sm_build(); sm_update_int(); return (uint8_t)(0x05u | (sm_rxrdy ? 0x02u : 0)); }
        if (lo == 0xD3u) {
            const uint8_t v = sm_pkt[sm_pos > 2 ? 2 : sm_pos];
            if (sm_rxrdy) {
                sm_rxrdy = false;
                if (++sm_pos < 3) sm_rxrdy = true; else sm_build();
            }
            sm_update_int();
            return v;
        }
    }
    if (unlikely(spec_machine == MACHINE_ALF) && ((port & 0xFDu) == 0x1Du)) {
        const bool on = !(port & 0x02u);
        if (on != alf_sram) { alf_sram = on; apply_paging(); }
        if (on) return 0xFF;                // #1F carries on to the joystick
    }
    if (unlikely(spec_is_scorpion_class(spec_machine))) {
        uint8_t d;
        if (smuc_port_in(port, &d)) return d;
    }
    if (unlikely(settings.ide)) {
        uint8_t d;
        if (ide_port_in(port, &d)) return d;
    }
    if (settings.opl3 && (port & 0xFFu) == 0xC4u) return opl_status_read(t);
    if (settings.ulaplus && port == 0xFF3Bu) {
        const uint8_t reg = spec_ulaplus_reg;
        if ((reg & 0xC0u) == 0x00u) return spec_ulaplus_pal[reg & 0x3Fu];
        if ((reg & 0xC0u) == 0x40u) return spec_ulaplus_on ? 1u : 0u;
        return 0xFFu;
    }

    /*
     * The SCLD reads back the whole byte last written to #FF, and #F4 the
     * chunk select. The TC2068 ROM flips the dock/EX-ROM bit with IN, SET 7,
     * OUT -- answered with the floating bus's 0xFF, that also set hi-res,
     * the second screen and the interrupt inhibit. After pico-speccy.
     */
    if ((settings.timex || spec_has_scld(spec_machine))
        && (port & 0xFFu) == 0xFFu
        && (spec_machine == MACHINE_TIMEX || !(port & 0x0100u))
        && !beta_rom_paged())
        return timex_ff;
    if (spec_machine == MACHINE_TIMEX && (port & 0xFFu) == 0xF4u) return spec_timex_hsr;

    uint8_t v;
    if (tape_port_in(port, &v)) return v;
    if (beta_port_in(port, &v)) return v;
    if (unlikely(beta_ports_open)) {
        const uint16_t p = profi_cpm_port(port);
        if (p != port && beta_port_in(p, &v)) return v;
    }
    if (fdc_port_in(port, &v)) return v;
    if (divmmc_port_in(port, &v)) return v;
    if (divmmc_zc_in(port, &v)) return v;

    /*
     * The Pentagon 512/1024's cache switch: an IN from #FB puts the cache
     * page where the ROM was, an IN from #7B takes it away. Only on those
     * two -- a plain Pentagon 128 reads #FB as the printer port, and some
     * loaders probe it.
     */
    if ((spec_machine == MACHINE_PENT512 || spec_machine == MACHINE_PENT1024)
        && ((port & 0xFFu) == 0xFBu || (port & 0xFFu) == 0x7Bu)) {
        const bool in = (port & 0xFFu) == 0xFBu;
        if (in != pent_cache_in) { pent_cache_in = in; apply_paging(); }
        return 0xFF;
    }

    /*
     * The Kempston mouse, ahead of the joystick that can share #DF.
     *
     * Decoded as the original interface was on the high byte -- X at A10-A8
     * 011, Y at 111, the buttons at A9=1 A8=0 -- with the low byte held to
     * the full #DF so it does not steal the joystick's or the Beta's reads.
     * The buttons answer 0xFF until a mouse has ever been seen, which is
     * how software decides there is none. After pico-speccy's Ports.cpp.
     */
    /* On the Profi the mouse is decoded on all sixteen bits and only out of
     * CP/M: #0ADF is the Karabas clock's presence probe. */
    if (settings.mouse && (port & 0xFFu) == 0xDFu
        && !(spec_machine == MACHINE_PROFI
             && ((spec_port_dffd & 0x20u) || !(port == 0xFBDFu || port == 0xFFDFu || port == 0xFADFu)))) {
        const uint16_t hi = port & 0x0700u;
        if (hi == 0x0300u) return spec_mouse_x;
        if (hi == 0x0700u) return spec_mouse_y;
        if ((hi & 0x0300u) == 0x0200u) {
            if (!spec_mouse_seen) return 0xFF;
            const uint8_t b = spec_mouse_buttons;
            return (uint8_t)(((spec_mouse_wheel & 0x0Fu) << 4) | 0x08u |
                             ((b & 4u) ? 0 : 0x04u) | ((b & 1u) ? 0 : 0x02u) | ((b & 2u) ? 0 : 0x01u));
        }
    }

    if (settings.joystick == JOY_KEMPSTON &&
        ((port & 0xFF) == (settings.kempston_port == 1 ? 0xDF : 0x1F)
         || (settings.kempston_port == 2 && (port & 0xFF) == 0x37)
         || (settings.kempston_port == 3 && (port & 0xFF) == 0x5F)))
        return kempston_byte();

    if (settings.joystick == JOY_FULLER && (port & 0xFF) == 0x7F)
        return fuller_byte();

    if (ula_decodes(port)) {                    // the ULA: keyboard
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
        /*
         * The EAR bit, from a real tape if one is playing.
         *
         * Their rule: the live input wins only when no image is
         * mounted. With a tape mounted the ROM trap is doing the
         * loading and there is nothing to decode; without one, the
         * bit is whatever the machine last wrote to the ULA, which is
         * what a Spectrum with nothing plugged in reads back.
         */
        uint8_t ear;
        if (unlikely(tapeplay_active))
            ear = tapeplay_ear(t) ? 0x40 : 0;
        else if (unlikely(tapein_running()) && !tape_mounted())
            ear = tapein_level(t) ? 0x40 : 0;
        else
            ear = settings.issue2 ? ((last_fe & 0x18) ? 0x40 : 0)
                                  : ((last_fe & 0x10) ? 0x40 : 0);
        return (uint8_t)(result | 0xA0 | ear);
    }

    // The AY's read-back port is the select port, not the data one.
    if (have_ay() && (port & 0xC002) == 0xC000)
        // TurboSound FM in ready-poll mode: the YM2203's status, never busy.
        return (settings.tsfm && tsfm_status) ? 0x00u
                                              : snd_mirror.reg[ay_sel][snd_mirror.latched[ay_sel]];

    /*
     * Everything else is the floating bus: whatever the ULA was fetching
     * at that moment, because nothing else is driving the data lines.
     *
     * It used to be 0xFF here, because the picture ran on its own clock
     * and nothing knew where the beam was. Every handler is told its
     * T-state now, and the beam is a function of the T-state -- so a
     * 48K or a 128K answers with the bitmap or attribute byte under the
     * beam, in the order the ULA reads them, and 0xFF in the border and
     * the gaps. Arkanoid and Sidewize time themselves off this. The
     * Pentagon, the +2A/+3 and the clones answer 0xFF, as theirs did.
     */
    return floating_bus(t);
}

/* The test tone lives with the rest of the sound now. See sound.c. */
void spec_test_tone(const bool on) { sound_test_tone(on); }
bool spec_test_tone_is_on(void) { return sound_test_tone_is_on(); }

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
         * An arrow is CAPS SHIFT plus a digit, which is what the
         * Spectrum's own cursor keys are and what the 128K menu reads --
         * and the joystick as well when there is one to be, the way the
         * USB keyboard does it.
         */
        if (settings.joystick != JOY_NONE && settings.cursor_as_joy) {
            switch (c) {
                case 'A': held_joy = 1u << JOY_BIT_UP;    break;
                case 'B': held_joy = 1u << JOY_BIT_DOWN;  break;
                case 'C': held_joy = 1u << JOY_BIT_RIGHT; break;
                case 'D': held_joy = 1u << JOY_BIT_LEFT;  break;
                default:  break;
            }
            joy_state |= held_joy;
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
    arena_big_ram = spec_ram_banks() > SPEC_MAX_BANKS;
    ula_set_enabled(settings.beam != 0);
    contend_setup();
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

    memset(&snd_mirror, 0, sizeof snd_mirror);
    snd_put(0, SD_RESET, 0);
    ay_sel         = 0;
    spec_beeper    = false;
    last_fe        = 0;
    joy_state      = 0;
    spec_ay_writes = 0;

    esc_state      = 0;
    held_joy       = 0;
    type_str       = NULL;
    paging_locked  = false;

    spec_port_7ffd = 0;
    spec_port_1ffd = 0;
    spec_port_dffd = 0;
    sm_ctl = 0; sm_inten = 0; sm_pos = 3; sm_rxrdy = false; sm_last_btns = 0;
    spec_mouse_dx = spec_mouse_dy = 0;
    z80_int_vector = 0xFF;
    spec_port_eff7 = 0;
    spec_pent16c   = false;
    pent_cache_in  = false;
    if (spec_is_atm(spec_machine)) atm_reset();
    ide_reset();
    smuc_reset();
    spec_timex_hsr = 0;
    timex_ff = 0;
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
    if (beta_boot && !spec_is_48k_class(spec_machine)) spec_port_7ffd = 0x10;
    beta_set_rom_paged(beta_boot);

    apply_paging();

    z80_io_in  = spec_io_in;
    z80_io_out = spec_io_out;
    update_opfetch_hook();

    // The stage-0 wiring watch, if a quick check left it on. A machine
    // never wants it and it is expensive: see z80_bus.h.
    z80_edge_watch_stop();

    z80_reset();
    z80_park();         // it may sit a while before it runs: see z80_park()
}

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
 * Run the machine to t_end.
 *
 * The frame used to be cut into one slice per audio sample, with core 1
 * mixing a sample between slices. The mixing is core 0's now, so a slice
 * is just a place to stop: often enough that core 0 can render a frame
 * as it goes rather than all of it at the end.
 */
#define FRAME_STOPS 16u

__force_inline static void run_slice(const uint32_t t_end) {
    // The Profi's mouse can want the CPU at any point of the frame.
    if (unlikely(spec_machine == MACHINE_PROFI) && (sm_ctl & 0x04u)) { sm_build(); sm_update_int(); }
    z80_run_until(t_end);
    snd_put(t_end, SD_MARK, 0);
}

/*
 * The machine's own clock, which is not the same on every machine.
 *
 * A 128K's crystal makes 3.5469 MHz, a 48K's 3.5 MHz, a Pentagon's and
 * a Scorpion's 3.5 MHz, a Timex's 3.528 MHz -- and each frame is however
 * many T-states long at that clock. Pacing every machine to the 128K's
 * 50.01 Hz made a 48K run 1.3 % fast and a Pentagon 2.4 %.
 */
uint32_t spec_cpu_hz(void) {
    uint32_t base;
    switch (spec_machine) {
        case MACHINE_128K:
        case MACHINE_PLUS2:
        case MACHINE_PLUS3:
        case MACHINE_PLUS2A: base = SPEC_CPU_HZ; break;
        case MACHINE_TIMEX: base = 3528000u;     break;
        default:            base = 3500000u;     break;
    }
    return base * turbo_num() / 2u;
}

/*
 * Pacing by the wall clock.
 *
 * This used to be a tuner: it adjusted the pace -- how long the engine
 * waits after each clock edge -- until five frames took a hundred
 * milliseconds. That made the machine's speed a function of what it was
 * doing, because an I/O cycle costs the engine more than a memory cycle,
 * and it hid how much room was left.
 *
 * Now the engine runs at the measured floor, as fast as it is safe to
 * run, and each frame ends by waiting for a deadline that advances by
 * exactly one frame's length of real time. The wait *is* the headroom,
 * and it is measured rather than guessed. A frame that overruns its
 * deadline is late; a run of them means the machine cannot keep up, and
 * the deadline is dragged forward rather than chased.
 *
 * The frame's length is kept in microseconds with a 16-bit fraction,
 * because 70908 / 3546900 s is 19991.5 us and dropping the half every
 * frame would be 25 us a second of drift.
 */
static uint64_t pace_deadline;          // us, absolute
static uint32_t frame_us_q16;           // one frame, us << 16
static uint32_t frame_us_frac;
static uint32_t spf_q16;                // samples per frame << 16
static uint32_t spf_frac;
static uint64_t frame_started;

spec_timing_t spec_timing;

static void pace_recompute(void) {
    contend_setup();            // turbo and the machine both decide it
    const uint32_t hz = spec_cpu_hz();
    const uint32_t len = spec_frame_tstates();
    frame_us_q16 = (uint32_t)(((uint64_t)len * 1000000u << 16) / hz);
    // Samples per frame at the DAC's real rate. See audio_rate_millihz().
    spf_q16 = (uint32_t)(((uint64_t)len * audio_rate_millihz() << 16) / ((uint64_t)hz * 1000u));
    samples_per_frame = spf_q16 >> 16;
}

/*
 * How many samples this frame is cut into: the whole part, plus one
 * whenever the fraction carries.
 */
static uint32_t next_frame_samples(void) {
    spf_frac += spf_q16 & 0xFFFFu;
    uint32_t n = spf_q16 >> 16;
    if (spf_frac >= 0x10000u) { spf_frac -= 0x10000u; n++; }
    return n;
}

/*
 * Wait for this frame's deadline, and account for it.
 *
 * Busy, because this is core 1 and there is nothing else for it to do:
 * a sleep would hand the time to nobody and come back late.
 */
static void pace_frame(void) {
    frame_us_frac += frame_us_q16 & 0xFFFFu;
    uint32_t step = frame_us_q16 >> 16;
    if (frame_us_frac >= 0x10000u) { frame_us_frac -= 0x10000u; step++; }

    /*
     * The ring is the backstop. The DAC and this deadline run off one
     * crystal, so they do not drift -- but a burst of late frames drains
     * it, and a deadline that has been dragged forward has lost the time
     * for good. A tenth of a millisecond a frame either way puts it back
     * without anyone hearing the difference.
     */
    const uint32_t level = audio_ring_level();
    if (level < AUDIO_RING_SIZE / 4u)          step -= 100u;
    else if (level > AUDIO_RING_SIZE * 3u / 4u) step += 100u;

    pace_deadline += step;
    const uint64_t now = time_us_64();
    spec_timing.run_us = (uint32_t)(now - frame_started);

    if (now > pace_deadline) {
        spec_timing.late++;
        spec_timing.idle_us = 0;
        // More than a frame behind: stop owing it. Chasing a deadline the
        // machine cannot meet only makes every later frame late as well.
        if (now - pace_deadline > step) pace_deadline = now;
    } else {
        spec_timing.idle_us = (uint32_t)(pace_deadline - now);
        z80_park();
        while (time_us_64() < pace_deadline) tight_loop_contents();
    }
    if (spec_timing.idle_us < spec_timing.idle_min_us) spec_timing.idle_min_us = spec_timing.idle_us;
    frame_started = time_us_64();
}

/*
 * Put the engine at its floor and say how fast the machine could go.
 *
 * Kept under its old name because everything that changes the machine's
 * cost -- the turbo setting, the Beta Disk's fetch inspection -- already
 * calls it. It no longer tunes anything: it runs a second of unpaced
 * frames to measure what the engine can do, then resets the pacer. The
 * return is the rate the machine will actually run at, in frames per
 * second x1000: its own rate, or the measured one if it cannot reach it.
 */
/* What the engine manages unpaced at the current pace, in fps x1000. */
static uint32_t measure_capacity(void) {
    for (int f = 0; f < 10; f++) run_frame();           // settle into something typical
    const uint64_t t0 = time_us_64();
    for (int f = 0; f < 25; f++) run_frame();
    const uint64_t us = time_us_64() - t0;
    return (uint32_t)(25000000000ull / (us ? us : 1));
}

uint32_t spec_tune_clock(void) {
    pace_recompute();
    spec_timing.target_mfps = (uint32_t)(((uint64_t)spec_cpu_hz() * 1000u) / spec_frame_tstates());

    /*
     * Twice the floor, and down towards it only as far as the frame rate
     * needs.
     *
     * The first version of the pacer ran the engine AT the floor, on the
     * reasoning that the wait at the end of the frame made speed free.
     * But the floor is the last pace that passed one short program at
     * boot plus four -- a boundary, not a place to live -- and the bus
     * sampled at it was right most of the time and wrong some of it:
     * after a menu, a 128K came back with a black screen and the engine
     * answering reads of #FFFF that the CPU never made. Doubling it is
     * the margin the old frame-rate tuner had always left by accident.
     */
    const uint32_t floor = z80_get_pace_floor();
    const uint32_t want = spec_timing.target_mfps + spec_timing.target_mfps / 12u;   // +8 %
    uint32_t pace = floor * 2u;
    uint32_t cap;

    audio_set_enabled(false);
    for (;;) {
        z80_set_pace(pace);
        cap = measure_capacity();
        if (cap >= want || pace <= floor) break;
        const uint32_t step = (pace - floor + 1u) / 2u;
        pace -= step ? step : 1u;
    }
    audio_set_enabled(true);
    z80_park();

    spec_timing.capacity_mfps = cap;
    spec_timing.late = 0;
    spec_timing.idle_min_us = UINT32_MAX;

    // Run from the settings page, these frames were made on core 0 with
    // nobody consuming them; nobody is meant to hear them either.
    if (get_core_num() == 0) sound_flush();

    pace_deadline = time_us_64();
    frame_started = pace_deadline;
    return spec_timing.capacity_mfps < spec_timing.target_mfps ? spec_timing.capacity_mfps
                                                              : spec_timing.target_mfps;
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

// ---------------------------------------------------------------------------
// Contention and the floating bus
// ---------------------------------------------------------------------------
/*
 * The ULA's pattern: how long it holds an access at each of the eight
 * T-states of its fetch cycle. The +2A/+3 gate array's is the same
 * shape, started two T-states later. Both as Fuse documents them.
 */
static const uint8_t contend_48[8]  = { 6, 5, 4, 3, 2, 1, 0, 0 };
static const uint8_t contend_p3[8]  = { 1, 0, 7, 6, 5, 4, 3, 2 };

/*
 * Which banks the ULA shares with the CPU: bank 5 on every Sinclair
 * machine, the odd banks on a 128K and +2, banks 4-7 on a +2A/+3.
 */
static bool bank_contended(const unsigned b) {
    switch (spec_machine) {
        case MACHINE_48K:
        case MACHINE_16K:
        case MACHINE_TC2048:
        case MACHINE_GAMA:  return b == 5u;
        case MACHINE_128K:
        case MACHINE_PLUS2: return (b & 1u) != 0;
        case MACHINE_PLUS3:
        case MACHINE_PLUS2A: return b >= 4u && b <= 7u;
        default:            return false;
    }
}

/* After every paging change: which windows are contended now. */
static void contend_remask(void) {
    if (!z80_contend.on) return;
    uint8_t m = 0;
    for (unsigned w = 0; w < 4; w++)
        for (unsigned b = 0; b < SPEC_RAM_BANKS; b++)
            if (z80_rpage[w * 2u] == spec_ram[b] && bank_contended(b)) m |= (uint8_t)(1u << w);
    z80_contend.mask = m;
}

static void contend_setup(void) {
    z80_contend_t c = { 0 };
    const bool turbo = settings.turbo != 0;
    switch (spec_machine) {
        case MACHINE_48K:
        case MACHINE_16K:
        case MACHINE_TC2048:
        case MACHINE_GAMA:
            c = (z80_contend_t){ true, true, 0, 14335u, 224u, 0, contend_48 };
            break;
        case MACHINE_128K:
        case MACHINE_PLUS2:
            c = (z80_contend_t){ true, true, 0, 14361u, 228u, 0, contend_48 };
            break;
        case MACHINE_PLUS3:
        case MACHINE_PLUS2A:
            c = (z80_contend_t){ true, false, 0, 14361u, 228u, 0, contend_p3 };
            break;
        default:
            break;
    }
    c.span = c.line * 192u;
    // A machine in turbo has no ULA keeping time with it; nor does one
    // with the setting off.
    if (turbo || !settings.contention) c.on = false;
    z80_contend = c;
    contend_remask();
}

static uint8_t __not_in_flash_func(floating_bus)(const uint32_t t) {
    if (!z80_contend.on || spec_is_plus3_class(spec_machine)) return 0xFF;
    const uint32_t d = t - z80_contend.t0;
    if (d >= z80_contend.span) return 0xFF;
    const uint32_t y = d / z80_contend.line, x = d % z80_contend.line;
    if (x >= 128u) return 0xFF;
    const uint32_t ph = x & 7u;
    if (ph >= 4u) return 0xFF;
    const uint32_t col = (x >> 3) * 2u + (ph >> 1);
    const uint8_t *const scr = spec_ram[spec_screen_bank];
    if (ph & 1u) return scr[0x1800u + ((y >> 3) << 5) + col];
    return scr[(((y & 0xC0u) << 5) | ((y & 0x38u) << 2) | ((y & 0x07u) << 8)) + col];
}

// ---------------------------------------------------------------------------
// Reading the CPU's registers: the NMI dump
// ---------------------------------------------------------------------------
/*
 * There is no reading a Z84C00's registers from outside it, so the CPU is
 * asked to write them down.
 *
 * An NMI makes it push PC and fetch from 0x0066. At that fetch a stub is
 * put over the bottom 8 KB, the way the snapshot trampoline is, and the
 * stub PUSHes every register -- each push is a memory write, and the
 * write-watch hook sees the address and the byte on its way past. Then
 * it POPs them all back, which reads the same bytes, and leaves with
 * RETN, which puts the interrupt flip-flop back too. The first fetch
 * after that is the instruction the NMI interrupted; the real page goes
 * back under it there.
 *
 * What it costs the guest: thirty bytes of stack below SP, which are
 * put back afterwards byte for byte from what the watch saw them hold,
 * and R, which counts the stub's fetches and cannot be wound back.
 *
 * What it cannot see: the interrupt mode. Nothing in a Z80 reads IM back.
 * It is inferred from I -- the ROM leaves 0x3F there in IM 1, and a
 * program that has moved I somewhere else is almost always about to use
 * it for IM 2.
 */
static const uint8_t nmi_stub[] = {
    0xF5, 0xC5, 0xD5, 0xE5,             // PUSH AF/BC/DE/HL
    0xD9, 0x08,                         // EXX ; EX AF,AF'
    0xF5, 0xC5, 0xD5, 0xE5,             // PUSH AF'/BC'/DE'/HL'
    0xDD, 0xE5, 0xFD, 0xE5,             // PUSH IX ; PUSH IY
    0xED, 0x57, 0xF5,                   // LD A,I ; PUSH AF   (P/V = IFF2)
    0xED, 0x5F, 0xF5,                   // LD A,R ; PUSH AF
    0xF1, 0xF1,                         // POP AF ; POP AF    (dropped)
    0xFD, 0xE1, 0xDD, 0xE1,             // POP IY ; POP IX
    0xE1, 0xD1, 0xC1, 0xF1,             // POP HL'/DE'/BC'/AF'
    0x08, 0xD9,                         // EX AF,AF' ; EXX
    0xE1, 0xD1, 0xC1, 0xF1,             // POP HL/DE/BC/AF
    0xED, 0x45,                         // RETN
};
#define NMI_AT        0x0066u
#define NMI_RETN_AT   (NMI_AT + sizeof nmi_stub - 2u)
#define NMI_PUSHES    13u               // pairs the stub writes, PC's included
/* M1 cycles from the NMI's dummy fetch up to and including LD A,R. */
#define NMI_R_FETCHES 21u

typedef struct { uint16_t addr; uint8_t old, val; } nmi_write_t;
static nmi_write_t nmi_w[64];
static uint32_t    nmi_nw, nmi_k;
static volatile uint8_t nmi_ds;         // 0 idle, 1 armed, 2 in the stub, 3 leaving, 4 done
static uint8_t *nmi_saved_r[2], *nmi_saved_w[2];

static void __not_in_flash_func(nmi_wwatch)(const uint16_t a, const uint8_t v, const uint32_t t) {
    (void)t;
    if (nmi_nw < count_of(nmi_w)) nmi_w[nmi_nw++] = (nmi_write_t){ a, z80_peek(a), v };
}

static void __not_in_flash_func(nmi_opfetch)(const uint16_t addr, const uint32_t t) {
    (void)t;
    switch (nmi_ds) {
        case 1:
            if (addr != NMI_AT) return;
            nmi_k = nmi_nw;                 // the last two writes were PC
            for (int i = 0; i < 2; i++) { nmi_saved_r[i] = z80_rpage[i]; nmi_saved_w[i] = z80_wpage[i]; }
            z80_map_page8(0, spec_launch_page(), NULL);
            z80_map_page8(1, spec_launch_page() + 8192, NULL);
            nmi_ds = 2;
            return;
        case 2:
            if (addr == NMI_RETN_AT + 1u) nmi_ds = 3;
            return;
        case 3:
            for (int i = 0; i < 2; i++) { z80_rpage[i] = nmi_saved_r[i]; z80_wpage[i] = nmi_saved_w[i]; }
            nmi_ds = 4;
            return;
        default:
            return;
    }
}

/* Core 1 only: it clocks the CPU. */
static bool nmi_dump(z80_state_t *st) {
    uint8_t *const page = spec_launch_page();
    memset(page, 0xFF, 0x100);
    memcpy(page + NMI_AT, nmi_stub, sizeof nmi_stub);

    const z80_opfetch_fn saved_fetch = z80_opfetch_cb;
    const z80_wwatch_fn  saved_watch = z80_wwatch_cb;
    const uint8_t        saved_mask  = z80_wwatch_mask;

    nmi_nw = nmi_k = 0;
    nmi_ds = 1;
    z80_wwatch_cb   = nmi_wwatch;
    z80_wwatch_mask = 0xFF;
    z80_opfetch_cb  = nmi_opfetch;

    const uint32_t t_saved = z80_t;
    z80_pulse_nmi();
    for (int i = 0; i < 60 && nmi_ds != 4; i++) z80_run(50);
    z80_t = t_saved;

    z80_opfetch_cb  = saved_fetch;
    z80_wwatch_cb   = saved_watch;
    z80_wwatch_mask = saved_mask;

    if (nmi_ds != 4) {
        if (nmi_ds >= 2)
            for (int i = 0; i < 2; i++) { z80_rpage[i] = nmi_saved_r[i]; z80_wpage[i] = nmi_saved_w[i]; }
        nmi_ds = 0;
        return false;
    }
    nmi_ds = 0;
    if (nmi_k < 2 || nmi_nw < nmi_k - 2 + NMI_PUSHES * 2) return false;

    // Put the stack back as it was: oldest first wins, so in reverse.
    const nmi_write_t *const w = &nmi_w[nmi_k - 2];
    const uint32_t n = nmi_nw - (nmi_k - 2);
    for (uint32_t i = n; i--; ) z80_poke(w[i].addr, w[i].old);

    // Each push is high byte, then low.
    uint16_t v[NMI_PUSHES];
    for (uint32_t i = 0; i < NMI_PUSHES; i++)
        v[i] = (uint16_t)((w[2 * i].val << 8) | w[2 * i + 1].val);

    memset(st, 0, sizeof *st);
    st->pc  = v[0];
    st->sp  = (uint16_t)(w[0].addr + 1u);
    st->af  = v[1];  st->bc  = v[2];  st->de  = v[3];  st->hl  = v[4];
    st->af_ = v[5];  st->bc_ = v[6];  st->de_ = v[7];  st->hl_ = v[8];
    st->ix  = v[9];  st->iy  = v[10];
    st->i   = (uint8_t)(v[11] >> 8);
    st->iff = (v[11] & 0x04u) != 0;
    const uint8_t r = (uint8_t)(v[12] >> 8);
    st->r   = (uint8_t)((r & 0x80u) | ((r - NMI_R_FETCHES) & 0x7Fu));
    st->im  = (st->i == 0x3F || st->i < 0x40) ? 1 : 2;
    st->border   = spec_border;
    st->port7ffd = spec_port_7ffd;
    st->port1ffd = spec_port_1ffd;
    st->portdffd = spec_port_dffd;
    return true;
}

static volatile bool        dump_req, dump_ok;
static z80_state_t *volatile dump_into;

bool spec_capture_state(z80_state_t *st) {
    if (!paused) return false;
    dump_into = st;
    dump_ok   = false;
    dump_req  = true;
    const absolute_time_t deadline = make_timeout_time_ms(200);
    while (dump_req && absolute_time_diff_us(get_absolute_time(), deadline) > 0)
        tight_loop_contents();
    return !dump_req && dump_ok;
}

static void pause_point(void) {
    if (!pause_req) return;
    z80_park();
    paused = true;
    while (pause_req) {
        if (dump_req) {
            dump_ok  = nmi_dump(dump_into);
            z80_park();
            dump_req = false;
        }
        tight_loop_contents();
    }
    paused = false;
    // The time spent stopped is not a debt.
    pace_deadline = time_us_64();
    frame_started = pace_deadline;
}

void spec_set_machine(const machine_t m) {
    if (m >= MACHINE_COUNT) return;
    // A cartridge only fits a 48K; any other machine pulls it out.
    if (m != MACHINE_48K) if2_in = false;
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
    /*
     * The ATM-Turbo 3's upper 3 MB are the +3 disk's, the ALF cartridge's
     * and the tape capture's space on every other machine (arena.h): take
     * the disk out and forget the cartridge before the RAM lands on them.
     */
    const bool big = (m == MACHINE_ATM3);
    if (big && !arena_big_ram) {
        fdc_eject();
        alf_banks = 0;
    }
    arena_big_ram = big;
    spec_machine = m;
    pace_recompute();
    pulse_set_is_48k(spec_is_48k_class(m));
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
static void run_frame(void) {

    /* Swap the tape input's buffers before the machine runs into the
     * frame they describe. */
    if (unlikely(tapein_running())) tapein_frame_started(spec_frame_tstates());

    /*
     * The whole main loop of the machine, sliced.
     *
     * An emulator spends a frame stepping a CPU and counting contention.
     * This spends it letting a Z84C00 be a Z80, stopping once per sample
     * to take one. The only thing the ULA owes the CPU is an interrupt
     * every 70908 T-states, and the only thing that has to be right is
     * the count.
     *
     * T-state 0 is where ~INT goes down, as it is on the ULA, so every
     * position in the frame a handler is told is the position a real
     * machine's timing tables are written against.
     */
    const uint32_t num    = turbo_num();
    const uint32_t total  = spec_frame_tstates();

    // Tell core 0 how long this frame is and how many samples to cut it
    // into, before anything in it happens.
    snd_put(total, SD_BEGIN, 0);
    snd_put(next_frame_samples(), SD_SPF, 0);

    // The ULA holds ~INT low for about ten microseconds, which is 36
    // T-states at 3.5 MHz and proportionally more at any other speed --
    // it is a length of time, not a number of instructions.
    const uint32_t held = int_tstates() * num / 2;

    ula_frame_begin();
    z80_t = 0;
    // The ATM 2+ and 3 gate the frame interrupt with #77 D5.
    const bool int_on = !spec_is_atm(spec_machine) || atm_int_enabled();
    frame_int_on = int_on;
    if (unlikely(spec_machine == MACHINE_PROFI)) sm_update_int(); else z80_set_int(int_on);
    z80_run_until(held);
    frame_int_on = false;
    if (unlikely(spec_machine == MACHINE_PROFI)) sm_update_int(); else z80_set_int(false);

    /*
     * Sixteen stops a frame, each one a mark core 0 can render up to:
     * about 1.25 ms of sound at a time. Nothing else needs the machine
     * to stop -- every handler is told its own T-state.
     */
    for (uint32_t i = 1; i <= FRAME_STOPS; i++) {
        const uint32_t end = (uint32_t)((uint64_t)total * i / FRAME_STOPS);
        if (end > held) run_slice(end);
    }
    snd_put(total, SD_END, 0);
    ula_frame_end();
    if (unlikely(tapeplay_active)) tapeplay_frame_end(total, num);

    /*
     * The frame is over, so its T-states are spent: whatever the bus does
     * before the next one starts happens at the start of that one. The
     * pacer parks the CPU with z80_park(), which finishes the cycle in
     * progress -- and when that was an IN of #FE, the tape was asked
     * where it was at T 69888 of a frame it had already left, ran a whole
     * frame ahead, and stood still for the next one: one dead frame in
     * the middle of a header, and a real-time load failed every time.
     */
    z80_t = 0;

    if (giga_flips) { giga_quiet = 0; if (++giga_run >= 3) spec_giga_auto = true; }
    else            { giga_run = 0;   if (++giga_quiet >= 50) spec_giga_auto = false; }
    giga_flips = 0;

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
    pace_frame();
}
