/*
 * frank-dino — an RP2350B acting as the whole machine around a real Z80
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-dino
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Ported from pico-speccy (https://github.com/drewpo28/pico-speccy, GPL-3.0):
 * Ports.cpp smucPortRead/smucPortWrite and devices/storage/Nvram24.cpp, which
 * is ZXMAK2's NvramChip.cs, which is UnrealSpeccy's model.
 */

/*
 * SMUC, the Scorpion's multi-unit controller.
 *
 * One card with three chips on it: an ATA port, a 24LC16 I2C EEPROM, and an
 * MC146818 clock -- the last two are where ProfROM keeps its settings and
 * its disk's partition table. Decoded on A12=A11=A7=A5=A1=1, A0=0, A6=0:
 *
 *   #5FBA  R   version   (#FF would mean "no SMUC" to the ROM's detect)
 *   #5FBE  R   revision
 *   #7FBA  RW  virtual-FDD latch
 *   #7FBE  R   the ISA 8259 -- "not installed"
 *   #DFBA  RW  the clock; SYS D7 picks register select (0) or data (1)
 *   #FFBA  RW  SYS: the EEPROM's I2C lines, the drive reset, the D7 mode bit
 *   #F8BE..#FFBE  RW  ATA registers 0..7 (A8-A10)
 *   #D8BE..#DFBE  RW  the 16-bit data register's high byte
 *
 * Only while the TR-DOS ROM is paged or #1FFD D1 (SYSEN) is set -- that is
 * the card's own gate.
 *
 * Their two deliberate deviations are kept: SYS D0 resets the drive on the
 * 0->1 edge rather than the level (ProfROM holds D0 set), and the INTRQ bit
 * reads 0 (every driver polls BSY).
 */
#include "smuc.h"

#include <string.h>

#include "beta.h"
#include "ide.h"
#include "rtc.h"
#include "settings.h"
#include "spectrum.h"

#ifndef SMUC_HOST_TEST
#include "ff.h"
#include "pico/time.h"
#endif

// ---------------------------------------------------------------------------
// The 24LC16
// ---------------------------------------------------------------------------
/*
 * Bit-banged by the guest through the SYS byte -- write: D6 SCL, D5 WP,
 * D4 SDA; read: D6 SDA. A state machine, not a memory window: START and
 * STOP are SDA edges while SCL is high, everything else is clocked on SCL.
 * The device address is #Ax with the page in D3-D1, which is how a 24LC16
 * reaches 2 KB through an 8-bit register; a page write wraps in 16 bytes.
 */
#define NV_SIZE 2048u
#define SCL     0x40u
#define SDA     0x10u
#define SDA_1   0xFFu           // the chip releases the line, or drives it high
#define SDA_0   0xBFu           // the chip pulls SDA (D6) low

enum { NV_IDLE, NV_CMD, NV_ADDR, NV_DATA, NV_SEND, NV_RDACK };

static uint8_t  nv[NV_SIZE];
static uint16_t nv_addr;
static uint8_t  nv_in, nv_outb, nv_bitsin, nv_bitsout, nv_state, nv_prev;
static uint8_t  nv_out = SDA_1, nv_z = 1;
static volatile bool     nv_dirty;
#ifndef SMUC_HOST_TEST
static volatile uint64_t nv_dirty_at;
#endif

static void nv_reset(void) {
    nv_state = NV_IDLE;
    nv_bitsin = nv_bitsout = 0;
    nv_addr = 0;
    nv_prev = 0;
    nv_out = SDA_1;
    nv_z = 1;
}

uint8_t *nvram24_mem(void) { return nv; }
uint8_t  nvram24_read(void) { return nv_out; }

void nvram24_write(const uint8_t v) {
    if ((v ^ nv_prev) & SCL) {                      // a clock edge
        if (v & SCL) {                              // rising: the chip samples SDA
            if (nv_state == NV_RDACK) {
                if (v & SDA) { nv_state = NV_IDLE; nv_z = 1; }      // NAK: stop
                else {                                              // ACK: next byte
                    nv_state = NV_SEND;
                    nv_outb = nv[nv_addr];
                    nv_addr = (uint16_t)((nv_addr + 1u) & (NV_SIZE - 1u));
                    nv_bitsout = 0;
                }
                goto done;
            }
            if ((nv_state == NV_CMD || nv_state == NV_ADDR || nv_state == NV_DATA) && nv_z) {
                nv_in = (uint8_t)(2u * nv_in + ((v >> 4) & 1u));
                nv_bitsin++;
            }
        } else {                                    // falling: the chip drives SDA
            if (nv_bitsin == 8) {
                nv_bitsin = 0;
                if (nv_state == NV_CMD) {
                    if ((nv_in & 0xF0u) != 0xA0u) { nv_state = NV_IDLE; nv_z = 1; goto done; }
                    nv_addr = (uint16_t)((nv_addr & 0xFFu) + ((nv_in << 7) & 0x700u));
                    if (nv_in & 1u) {
                        nv_outb = nv[nv_addr];
                        nv_addr = (uint16_t)((nv_addr + 1u) & (NV_SIZE - 1u));
                        nv_bitsout = 0;
                        nv_state = NV_SEND;
                    } else {
                        nv_state = NV_ADDR;
                    }
                } else if (nv_state == NV_ADDR) {
                    nv_addr = (uint16_t)((nv_addr & 0x700u) + nv_in);
                    nv_state = NV_DATA;
                } else if (nv_state == NV_DATA) {
                    if (nv[nv_addr] != nv_in) {
                        nv[nv_addr] = nv_in;
                        nv_dirty = true;
#ifndef SMUC_HOST_TEST
                        nv_dirty_at = time_us_64();
#endif
                    }
                    nv_addr = (uint16_t)((nv_addr & 0x7F0u) + ((nv_addr + 1u) & 0x0Fu));
                }
                nv_out = SDA_0;                     // the EEPROM always ACKs
                nv_z = 0;
                goto done;
            }
            if (nv_state == NV_SEND) {
                if (nv_bitsout == 8) { nv_state = NV_RDACK; nv_z = 1; goto done; }
                nv_out = (nv_outb & 0x80u) ? SDA_1 : SDA_0;
                nv_outb = (uint8_t)(nv_outb << 1);
                nv_bitsout++;
                nv_z = 0;
                goto done;
            }
            nv_z = 1;
        }
        goto done;
    }
    if ((v & SCL) && ((v ^ nv_prev) & SDA)) {      // START or STOP
        if (v & SDA) nv_state = NV_IDLE;
        else { nv_state = NV_CMD; nv_bitsin = 0; }
        nv_z = 1;
    }
done:
    if (nv_z) nv_out = (v & SDA) ? SDA_1 : SDA_0;  // the line follows the host
    nv_prev = v;
}

// ---------------------------------------------------------------------------
// The card
// ---------------------------------------------------------------------------
static uint8_t sys, fdd;

bool smuc_fitted(void) {
    const machine_t m = spec_machine;
    // ProfROM's machines were sold with it, and their ROMs keep their
    // settings on it; a plain Scorpion has it when the clock or the
    // SMUC disk is asked for.
    if (m == MACHINE_ZS1024 || m == MACHINE_SCORPEVO) return true;
    return m == MACHINE_SCORPION && (settings.rtc || settings.ide == IDE_SMUC);
}

static inline bool decoded(const uint16_t a) {
    return (a & 0x18A3u) == 0x18A2u && !(a & 0x0040u);
}

static inline bool active(void) {
    return beta_rom_paged() || (spec_port_1ffd & 0x02u);
}

static inline bool disk(void) { return settings.ide == IDE_SMUC && ide_mounted(); }

void smuc_reset(void) {
    sys = 0;
    fdd = 0;
    nv_reset();
}

bool smuc_port_out(const uint16_t a, const uint8_t v) {
    if (!decoded(a) || !smuc_fitted() || !active()) return false;
    if (a & 0x0004u) {                              // the #BE family
        if (!(a & 0x8000u)) return true;            // #5FBE / #7FBE: read-only
        if (!disk()) return true;
        const unsigned reg = (a >> 8) & 7u;
        if (!(a & 0x2000u)) { ide_smuc_latch_write(v); return true; }
        if ((sys & 0x80u) && reg == 6) ide_smuc_control(v);     // Device Control
        else                           ide_smuc_write(reg, v);
        return true;
    }
    if (a & 0x8000u) {
        if (a & 0x2000u) {                          // #FFBA SYS
            if ((v & 0x01u) && !(sys & 0x01u) && disk()) ide_reset();
            nvram24_write(v);
            sys = v;
        } else {                                    // #DFBA the clock
            if (sys & 0x80u) rtc_write(v); else rtc_select(v);
        }
    } else if (a & 0x2000u) {
        fdd = v;                                    // #7FBA
    }
    return true;
}

bool smuc_port_in(const uint16_t a, uint8_t *v) {
    if (!decoded(a) || !smuc_fitted() || !active()) return false;
    if (a & 0x0004u) {
        if (!(a & 0x8000u)) { *v = (a & 0x2000u) ? 0x57u : 0x17u; return true; }
        if (!disk()) { *v = 0x00; return true; }    // the card is in, no drive on it
        const unsigned reg = (a >> 8) & 7u;
        if (!(a & 0x2000u))                *v = ide_smuc_latch_read();
        else if ((sys & 0x80u) && reg == 6) *v = ide_smuc_read(7);      // AltStatus
        else                                *v = ide_smuc_read(reg);
        return true;
    }
    if (a & 0x8000u) *v = (a & 0x2000u) ? (uint8_t)(nvram24_read() & 0x7Fu) : rtc_read();
    else             *v = (a & 0x2000u) ? (uint8_t)(fdd | 0x3Fu) : 0x57u;
    return true;
}

// ---------------------------------------------------------------------------
// The battery, which is a file
// ---------------------------------------------------------------------------
#define NV_PATH "/smuc.nvr"

void smuc_init(void) {
    memset(nv, 0, sizeof nv);
    nv_reset();
#ifndef SMUC_HOST_TEST
    FIL f;
    if (f_open(&f, NV_PATH, FA_READ) == FR_OK) {
        UINT got = 0;
        f_read(&f, nv, sizeof nv, &got);
        f_close(&f);
    }
#endif
}

void smuc_task(void) {
#ifndef SMUC_HOST_TEST
    // A second and a half after the last byte: ProfROM writes its setup a
    // byte at a time, and the card should see one file, not two hundred.
    if (!nv_dirty || time_us_64() - nv_dirty_at < 1500000u) return;
    nv_dirty = false;
    FIL f;
    if (f_open(&f, NV_PATH, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) return;
    UINT put = 0;
    f_write(&f, nv, sizeof nv, &put);
    f_close(&f);
#endif
}
