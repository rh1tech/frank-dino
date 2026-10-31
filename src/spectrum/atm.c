/*
 * frank-dino — an RP2350B acting as the whole machine around a real Z80
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-dino
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Ported from pico-speccy's machines/Atm.cpp
 * (https://github.com/drewpo28/pico-speccy, GPL-3.0) by drew.po28, after
 * UnrealSpeccy and MAME. The register semantics, reset states and port
 * decodes are theirs; the comments carry the reasoning where it is not
 * obvious from the hardware.
 */

#include "atm.h"

#include <string.h>

#include "beta.h"
#include "settings.h"
#include "z80_bus.h"

IMPORT_BIN("roms/atm1.rom",       atm_rom1);
IMPORT_BIN("roms/atm2-106.rom",   atm_rom2_106);
IMPORT_BIN("roms/atm2-107.rom",   atm_rom2_107);
IMPORT_BIN("roms/atm2-xbios.rom", atm_rom2x);
IMPORT_BIN("roms/atm3.rom",       atm_rom3);
IMPORT_BIN("roms/atm-font.rom",   atm_font);
extern const uint8_t atm_rom1[], atm_rom2_106[], atm_rom2_107[], atm_rom2x[], atm_rom3[];
extern const uint8_t atm_font[2048];
const uint8_t *atm_font_ptr(void) { return atm_font; }

const uint8_t *atm_rom_image(const machine_t m, unsigned *pages) {
    switch (m) {
        case MACHINE_ATM1:  *pages = 4;  return atm_rom1;
        case MACHINE_ATM2:  *pages = 4;  return atm_rom2_106;
        case MACHINE_ATM2P: *pages = 4;  return atm_rom2_107;
        case MACHINE_ATM2X: *pages = 8;  return atm_rom2x;
        case MACHINE_ATM3:  *pages = 16; return atm_rom3;
        default:            *pages = 0;  return NULL;
    }
}

// ---------------------------------------------------------------------------
// The register file
// ---------------------------------------------------------------------------
static bool     atm1, atm3, int_gated;
static uint8_t  p7ffd;
static uint8_t  aFE, aFB, pFDFD;            // ATM-Turbo 1
static uint16_t a77;                        // ATM-Turbo 2+: the address of the last #77 write
static uint8_t  p77;                        // ...and its data
/* Unreal's pFFF7 form: bits 7-0 the page, bit 8 ROM, bit 9 "from the register". */
static uint16_t pF7[8];
static uint8_t  pBF;                        // ATM-Turbo 3
static uint8_t  pal[16], palHi[16];
static bool     beta;                       // the Beta-128's DOS signal
static uint8_t  s_ro;                       // which windows hold ROM
static uint8_t  border4;

volatile bool atm_pal_dirty;

/* The 2+'s /CPM; the ATM1's CP/M mode (#FE D7) remaps memory but does not
 * raise the DOS signal (pico-speccy's cpmOn()). */
static bool cpm_on(void) { return !atm1 && !(a77 & 0x200u); }

/* The TR-DOS ports and the DOS ROM signal are up in DOS or in CP/M. */
static bool dos_ports(void) { return beta || cpm_on(); }

static void dos_recalc(void) {
    beta_set_rom_paged(beta || cpm_on());
}

/* #xxF7's data, in the register form. Every bit is active low: D7 = 0
 * takes the whole page from here (1: the low bits from #7FFD), D6 = 0 is
 * ROM, D5-D0 the page inverted. pico-speccy's f7enc(): ... ^ 0x33F. */
static uint16_t f7enc(const uint8_t d) {
    return (uint16_t)((((d & 0xC0u) << 2) | (d & 0x3Fu)) ^ 0x33Fu);
}

static unsigned rom_pages(void) {
    unsigned n;
    atm_rom_image(spec_machine, &n);
    return n ? n : 4u;
}

static void map_rom(const unsigned w, const unsigned page) {
    unsigned n;
    const uint8_t *img = atm_rom_image(spec_machine, &n);
    uint8_t *p = spec_rom_page(img + (size_t)(page % (n ? n : 4u)) * SPEC_BANK_SIZE);
    z80_map_page(w, p, NULL);
    s_ro |= (uint8_t)(1u << w);
}

static void map_ram(const unsigned w, const unsigned page) {
    uint8_t *p = spec_ram_page(page & (SPEC_MAX_BANKS - 1u));
    z80_map_page(w, p, p);
}

void atm_remap(void) {
    s_ro = 0;
    const unsigned n = rom_pages();

    if (atm1) {
        // #FDFD D2-D0 extend the #C000 page to 1 MB (Unreal MM_ATM450, 1024K).
        const unsigned pg3 = (p7ffd & 7u) | ((pFDFD & 7u) << 3);
        if (!(aFE & 0x80u)) {
            map_ram(0, 0);                          // CP/M: RAM 0 at #0000,
            map_ram(1, 4);                          // and page 4 at #4000
        } else {
            if (p7ffd & 0x20u) aFB &= 0x7Fu;        // the 48 lock drops CPSYS
            if (beta && (pFDFD & 0x08u)) aFB |= 0x80u;
            unsigned r;
            if (aFB & 0x80u) r = 0;                 // SYSTEM ROM
            else if (beta)   r = 1;                 // TR-DOS
            else             r = (p7ffd & 0x10u) ? 3u : 2u;
            map_rom(0, r);
            map_ram(1, 5);
        }
        map_ram(2, 2);
        map_ram(3, pg3);
    } else if (!(a77 & 0x100u)) {
        // PEN = 0: the BIOS, the last ROM page, in every window.
        for (unsigned w = 0; w < 4; w++) map_rom(w, n - 1u);
    } else {
        const unsigned set = (p7ffd & 0x10u) ? 4u : 0u;
        const unsigned dos = dos_ports() ? 1u : 0u;
        for (unsigned w = 0; w < 4; w++) {
            const uint16_t v = pF7[set + w];
            const uint8_t page = (uint8_t)v;
            switch (v & 0x300u) {
                case 0x000: map_ram(w, (page & 0xF8u) | (p7ffd & 7u)); break;
                case 0x200: map_ram(w, page); break;
                case 0x100: map_rom(w, (page & 0xFEu) | dos); break;
                default:    map_rom(w, page); break;
            }
        }
    }
    spec_screen_bank = (p7ffd & 0x08u) ? 7 : 5;
    spec_port_7ffd   = p7ffd;
}

static void load_spec_palette(void) {
    for (int i = 0; i < 16; i++) {
        const bool b = i & 1, r = i & 2, g = i & 4, br = i & 8;
        uint8_t v;
        if (atm1) v = (uint8_t)((b ? 0x01 : 0) | (r ? 0x02 : 0) | (g ? 0x04 : 0) |
                                (br ? ((b ? 0x08 : 0) | (r ? 0x10 : 0) | (g ? 0x20 : 0)) : 0));
        else      v = (uint8_t)((b ? 0x01 : 0) | (r ? 0x02 : 0) | (g ? 0x10 : 0) |
                                (br ? ((b ? 0x20 : 0) | (r ? 0x40 : 0) | (g ? 0x80 : 0)) : 0));
        pal[i] = (uint8_t)~v;
        palHi[i] = 0xFF;
    }
    atm_pal_dirty = true;
}

void atm_reset(void) {
    const machine_t m = spec_machine;
    atm1 = m == MACHINE_ATM1;
    atm3 = m == MACHINE_ATM3;
    // The frame INT is gated by #77 D5 on the 2+ and 3. BIOS 1.06.02 never
    // sets it and waits on the INT regardless, so the plain 2 is ungated.
    int_gated = (m == MACHINE_ATM2P || m == MACHINE_ATM2X || m == MACHINE_ATM3);
    p7ffd = 0;
    beta = false;
    pBF = 0;
    border4 = 0;
    if (atm1) {
        aFE = 0x80;                 // no CP/M, EGA
        aFB = 0x80;                 // CPSYS: the SYSTEM ROM
        pFDFD = 0;
        a77 = 0x0200;
        p77 = 0x20;
    } else {
        // #77 written with address 0 and data 0: PEN = 0 (the BIOS in all
        // four windows), /CPM = 0 (DOS signal up), EGA, INT off.
        a77 = 0;
        p77 = 0;
        aFE = 0xFE;
        aFB = 0;
        pFDFD = 0;
    }
    memset(pF7, 0, sizeof pF7);
    load_spec_palette();
    dos_recalc();
    atm_remap();
}

bool atm_int_enabled(void) { return !int_gated || (p77 & 0x20u); }

atm_vmode_t atm_video_mode(void) {
    if (atm1) {
        switch ((aFE >> 5) & 3u) {
            case 0:  return ATM_VM_EGA;
            case 1:  return ATM_VM_HIRES;
            default: return ATM_VM_ZX;
        }
    }
    switch (p77 & 7u) {
        case 0:  return ATM_VM_EGA;
        case 2:  return ATM_VM_HIRES;
        case 6:  return ATM_VM_TEXT;
        default: return ATM_VM_ZX;      // 3 is ZX; the undefined codes render as ZX
    }
}

uint8_t atm_border_index(void) { return border4; }

// ---------------------------------------------------------------------------
// Palette
// ---------------------------------------------------------------------------
/*
 * Data bits inverted: grbG--RB on the 2+ and 3, --grbGRB on the 1. With
 * the ATM-Turbo 3's #BF D5 the address byte of the same write carries
 * the two low bits a channel -- 4096 colours, of which this board shows
 * the top two bits, which is all its ladder has.
 */
void atm_palette_rgb2(uint8_t rgb[16][3]) {
    for (int i = 0; i < 16; i++) {
        const uint8_t v = (uint8_t)~pal[i];
        unsigned r, g, b;
        if (atm1) {
            r = (((v >> 1) & 1u) << 1) | ((v >> 4) & 1u);
            g = (((v >> 2) & 1u) << 1) | ((v >> 5) & 1u);
            b = (((v >> 0) & 1u) << 1) | ((v >> 3) & 1u);
        } else {
            r = (((v >> 1) & 1u) << 1) | ((v >> 6) & 1u);
            g = (((v >> 4) & 1u) << 1) | ((v >> 7) & 1u);
            b = (((v >> 0) & 1u) << 1) | ((v >> 5) & 1u);
        }
        rgb[i][0] = (uint8_t)r; rgb[i][1] = (uint8_t)g; rgb[i][2] = (uint8_t)b;
    }
}

static void pal_write(const uint8_t data, const uint8_t hi) {
    const unsigned i = border4 & 15u;
    if (pal[i] != data || palHi[i] != hi) {
        pal[i] = data;
        palHi[i] = hi;
        atm_pal_dirty = true;
    }
}

// ---------------------------------------------------------------------------
// Ports
// ---------------------------------------------------------------------------
static void write7ffd(const uint8_t data) {
    if (p7ffd & 0x20u) return;      // the 48 lock
    p7ffd = data;
    atm_remap();
}

void atm_fe_write(const uint16_t port, const uint8_t value) {
    // The border's fourth bit is A3 of the write, inverted.
    border4 = (uint8_t)((value & 7u) | ((port & 0x08u) ? 0u : 8u));
    if (!atm1) return;
    const uint8_t a = (uint8_t)port;
    const uint8_t old = aFE;
    aFE = a;
    if ((old ^ a) & 0x80u) { dos_recalc(); atm_remap(); }
}

bool atm_port_write(const uint16_t port, const uint8_t data) {
    const uint8_t lo = (uint8_t)port;

    if (atm1) {
        if (port & 2u) return false;
        if ((port & 0x8202u) == 0x0000u) { pal_write(data, 0xFF); return true; }   // #7DFD
        if ((port & 0x8202u) == 0x0200u) { write7ffd(data); return true; }         // #7FFD
        if ((port & 0x8202u) == 0x8000u) { pFDFD = data; atm_remap(); return true; } // #FDFD
        return false;
    }

    // ATM-Turbo 3 #BF: D0 shadow ports, D1 #xxE7 as #x7F7, D5 4096 colours.
    if (atm3 && lo == 0xBFu) { pBF = data & 0x23u; atm_pal_dirty = true; return true; }

    if (atm3 ? (lo == 0xFDu && !(port & 0x8000u)) : !(port & 0x8002u)) {
        write7ffd(data);
        return true;
    }

    // Everything below is a shadow port: the palette too, which outside
    // DOS would take every OUT (#FF) a game makes for a colour.
    if (!dos_ports() && !(atm3 && (pBF & 1u))) return false;

    const unsigned sel = ((p7ffd & 0x10u) >> 2) | (port >> 14);
    if (atm3 && lo == 0xE7u) {
        pF7[sel] = (pBF & 2u) ? (uint16_t)((pF7[sel] & ~0x1FFu) | (uint8_t)(data ^ 0xFFu))
                              : f7enc(data);
        atm_remap();
        return true;
    }
    // %nXnnnnXX 0nn101n1 / 1nn101n1: A6, A5, A1 not decoded.
    if ((lo & 0x9Du) == 0x15u) {
        a77 = port;
        p77 = data;
        dos_recalc();
        atm_remap();
        return true;
    }
    if ((lo & 0x9Du) == 0x95u) {
        if (atm3 && !(port & 0x0800u))
            pF7[sel] = (uint16_t)((pF7[sel] & ~0x1FFu) | (uint8_t)(data ^ 0xFFu));
        else
            pF7[sel] = f7enc(data);
        atm_remap();
        return true;
    }
    if ((lo & 0x9Fu) == 0x9Fu && !(a77 & 0x4000u)) pal_write(data, (uint8_t)(port >> 8));
    return false;
}

bool atm_port_read(const uint16_t port, uint8_t *v) {
    if (atm1) {
        // IN #FB latches the low address byte: A7 is CPSYS.
        if ((port & 0x05u) == 0x01u) {
            const uint8_t old = aFB;
            aFB = (uint8_t)port;
            if ((old ^ aFB) & 0x80u) atm_remap();
            *v = 0x7F;
            return true;
        }
        if ((port & 0x05u) == 0x00u) { *v = 0xFF; return true; }
        return false;
    }
    if (atm3 && (uint8_t)port == 0xBFu) { *v = pBF; return true; }
    if ((port & 0x07u) == 0x03u) { *v = 0x7F; return true; }          // printer: not busy
    if (dos_ports() && (port & 0x1Fu) == 0x1Fu) return false;         // the WD1793's
    if ((port & 0x8202u) == 0x0200u) { *v = 0x3F | 0x40; return true; }
    return false;
}

// ---------------------------------------------------------------------------
// TR-DOS, by fetch
// ---------------------------------------------------------------------------
/*
 * In at #3Dxx with the 48 ROM selected and ROM really in window 0 --
 * "for Scorp, ATM-1/2 and KAY, TR-DOS not started on executing RAM
 * 3Dxx" (Unreal). Out on the first fetch from a window that holds RAM.
 */
void atm_opfetch(const uint16_t addr) {
    if (!settings.betadisk) return;
    if (!beta) {
        const bool rom_ok = (p7ffd & 0x10u) || (atm1 && (aFB & 0x80u));
        if ((addr >> 8) == 0x3Du && rom_ok && (s_ro & 1u)) {
            beta = true;
            dos_recalc();
            atm_remap();
        }
    } else if (!(s_ro & (1u << (addr >> 14)))) {
        beta = false;
        dos_recalc();
        atm_remap();
    }
}
