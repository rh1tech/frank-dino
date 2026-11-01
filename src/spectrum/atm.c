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
#include "ide.h"
#include "rtc.h"
#include "settings.h"
#include "z80_bus.h"

IMPORT_BIN("roms/atm1.rom",       atm_rom1);
IMPORT_BIN("roms/atm2-106.rom",   atm_rom2_106);
IMPORT_BIN("roms/atm2-107.rom",   atm_rom2_107);
IMPORT_BIN("roms/atm2-xbios.rom", atm_rom2x);
IMPORT_BIN("roms/atm3.rom",       atm_rom3);
IMPORT_BIN("roms/zxevo_fe.rom",   evo_rom);
IMPORT_BIN("roms/atm-font.rom",   atm_font);
extern const uint8_t atm_rom1[], atm_rom2_106[], atm_rom2_107[], atm_rom2x[], atm_rom3[], evo_rom[];
extern const uint8_t atm_font[2048];
/*
 * The Evo's character generator is a RAM the FPGA fills from the same
 * font at power-up, and #BF D2 lets every memory write land in it too.
 */
static uint8_t evo_font[2048];
static bool    evo_font_loaded;
const uint8_t *atm_font_ptr(void) { return spec_machine == MACHINE_EVO ? evo_font : atm_font; }

const uint8_t *atm_rom_image(const machine_t m, unsigned *pages) {
    switch (m) {
        case MACHINE_ATM1:  *pages = 4;  return atm_rom1;
        case MACHINE_ATM2:  *pages = 4;  return atm_rom2_106;
        case MACHINE_ATM2P: *pages = 4;  return atm_rom2_107;
        case MACHINE_ATM2X: *pages = 8;  return atm_rom2x;
        case MACHINE_ATM3:  *pages = 16; return atm_rom3;
        case MACHINE_EVO:   *pages = 32; return evo_rom;
        default:            *pages = 0;  return NULL;
    }
}

// ---------------------------------------------------------------------------
// The register file
// ---------------------------------------------------------------------------
static bool     atm1, atm3, evo, int_gated;
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

/*
 * The ZX Evolution's BaseConf on top of the 2+'s manager (base_trdemu's
 * zports.v, atm_pager.v, zdos.v, znmi.v; see the Evo section below).
 */
static uint8_t  pEFF7;                      // D2 128K paging (else Pentagon 1024), D3 RAM 0 at #0000, D7 Gluk
static bool     in_nmi;                     // RAM page #FF in window 0, from the NMI to OUT (#BE)
static uint8_t  nmi_clr;                    // M1s left before in_nmi drops after OUT (#BE)
static bool     nmi_armed;                  // the NMI is on its way to #0066
static volatile bool nmi_req;               // #BF D3 went 1 -> 0: an NMI at the next frame
static bool     in_trdemu;                  // RAM page #FE in window 0: ERS's drive emulator
static uint8_t  fdd_mask, vg_sys;
static uint8_t  sav[4] = { 0xFF, 0xFF, 0xFF, 0xFF };   // #2F/#4F/#6F/#8F; kept over a reset

static bool mode1m(void) { return !(pEFF7 & 0x04u); }
static void evo_font_watch(bool on);
static bool evo_port_write(uint16_t port, uint8_t data);
static bool evo_port_read(uint16_t port, uint8_t *v);
static void evo_opfetch(uint16_t addr);

/* The 2+'s /CPM; the ATM1's CP/M mode (#FE D7) remaps memory but does not
 * raise the DOS signal (pico-speccy's cpmOn()). */
static bool cpm_on(void) { return !atm1 && !(a77 & 0x200u); }

/* The TR-DOS ports and the DOS ROM signal are up in DOS or in CP/M. */
static bool dos_ports(void) { return beta || cpm_on() || (evo && (pBF & 1u)); }

static void dos_recalc(void) {
    // The Evo's DOS is a latch: set while /CPM = 0, and it stays set
    // after /CPM goes back up until code runs from RAM (zdos.v).
    if (evo && cpm_on()) beta = true;
    beta_set_rom_paged(dos_ports());
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
    uint8_t *p = spec_ram_page(page & (spec_ram_banks() - 1u));
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
        const unsigned dos = (beta || cpm_on()) ? 1u : 0u;
        // The Evo's Pentagon-1024 mode: a RAM window in "#7FFD mode" takes
        // six page bits from #7FFD (D7-D5, D2-D0) under the register's two.
        const bool evo1m = evo && mode1m();
        const unsigned pent1m = ((p7ffd >> 2) & 0x38u) | (p7ffd & 7u);
        for (unsigned w = 0; w < 4; w++) {
            // The Evo's window 0, ahead of the manager: RAM #FF in the NMI,
            // #FE while ERS emulates a drive, RAM 0 with #EFF7 D3.
            if (evo && w == 0 && (in_nmi || in_trdemu || (pEFF7 & 0x08u))) {
                map_ram(0, (in_nmi || in_trdemu) ? (0xFEu | (in_nmi ? 1u : 0u)) : 0u);
                continue;
            }
            const uint16_t v = pF7[set + w];
            const uint8_t page = (uint8_t)v;
            switch (v & 0x300u) {
                case 0x000: map_ram(w, evo1m ? (page & 0xC0u) | pent1m : (page & 0xF8u) | (p7ffd & 7u)); break;
                case 0x200: map_ram(w, page); break;
                case 0x100: map_rom(w, (page & 0xFEu) | dos); break;
                default:    map_rom(w, page); break;
            }
        }
    }
    spec_screen_bank = (p7ffd & 0x08u) ? 7 : 5;
    spec_port_7ffd   = p7ffd;
    spec_contend_remask();
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
    evo  = m == MACHINE_EVO;
    // The frame INT is gated by #77 D5 on the 2+ and 3. BIOS 1.06.02 never
    // sets it and waits on the INT regardless, so the plain 2 is ungated.
    int_gated = (m == MACHINE_ATM2P || m == MACHINE_ATM2X || m == MACHINE_ATM3);
    p7ffd = 0;
    beta = false;
    pBF = 0;
    border4 = 0;
    if (evo) {
        // zports.v's reset: ZX mode, no manager (the last ROM page in
        // every window), /CPM = 0 so DOS is up, /PEN2 = 0.
        a77 = 0x4000;
        p77 = 3;
        aFE = 0xFE;
        aFB = 0;
        pFDFD = 0;
        pEFF7 = 0;
        in_nmi = in_trdemu = nmi_armed = false;
        nmi_req = false;
        nmi_clr = 0;
        fdd_mask = vg_sys = 0;
        // The font RAM is loaded when the FPGA is, not by a reset.
        if (!evo_font_loaded) { memcpy(evo_font, atm_font, sizeof evo_font); evo_font_loaded = true; }
        evo_font_watch(false);
    } else if (atm1) {
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
        case 7:  return evo ? ATM_VM_TEXT1 : ATM_VM_ZX;
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
    // The 48 lock; in the Evo's Pentagon-1024 mode D5 is a page bit.
    if ((p7ffd & 0x20u) && !(evo && mode1m())) return;
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

// ---------------------------------------------------------------------------
// ZX Evolution BaseConf
// ---------------------------------------------------------------------------
/*
 * After pico-speccy's EvoBase.cpp and its tools/evo_sim.c, which read the
 * FPGA out of svn.zxevo.ru's pentevo/fpga/base_trdemu -- the build the
 * shipped zxevo_fe.rom is made for. The ports, port for port:
 *
 *   #xFF7 / #x7F7  the manager, in shadow (DOS, /CPM or #BF D0) only
 *   #EFF7          outside shadow: D2 128K paging (else Pentagon 1024),
 *                  D3 RAM 0 at #0000, D4 turbo off, D7 the Gluk ports
 *   #DFF7/#BFF7    the Gluk clock outside shadow; #DEF7/#BEF7 inside it
 *   #BF            D0 shadow ports, D2 font RAM write, D3 1->0 an NMI,
 *                  D5 the 4096-colour palette
 *   #BE            OUT: leave the NMI or the drive emulator
 *   #BD            IN: the configuration read back by A12-A8;
 *                  OUT #13BD: the drives ERS's emulator serves
 *
 * The clock is the real Z80's, so #77 D3 and #EFF7 D4 -- 7 and 14 MHz
 * -- are latched and ignored, as on the other ATMs.
 */

/* Every memory write into the character generator too, at A10-A0. */
static z80_wwatch_fn font_prev_cb;
static uint8_t       font_prev_mask;
static bool          font_on;

static void __not_in_flash_func(evo_font_wwatch)(const uint16_t addr, const uint8_t v, const uint32_t t) {
    evo_font[addr & 0x7FFu] = v;
    if ((font_prev_mask >> (addr >> 13)) & 1u) font_prev_cb(addr, v, t);
}

static void evo_font_watch(const bool on) {
    if (on == font_on) return;
    font_on = on;
    if (on) {
        font_prev_cb   = z80_wwatch_cb;
        font_prev_mask = z80_wwatch_mask;
        z80_wwatch_cb   = evo_font_wwatch;
        z80_wwatch_mask = 0xFF;
    } else {
        z80_wwatch_mask = 0;
        z80_wwatch_cb   = font_prev_cb;
        z80_wwatch_mask = font_prev_mask;
    }
}

/* The NMI ERS asks for with #BF D3. Core 1, at the frame's start. */
bool atm_take_nmi(void) {
    if (!nmi_req) return false;
    nmi_req = false;
    if (in_nmi) return false;
    nmi_armed = true;
    return true;
}

/* The Magic button: the NMI the board's own key raises. */
void atm_nmi_button(void) { if (evo) nmi_req = true; }

/*
 * Where the CPU fetches decides three things.
 *
 * DOS (zdos.v): any M1 from a window whose register says RAM drops it
 * while /CPM = 1; an M1 at #3Dxx of a window whose set-1 register is ROM
 * "by #7FFD", with #7FFD D4 set, raises it; /CPM = 0 holds it up.
 *
 * The NMI (znmi.v): RAM page #FF comes into window 0 for the fetch at
 * #0066, and leaves two M1s after OUT (#BE) -- so the RETN after the OUT
 * still comes from it.
 */
static void __not_in_flash_func(evo_opfetch)(const uint16_t addr) {
    bool again = false;
    if (nmi_clr) {
        if (nmi_clr == 1) { nmi_clr = 0; in_nmi = false; again = true; }
        else nmi_clr--;
    }
    if (nmi_armed && addr == 0x0066u) { nmi_armed = false; in_nmi = true; again = true; }

    const bool was = beta;
    const unsigned w = addr >> 14;
    const unsigned set = (p7ffd & 0x10u) ? 4u : 0u;
    const bool reg_ram = (a77 & 0x100u) && !(pF7[set + w] & 0x100u);
    if (reg_ram) {
        if (!cpm_on()) beta = false;
    } else if (((addr >> 8) & 0x3Fu) == 0x3Du && (p7ffd & 0x10u) && (pF7[4 + w] & 0x300u) == 0x100u) {
        beta = true;
    }
    if (cpm_on()) beta = true;
    if (beta != was) { dos_recalc(); again = true; }
    if (again) atm_remap();
}

/*
 * ERS's drive emulator (zdos.v trdemu): an access to a WD1793 port in
 * shadow, for a drive #13BD masks, with DOS up, ROM in window 0 and
 * /PEN2 = 1, puts RAM page #FE in window 0 and the WD1793 never sees it.
 */
static bool evo_trdemu(const uint8_t lo) {
    if (lo != 0x1Fu && lo != 0x3Fu && lo != 0x5Fu && lo != 0x7Fu && lo != 0xFFu) return false;
    if (!(fdd_mask & (1u << (vg_sys & 3u)))) return false;
    if (!beta || !(a77 & 0x4000u) || !(s_ro & 1u) || in_trdemu || in_nmi) return false;
    in_trdemu = true;
    atm_remap();
    return true;
}

static void evo_write77(const uint16_t port, const uint8_t data) {
    a77 = port;
    p77 = data;
    dos_recalc();
    atm_remap();
}

/* The NEMO IDE's decode: swallowed when no disk answers it, so it does
 * not reach the ULA as a border colour. */
static bool nemo_decode(const uint8_t lo) {
    return ((lo & 7u) == 0 && (((lo >> 3) ^ (lo >> 4)) & 1u)) || lo == 0x11u;
}

static bool evo_port_write(const uint16_t port, const uint8_t data) {
    const uint8_t lo = (uint8_t)port;
    const bool sh = dos_ports();
    switch (lo) {
        case 0xBF: {
            if ((pBF & 0x08u) && !(data & 0x08u)) nmi_req = true;
            const uint8_t old = pBF;
            pBF = data & 0x3Fu;
            if ((old ^ pBF) & 0x20u) atm_pal_dirty = true;
            if ((old ^ pBF) & 0x01u) dos_recalc();
            evo_font_watch((pBF & 0x04u) != 0);
            return true;
        }
        case 0xBE:
            // in_trdemu drops at once; in_nmi two M1s later (znmi.v clr_count).
            if (in_nmi) nmi_clr = 3;
            else if (in_trdemu) { in_trdemu = false; atm_remap(); }
            return true;
        case 0xBD:
            if (((port >> 8) & 0x1Fu) == 0x13u) fdd_mask = data & 0x0Fu;
            return true;
        case 0xFD: case 0xFC:
            if (!(port & 0x8000u)) { write7ffd(data); return lo == 0xFDu; }
            return false;
        case 0xF7: {
            const bool a8 = (port & 0x100u) != 0;
            if (sh && a8) {
                const unsigned sel = ((p7ffd & 0x10u) >> 2) | (port >> 14);
                switch ((port >> 10) & 3u) {
                    case 3: pF7[sel] = f7enc(data); break;
                    case 1: pF7[sel] = (uint16_t)((pF7[sel] & ~0x1FFu) | (uint8_t)(data ^ 0xFFu)); break;
                    default: return true;                   // #xBF7, write protect: not modelled
                }
                atm_remap();
                return true;
            }
            if (sh == a8) return true;
            if (!sh && !(port & 0x1000u)) {                 // #EFF7
                const uint8_t old = pEFF7;
                pEFF7 = data;
                if ((old ^ data) & 0x0Cu) atm_remap();
                return true;
            }
            if (!(sh || (pEFF7 & 0x80u))) return true;      // the Gluk ports are off
            if (!(port & 0x2000u))      rtc_select(data);   // #DFF7 / #DEF7
            else if (!(port & 0x4000u)) rtc_write(data);    // #BFF7 / #BEF7
            return true;
        }
        default: break;
    }
    if (nemo_decode(lo) && !ide_mounted()) return true;
    if (!sh) return false;
    if (lo == 0x2Fu || lo == 0x4Fu || lo == 0x6Fu || lo == 0x8Fu) { sav[(lo >> 5) - 1u] = data; return true; }
    if (lo == 0x77u) { evo_write77(port, data); return true; }
    if (lo == 0xFFu) {
        vg_sys = data;
        if (!(a77 & 0x4000u)) pal_write(data, (uint8_t)(port >> 8));
    }
    if (evo_trdemu(lo)) return lo != 0xFFu;
    return false;
}

/* #BD: the configuration, read back by A12-A8. Pages come back inverted,
 * in the #xFF7 data format (top.v feeds zports ~pages). */
static uint8_t evo_bd(const unsigned idx) {
    if (idx < 8u) return (uint8_t)~pF7[idx];
    uint8_t v = 0;
    switch (idx) {
        case 0x08: for (unsigned i = 0; i < 8u; i++) if (!(pF7[i] & 0x100u)) v |= (uint8_t)(1u << i); return v;
        case 0x09: for (unsigned i = 0; i < 8u; i++) if (!(pF7[i] & 0x200u)) v |= (uint8_t)(1u << i); return v;
        case 0x0A: return p7ffd;
        case 0x0B: return pEFF7;
        case 0x0C: return (uint8_t)(((a77 & 0x4000u) ? 0x80u : 0u) | ((a77 & 0x200u) ? 0x40u : 0u) |
                                    ((a77 & 0x100u) ? 0x20u : 0u) | (beta ? 0x10u : 0u) | (p77 & 0x0Fu));
        case 0x0D: return pal[border4 & 15u];
        case 0x0F: return (uint8_t)(0xF0u | (border4 & 15u));
        case 0x13: return fdd_mask;     // ERS's _VERSION check wants it back exactly
        default:   return 0xFF;
    }
}

static bool evo_port_read(const uint16_t port, uint8_t *v) {
    const uint8_t lo = (uint8_t)port;
    const bool sh = dos_ports();
    switch (lo) {
        case 0xBF: *v = pBF; return true;
        // #BE reads the same: ATM CP/M's BIOS still reads it there.
        case 0xBD: case 0xBE: *v = evo_bd((port >> 8) & 0x1Fu); return true;
        case 0xF7:
            *v = (!(port & 0x4000u) && (((port & 0x100u) != 0) != sh) && (sh || (pEFF7 & 0x80u)))
                 ? rtc_read() : 0xFF;
            return true;
        default: break;
    }
    if (!sh) return false;
    if (lo == 0x2Fu || lo == 0x4Fu || lo == 0x6Fu || lo == 0x8Fu) { *v = sav[(lo >> 5) - 1u]; return true; }
    if (evo_trdemu(lo)) { *v = 0xFF; return true; }
    return false;
}

/* Which even ports reach the ULA: #FE, #F6 and #FC only. */
bool atm_evo_ula(const uint16_t port) {
    const uint8_t lo = (uint8_t)port;
    return lo == 0xFEu || lo == 0xF6u || lo == 0xFCu;
}

bool atm_port_write(const uint16_t port, const uint8_t data) {
    const uint8_t lo = (uint8_t)port;
    if (evo) return evo_port_write(port, data);

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
    if (evo) return evo_port_read(port, v);
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
    if (evo) { evo_opfetch(addr); return; }
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
