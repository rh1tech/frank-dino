/*
 * frank-dino — an RP2350B acting as the whole machine around a real Z80
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-dino
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The port schemes are pico-speccy's (https://github.com/drewpo28/pico-speccy,
 * GPL-3.0, devices/storage/IDE.cpp and core/Ports.cpp), after UnrealSpeccy;
 * the HDF header is RAMsoft's RS-IDE format.
 */

/*
 * An ATA hard disk, as a file on the card.
 *
 * One drive, the master: an .hdf (RS-IDE, with its own IDENTIFY block) or a
 * raw image, read and written a sector at a time through FatFs. Enough of
 * ATA for the ROMs that boot from it -- IDENTIFY, READ and WRITE SECTORS
 * in CHS or LBA, the multiple-sector forms, INITIALIZE DEVICE PARAMETERS,
 * SET FEATURES, RECALIBRATE, DIAGNOSTIC -- behind whichever interface the
 * machine had:
 *
 *   NEMO     registers at #10-#F0 (A7-A5), the high data byte through #11
 *   DivIDE   #A3 data, #A7-#BF registers; the data port alternates bytes
 *   ATM      the xx0F family, A7-A5 the register, A8 the high-byte latch
 */
#include "ide.h"

#include <stdio.h>
#include <string.h>

#include "ff.h"
#include "settings.h"

static FIL      img;
static bool     open_;
static uint32_t data_at;        // where sector 0 starts in the file
static uint16_t cyls, heads, spt;
static uint32_t total;
static uint8_t  identify[512];

/* The task file. */
static uint8_t  err, feat, count, sec, cyl_lo, cyl_hi, dh, status = 0x50, ctrl;
static uint8_t  buf[512];
static int      pos = -1;       // -1 no transfer; 0-511 into buf
static bool     writing;
static uint32_t left;           // sectors still to go in this command
static uint8_t  latch;          // the 16-bit interfaces' other byte
static bool     div_hi;         // DivIDE: the next data byte is the high one
static char     path_[96];

#define ST_BSY  0x80u
#define ST_DRDY 0x40u
#define ST_DSC  0x10u
#define ST_DRQ  0x08u
#define ST_ERR  0x01u

static void w16(uint8_t *p, const uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }

/* ATA strings are byte-swapped in pairs. */
static void ata_str(uint8_t *p, const char *s, const unsigned n) {
    for (unsigned i = 0; i < n; i += 2) {
        p[i + 1] = (uint8_t)(*s ? *s++ : ' ');
        p[i]     = (uint8_t)(*s ? *s++ : ' ');
    }
}

static void build_identify(void) {
    memset(identify, 0, sizeof identify);
    w16(identify + 0, 0x0040);
    w16(identify + 2, cyls);
    w16(identify + 6, heads);
    w16(identify + 12, spt);
    ata_str(identify + 20, "FRANKDINO0001", 20);
    ata_str(identify + 46, "1.0", 8);
    ata_str(identify + 54, "frank-dino hard disk", 40);
    w16(identify + 94, 0x8001);                  // READ/WRITE MULTIPLE, 1 sector
    w16(identify + 98, 0x0200);                  // LBA
    w16(identify + 106, 0x0001);
    w16(identify + 108, cyls);
    w16(identify + 110, heads);
    w16(identify + 112, spt);
    w16(identify + 114, (uint16_t)total);
    w16(identify + 116, (uint16_t)(total >> 16));
    w16(identify + 120, (uint16_t)total);
    w16(identify + 122, (uint16_t)(total >> 16));
}

void ide_eject(void) {
    if (open_) f_close(&img);
    open_ = false;
    path_[0] = 0;
}

bool ide_mount(const char *path, char *detail, const unsigned detail_len) {
    ide_eject();
    if (f_open(&img, path, FA_READ | FA_WRITE) != FR_OK && f_open(&img, path, FA_READ) != FR_OK) {
        snprintf(detail, detail_len, "cannot open that image");
        return false;
    }
    open_ = true;
    snprintf(path_, sizeof path_, "%s", path);

    uint8_t h[0x16 + 106];
    UINT got = 0;
    f_read(&img, h, sizeof h, &got);
    if (got >= 0x16 && !memcmp(h, "RS-IDE\x1A", 7)) {
        // HDF: the drive's own IDENTIFY is in the header; take its geometry.
        data_at = (uint32_t)h[9] | ((uint32_t)h[10] << 8);
        memset(identify, 0, sizeof identify);
        memcpy(identify, h + 0x16, got - 0x16 < 106u ? got - 0x16 : 106u);
        cyls  = (uint16_t)(identify[2] | (identify[3] << 8));
        heads = (uint16_t)(identify[6] | (identify[7] << 8));
        spt   = (uint16_t)(identify[12] | (identify[13] << 8));
    } else {
        data_at = 0;
        cyls = heads = spt = 0;
    }
    const uint32_t sectors = (uint32_t)((f_size(&img) - data_at) / 512u);
    if (!cyls || !heads || !spt) {
        heads = 16; spt = 63;
        cyls = (uint16_t)(sectors / (16u * 63u) ? (sectors / (16u * 63u) > 16383u ? 16383u
                                                                                  : sectors / (16u * 63u))
                                                : 1u);
    }
    total = sectors;
    build_identify();
    status = ST_DRDY | ST_DSC;
    snprintf(detail, detail_len, "%lu MB, C%u H%u S%u", (unsigned long)(sectors >> 11),
             cyls, heads, spt);
    return true;
}

bool ide_mounted(void) { return open_; }
const char *ide_path(void) { return path_; }

static uint32_t cur_lba(void) {
    if (dh & 0x40u)
        return ((uint32_t)(dh & 0x0Fu) << 24) | ((uint32_t)cyl_hi << 16) | ((uint32_t)cyl_lo << 8) | sec;
    const uint32_t c = ((uint32_t)cyl_hi << 8) | cyl_lo;
    return (c * heads + (dh & 0x0Fu)) * spt + (sec ? sec - 1u : 0u);
}

static void set_lba(const uint32_t l) {
    if (dh & 0x40u) {
        sec = (uint8_t)l; cyl_lo = (uint8_t)(l >> 8); cyl_hi = (uint8_t)(l >> 16);
        dh = (uint8_t)((dh & 0xF0u) | ((l >> 24) & 0x0Fu));
        return;
    }
    const uint32_t c = l / ((uint32_t)heads * spt), r = l % ((uint32_t)heads * spt);
    cyl_lo = (uint8_t)c; cyl_hi = (uint8_t)(c >> 8);
    dh = (uint8_t)((dh & 0xF0u) | (r / spt));
    sec = (uint8_t)(r % spt + 1u);
}

static bool sector_io(const uint32_t l, const bool wr) {
    if (!open_ || l >= total) return false;
    if (f_lseek(&img, (FSIZE_t)data_at + (FSIZE_t)l * 512u) != FR_OK) return false;
    UINT n = 0;
    if (wr) {
        const bool ok = f_write(&img, buf, 512, &n) == FR_OK && n == 512;
        f_sync(&img);
        return ok;
    }
    if (f_read(&img, buf, 512, &n) != FR_OK) return false;
    if (n < 512) memset(buf + n, 0, 512 - n);
    return true;
}

static void fail(void) { status = ST_DRDY | ST_DSC | ST_ERR; err = 0x04; pos = -1; }

static void next_read(void) {
    if (!sector_io(cur_lba(), false)) { fail(); return; }
    pos = 0;
    writing = false;
    status = ST_DRDY | ST_DSC | ST_DRQ;
}

static void command(const uint8_t c) {
    err = 0;
    if (!open_ || (dh & 0x10u)) {               // no slave, and no master without an image
        status = (dh & 0x10u) ? 0 : (ST_DRDY | ST_ERR);
        err = 0x04;
        return;
    }
    switch (c) {
        case 0xEC:                              // IDENTIFY DEVICE
            memcpy(buf, identify, 512);
            pos = 0; writing = false; left = 1;
            status = ST_DRDY | ST_DSC | ST_DRQ;
            return;
        case 0x20: case 0x21: case 0xC4:        // READ SECTORS / MULTIPLE
            left = count ? count : 256u;
            next_read();
            return;
        case 0x30: case 0x31: case 0xC5:        // WRITE SECTORS / MULTIPLE
            left = count ? count : 256u;
            pos = 0; writing = true;
            status = ST_DRDY | ST_DSC | ST_DRQ;
            return;
        case 0x90:                              // EXECUTE DIAGNOSTIC
            err = 0x01;
            status = ST_DRDY | ST_DSC;
            return;
        case 0x91:                              // INITIALIZE DEVICE PARAMETERS
            if (count) { spt = count; heads = (uint16_t)((dh & 0x0Fu) + 1u); }
            status = ST_DRDY | ST_DSC;
            return;
        default:
            if ((c & 0xF0u) == 0x10u || c == 0xEF || c == 0xC6 || c == 0x40 || c == 0x41
                || c == 0x70 || c == 0xE0 || c == 0xE1 || c == 0xE5 || c == 0xE7) {
                status = ST_DRDY | ST_DSC;      // RECALIBRATE, SET FEATURES, SEEK, idle...
                return;
            }
            status = ST_DRDY | ST_DSC | ST_ERR;
            err = 0x04;                         // aborted
            return;
    }
}

static uint8_t data_read(void) {
    if (pos < 0 || writing) return 0xFF;
    const uint8_t v = buf[pos++];
    if (pos >= 512) {
        pos = -1;
        status = ST_DRDY | ST_DSC;
        if (--left) {
            set_lba(cur_lba() + 1u);
            count--;
            next_read();
        }
    }
    return v;
}

static void data_write(const uint8_t v) {
    if (pos < 0 || !writing) return;
    buf[pos++] = v;
    if (pos >= 512) {
        if (!sector_io(cur_lba(), true)) { fail(); return; }
        pos = -1;
        status = ST_DRDY | ST_DSC;
        if (--left) {
            set_lba(cur_lba() + 1u);
            count--;
            pos = 0;
            status = ST_DRDY | ST_DSC | ST_DRQ;
        }
    }
}

/* The task file by register number: 0 data, 1 error/features ... 7 status/command. */
static uint8_t reg_read(const unsigned r) {
    switch (r & 7u) {
        case 0: return data_read();
        case 1: return err;
        case 2: return count;
        case 3: return sec;
        case 4: return cyl_lo;
        case 5: return cyl_hi;
        case 6: return dh;
        default: return (dh & 0x10u) ? 0x00u : status;
    }
}

static void reg_write(const unsigned r, const uint8_t v) {
    switch (r & 7u) {
        case 0: data_write(v); break;
        case 1: feat = v; break;
        case 2: count = v; break;
        case 3: sec = v; break;
        case 4: cyl_lo = v; break;
        case 5: cyl_hi = v; break;
        case 6: dh = v; break;
        default: command(v); break;
    }
}

// ---------------------------------------------------------------------------
// The interfaces
// ---------------------------------------------------------------------------
/*
 * A 16-bit read: the low byte through the data register, the high one
 * parked in the latch for the interface's other port.
 */
static uint8_t read16_lo(void) {
    const uint8_t lo = data_read();
    latch = data_read();
    return lo;
}

static void write16_lo(const uint8_t lo) { data_write(lo); data_write(latch); }

/*
 * The Evo's NEMO decode moves whole words through #10 alone: each access
 * flips a trigger, so the first read fetches the word and returns its low
 * byte and the second the high one, and two writes send one word. Any
 * other register, #11 included, resets it -- so the classic #10 + #11
 * pair still works. Evo ProfROM reads a sector with INIR : INIR on #10.
 * After pico-speccy's EvoBase.cpp and Unreal's IDE_NEMO_DIVIDE.
 */
bool ide_nemo_toggle;
static uint8_t nemo_rt, nemo_wt, nemo_w1, nemo_wlo;

bool ide_port_out(const uint16_t port, const uint8_t v) {
    if (!open_) return false;
    const uint8_t lo = (uint8_t)port;
    switch (settings.ide) {
        case IDE_NEMO:
            if (lo == 0x11u) { latch = v; nemo_wt = 0; nemo_w1 = 1; return true; }
            if ((lo & 0x07u) == 0 && (((lo >> 3) ^ (lo >> 4)) & 1u)) {
                const unsigned r = (lo >> 5) & 7u;
                if (r != 0) { nemo_wt = 0; reg_write(r, v); return true; }
                if (!ide_nemo_toggle) { write16_lo(v); return true; }
                nemo_wt ^= 1u;
                if (nemo_w1)     { nemo_w1 = 0; write16_lo(v); }       // #11 then #10
                else if (nemo_wt) nemo_wlo = v;                        // hold the low byte
                else             { latch = v; write16_lo(nemo_wlo); }  // then send both
                return true;
            }
            if (lo == 0xC8u) { ctrl = v; return true; }
            return false;
        case IDE_DIVIDE:
            if ((lo & 0xE3u) == 0xA3u) {
                const unsigned r = (lo >> 2) & 7u;
                if (r == 0) { data_write(v); } else reg_write(r, v);
                return true;
            }
            return false;
        case IDE_ATM:
            if ((lo & 0x1Fu) == 0x0Fu) {
                if (port & 0x100u) { latch = v; return true; }
                const unsigned r = (lo >> 5) & 7u;
                if (r == 0) write16_lo(v); else reg_write(r, v);
                return true;
            }
            return false;
        default:
            return false;
    }
}

bool ide_port_in(const uint16_t port, uint8_t *v) {
    if (!open_) return false;
    const uint8_t lo = (uint8_t)port;
    switch (settings.ide) {
        case IDE_NEMO:
            if (lo == 0x11u) { nemo_rt = 0; *v = latch; return true; }
            if ((lo & 0x07u) == 0 && (((lo >> 3) ^ (lo >> 4)) & 1u)) {
                const unsigned r = (lo >> 5) & 7u;
                if (r != 0) { nemo_rt = 0; *v = reg_read(r); return true; }
                if (!ide_nemo_toggle) { *v = read16_lo(); return true; }
                nemo_rt ^= 1u;
                *v = nemo_rt ? read16_lo() : latch;
                return true;
            }
            if (lo == 0xC8u) { *v = status; return true; }
            return false;
        case IDE_DIVIDE:
            if ((lo & 0xE3u) == 0xA3u) {
                const unsigned r = (lo >> 2) & 7u;
                *v = reg_read(r);
                (void)div_hi;
                return true;
            }
            return false;
        case IDE_ATM:
            if ((lo & 0x1Fu) == 0x0Fu) {
                if (port & 0x100u) { *v = latch; return true; }
                const unsigned r = (lo >> 5) & 7u;
                *v = r == 0 ? read16_lo() : reg_read(r);
                return true;
            }
            return false;
        default:
            return false;
    }
}

uint8_t ide_smuc_read(const unsigned r) { return (r & 7u) == 0 ? read16_lo() : reg_read(r); }
void    ide_smuc_write(const unsigned r, const uint8_t v) { if ((r & 7u) == 0) write16_lo(v); else reg_write(r, v); }
uint8_t ide_smuc_latch_read(void) { return latch; }
void    ide_smuc_latch_write(const uint8_t v) { latch = v; }

/* Device Control's SRST: set and cleared again is a reset, and the drive
 * comes back ready -- ProfROM's driver waits for DRDY after it. */
void ide_smuc_control(const uint8_t v) {
    if ((ctrl & 0x04u) && !(v & 0x04u)) ide_reset();
    ctrl = v;
}

void ide_reset(void) {
    err = 0x01; count = 1; sec = 1; cyl_lo = cyl_hi = 0; dh = 0;
    pos = -1; left = 0; div_hi = false;
    nemo_rt = nemo_wt = nemo_w1 = nemo_wlo = 0;
    status = open_ ? (ST_DRDY | ST_DSC) : 0;
}
