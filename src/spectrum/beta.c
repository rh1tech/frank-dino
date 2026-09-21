/*
 * frank-dino — an RP2350B acting as the whole machine around a real Z80
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-dino
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "beta.h"

#include <stdio.h>
#include <string.h>

#include <pico/time.h>

#include "dino.h"
#include "psram.h"
#include "settings.h"

uint8_t trd_image[TRD_MAX_BYTES] __attribute__((section(".psram")));

// ---------------------------------------------------------------------------
// The disk
// ---------------------------------------------------------------------------
//
// A TR-DOS disk is the simplest geometry there is: sixteen sectors of
// 256 bytes to a track, two sides, and the sides interleaved so that
// track 0 side 0 is followed by track 0 side 1. A .trd is exactly those
// bytes in that order with no header, which is why the size alone says
// how many tracks there are.
#define TRD_SECTOR   256
#define TRD_SPT      16
#define TRD_SIDES    2
#define TRD_TRACK    (TRD_SECTOR * TRD_SPT)

static uint32_t image_len;
static unsigned tracks;
static bool     mounted;
static bool     boot_pending;
static bool     rom_paged;

bool     beta_mounted(void)    { return mounted; }
unsigned beta_tracks(void)     { return tracks; }
bool     beta_rom_paged(void)  { return rom_paged; }
void     beta_set_rom_paged(const bool on) { rom_paged = on; }
void     beta_boot_next_reset(void) { boot_pending = true; }

bool beta_take_boot_request(void) {
    const bool b = boot_pending;
    boot_pending = false;
    return b;
}

// ---------------------------------------------------------------------------
// The WD1793
// ---------------------------------------------------------------------------

// Status bits, which mean two different things depending on the command.
#define ST_BUSY      0x01
#define ST_INDEX     0x02      // type I
#define ST_DRQ       0x02      // type II and III
#define ST_TRACK0    0x04      // type I
#define ST_LOSTDATA  0x04      // type II and III
#define ST_CRC       0x08
#define ST_SEEKERR   0x10      // type I
#define ST_NOTFOUND  0x10      // type II and III
#define ST_HEADLOAD  0x20      // type I
#define ST_WRITEPROT 0x40
#define ST_NOTREADY  0x80

static struct {
    uint8_t  status, track, sector, data, sysreg;
    uint8_t  side;
    bool     intrq, drq;

    /*
     * The transfer in progress.
     *
     * A real controller streams bytes off the surface as it goes by; this
     * has them all already, so a transfer is a pointer, a count and a
     * direction. `addr` of zero with `left` of zero means idle.
     */
    uint32_t addr;
    uint32_t left;
    bool     writing;

    // Read Address returns six bytes that are not on the disk at all, so
    // they are built here and streamed from here.
    uint8_t  idfield[6];
    bool     from_idfield;

    /*
     * Which set of meanings the status register currently has.
     *
     * A WD1793's status bits mean different things depending on the last
     * command: after a type I, bit 1 is the index pulse; after a type II
     * or III it is DRQ. That is not a detail -- it is the difference
     * between a manufactured index pulse being a sign of life and being
     * a phantom byte in the middle of somebody's sector.
     */
    bool     type1;
} wd;

/*
 * The index hole, which is the only proof a disk is turning.
 *
 * Two things read it and they want different things. TR-DOS decides
 * whether there is a disk in the drive at all by waiting for the pulse,
 * and it times operations out by counting revolutions -- so the pulse
 * has to be a clock, not a counter. A disk turns at 300 rpm, which is
 * one pulse every 200 ms lasting about four of them, and saying exactly
 * that costs one modulo.
 *
 * And it is bit 1 of the status register, which is DRQ after a type II
 * or III command. That is not a detail to gloss over: a phantom pulse
 * offered to a transfer loop that is polling for DRQ is an extra byte
 * read out of a finished sector and written into the middle of whatever
 * was being loaded. It showed up as a boot program that drew its menu
 * correctly about half the time and drew noise the rest -- which looks
 * like flaky memory and is not.
 */
static uint8_t index_pulse(void) {
    if (!mounted || !wd.type1 || (wd.status & ST_BUSY) || wd.left) return 0;
    const uint32_t us = (uint32_t)(time_us_64() % 200000u);     // 300 rpm
    return us < 4000u ? ST_INDEX : 0;
}

void beta_reset(void) {
    memset(&wd, 0, sizeof wd);
    wd.type1  = true;
    wd.status = ST_TRACK0 | ST_HEADLOAD;
    if (!mounted) wd.status |= ST_NOTREADY;
}

bool beta_mount(const uint32_t len, char *detail, const unsigned detail_len) {
    if (!psram_size()) {
        snprintf(detail, detail_len, "disks need the PSRAM, and none was found");
        return false;
    }
    if (len < TRD_TRACK * TRD_SIDES || len > TRD_MAX_BYTES) {
        snprintf(detail, detail_len, "a .trd of %luK is not a disk",
                 (unsigned long)(len / 1024));
        return false;
    }

    image_len = len;
    tracks    = len / (TRD_TRACK * TRD_SIDES);
    mounted   = true;
    beta_reset();

    /*
     * Sector 9 of track 0 is TR-DOS's volume descriptor: byte 0xE3 of it
     * is the disk type and 0xE5..0xE6 the free sector count. Reading the
     * disk label out of it turns "mounted" into something the user can
     * check against the disk they meant.
     */
    const uint8_t *vol = &trd_image[8 * TRD_SECTOR];
    char label[10] = {0};
    memcpy(label, &vol[0xF5], 8);
    for (int i = 0; i < 8; i++)
        if (label[i] < 0x20 || label[i] > 0x7E) label[i] = ' ';

    snprintf(detail, detail_len, "disk mounted: %u tracks, %u files, \"%s\"",
             tracks, vol[0xE4], label);
    return true;
}

void beta_eject(void) {
    mounted = false;
    beta_reset();
}

// ---------------------------------------------------------------------------

/*
 * Where a sector lives in the image.
 *
 * Sectors are numbered from one on a TR-DOS disk and from zero in the
 * file, which is the single most common off-by-one in this whole format.
 */
static uint32_t sector_offset(const uint8_t track, const uint8_t side, const uint8_t sector) {
    if (sector < 1 || sector > TRD_SPT) return UINT32_MAX;
    const uint32_t lt = (uint32_t)track * TRD_SIDES + side;
    const uint32_t off = (lt * TRD_SPT + (sector - 1u)) * TRD_SECTOR;
    return off + TRD_SECTOR <= image_len ? off : UINT32_MAX;
}

static void end_transfer(void) {
    wd.left  = 0;
    wd.drq   = false;
    wd.status &= (uint8_t)~(ST_BUSY | ST_DRQ);
    wd.intrq = true;
}

static void start_transfer(const uint32_t addr, const uint32_t len, const bool writing) {
    wd.addr    = addr;
    wd.left    = len;
    wd.writing = writing;
    wd.drq     = true;
    wd.status  = ST_BUSY | ST_DRQ;
    wd.intrq   = false;
    wd.type1   = false;
    wd.from_idfield = false;
}

/* The status a type I command leaves behind. */
static void type1_done(void) {
    wd.type1  = true;
    wd.status = (uint8_t)(ST_HEADLOAD | (wd.track == 0 ? ST_TRACK0 : 0) |
                          (mounted ? 0 : ST_NOTREADY));
    wd.intrq = true;
    wd.drq   = false;
}

/*
 * The last thing the controller was asked to do, for the settings page.
 *
 * There is no other way to see inside this: the disk is in the PSRAM,
 * the CPU is real, and a load that quietly produces a black screen looks
 * identical whether the sector was not found, the side was wrong or the
 * program simply crashed. Four numbers and two counters turn that into a
 * question with an answer.
 */
static uint8_t  last_cmd, last_trk, last_sec, last_side;
static uint32_t n_read, n_write, n_notfound;

void beta_stats(char *out, const unsigned len) {
    snprintf(out, len, "%02X t%u s%u/%u r%lu w%lu e%lu",
             last_cmd, last_trk, last_sec, last_side,
             (unsigned long)n_read, (unsigned long)n_write,
             (unsigned long)n_notfound);
}

static void command(const uint8_t cmd) {
    const uint8_t type = cmd >> 4;

    last_cmd  = cmd;
    last_trk  = wd.track;
    last_sec  = wd.sector;
    last_side = wd.side;

    switch (type) {
        case 0x0:                                   // Restore
            wd.track = 0;
            type1_done();
            return;

        case 0x1:                                   // Seek
            wd.track = wd.data;
            type1_done();
            return;

        case 0x2: case 0x3:                         // Step, with and without update
            type1_done();
            return;

        case 0x4: case 0x5:                         // Step in
            if (wd.track < 255) wd.track++;
            type1_done();
            return;

        case 0x6: case 0x7:                         // Step out
            if (wd.track > 0) wd.track--;
            type1_done();
            return;

        case 0x8: case 0x9: {                       // Read sector
            const uint32_t off = sector_offset(wd.track, wd.side, wd.sector);
            if (!mounted || off == UINT32_MAX) {
                wd.type1  = false;
                wd.status = (uint8_t)(ST_NOTFOUND | (mounted ? 0 : ST_NOTREADY));
                wd.intrq  = true;
                n_notfound++;
                return;
            }
            // Bit 4 is the multiple-sector flag. Honoured to the end of
            // the track, which is as far as a TR-DOS disk goes.
            const unsigned n = (cmd & 0x10) ? (TRD_SPT - wd.sector + 1u) : 1u;
            n_read++;
            start_transfer(off, n * TRD_SECTOR, false);
            return;
        }

        case 0xA: case 0xB: {                       // Write sector
            const uint32_t off = sector_offset(wd.track, wd.side, wd.sector);
            if (!mounted || off == UINT32_MAX) {
                wd.type1  = false;
                wd.status = (uint8_t)(ST_NOTFOUND | (mounted ? 0 : ST_NOTREADY));
                wd.intrq  = true;
                n_notfound++;
                return;
            }
            const unsigned n = (cmd & 0x10) ? (TRD_SPT - wd.sector + 1u) : 1u;
            n_write++;
            start_transfer(off, n * TRD_SECTOR, true);
            return;
        }

        case 0xC:                                   // Read address
            /*
             * The six bytes of an ID field, which exist on the surface of
             * a real disk and nowhere in a .trd -- so they are made up
             * from the geometry every TR-DOS disk has. TR-DOS uses this
             * to work out where the head is after a restore.
             */
            wd.idfield[0] = wd.track;
            wd.idfield[1] = wd.side;
            wd.idfield[2] = 1;                      // first sector of the track
            wd.idfield[3] = 1;                      // 256 bytes
            wd.idfield[4] = 0;                      // CRC, which nothing checks here
            wd.idfield[5] = 0;
            wd.sector     = wd.track;               // the controller does this
            start_transfer(0, sizeof wd.idfield, false);
            wd.from_idfield = true;
            return;

        case 0xD:                                   // Force interrupt
            end_transfer();
            type1_done();
            return;

        case 0xE:                                   // Read track
        case 0xF:                                   // Write track (format)
            /*
             * Formatting is accepted and discarded.
             *
             * A .trd has no gaps, no address marks and no room to put
             * them, so a format cannot be stored -- but a controller that
             * refuses one leaves TR-DOS waiting for a DRQ that never
             * comes. Consuming a track's worth of bytes and reporting
             * success is the behaviour that keeps it moving.
             */
            start_transfer(0, TRD_TRACK, type == 0xF);
            wd.from_idfield = true;                 // nowhere to put it
            return;

        default:
            return;
    }
}

// ---------------------------------------------------------------------------
// The ports
// ---------------------------------------------------------------------------
/*
 * Five registers, and a decode that is looser than the port names imply.
 *
 * The interface looks at A0, A1 and A7-A5 and nothing else, so what
 * selects it is the bottom two address bits both being set -- #1F is
 * merely the name everyone uses. A7-A5 then choose: 0 command and
 * status, 1 track, 2 sector, 3 data, 7 the interface's own system
 * register. 4, 5 and 6 are not decoded.
 *
 * Widening this from the obvious (port & 0x1F) == 0x1F matters because
 * software does use the aliases, and because the collisions that follow
 * -- #7B, #FB -- are collisions the real hardware has too.
 */
#define BETA_SELECTED(p) (((p) & 0x03) == 0x03)
#define BETA_REG(p)      (((p) >> 5) & 0x07)

bool beta_port_out(const uint16_t port, const uint8_t value) {
    // Only while the ROM is in. That is the real interface's rule, and it
    // is what lets a Kempston joystick share #1F with the status register.
    if (!settings.betadisk || !rom_paged || !BETA_SELECTED(port)) return false;

    switch (BETA_REG(port)) {
        case 0: command(value); break;                          // #1F
        case 1: wd.track  = value; break;                       // #3F
        case 2: wd.sector = value; break;                       // #5F

        case 3:                                                 // #7F, data
            wd.data = value;
            if (wd.left && wd.writing) {
                if (!wd.from_idfield && wd.addr < image_len) trd_image[wd.addr] = value;
                wd.addr++;
                if (--wd.left == 0) end_transfer();
            }
            break;

        case 7:                                                 // #FF, system
            /*
             * Drive, side and reset. Bit 4 is the side and it is
             * inverted -- zero selects side one -- which is the sort of
             * detail that shows up as every other track being wrong.
             */
            if (!(value & 0x04)) beta_reset();                  // /RESET is active low
            wd.sysreg = value;
            wd.side   = (uint8_t)(((value >> 4) & 1) ^ 1);
            break;

        default: break;                                         // not decoded
    }
    return true;
}

bool beta_port_in(const uint16_t port, uint8_t *value) {
    if (!settings.betadisk || !rom_paged || !BETA_SELECTED(port)) return false;

    switch (BETA_REG(port)) {
        case 0:                                                 // #1F, status
            // Reading the status is what clears the interrupt request,
            // and TR-DOS's wait loops depend on that.
            wd.intrq = false;
            *value = (uint8_t)(wd.status | index_pulse());
            break;

        case 1: *value = wd.track;  break;                      // #3F
        case 2: *value = wd.sector; break;                      // #5F

        case 3:                                                 // #7F, data
            if (wd.left && !wd.writing) {
                wd.data = wd.from_idfield ? wd.idfield[sizeof wd.idfield - wd.left]
                        : wd.addr < image_len ? trd_image[wd.addr] : 0xFF;
                wd.addr++;
                if (--wd.left == 0) end_transfer();
            }
            *value = wd.data;
            break;

        case 7:                                                 // #FF, system
            // Only two bits, and they are the two that matter: TR-DOS
            // polls this rather than the status register in its tight
            // transfer loops.
            *value = (uint8_t)((wd.intrq ? 0x80 : 0) | (wd.drq ? 0x40 : 0));
            break;

        default: *value = 0xFF; break;
    }
    return true;
}
