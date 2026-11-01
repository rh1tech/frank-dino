/*
 * frank-dino — an RP2350B acting as the whole machine around a real Z80
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-dino
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "beta.h"

#include "settings.h"
#include "storage.h"
#include "ff.h"

#include <stdio.h>
#include <string.h>

#include <pico/time.h>

#include "dino.h"
#include "psram.h"
#include "settings.h"

uint8_t trd_image[TRD_MAX_BYTES] __attribute__((section(".psram")));

/* Where the image came from, and whether the guest has changed it. */
static char image_path[STORAGE_MAX_PATH];
static bool image_dirty;

bool beta_dirty(void) { return image_dirty; }

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

/*
 * Drives B, C and D.
 *
 * There is room for one whole disk in the PSRAM and not for four, so the
 * other three are read from the card a cylinder at a time -- both sides
 * of one track, 8 KB -- into a cache each. A read or write that wants a
 * cylinder not in its drive's cache is held: the controller says BUSY,
 * core 0 (beta_task) fetches the cylinder, and the command runs when it
 * is there. That is milliseconds, and TR-DOS waits revolutions for a
 * controller. Writes go into the cache and back to the card when the
 * head moves on or the drive has been quiet a moment. .trd only: the
 * other formats are converted, and a conversion has nowhere to live but
 * drive A's image.
 */
#define CYL_BYTES (TRD_TRACK * TRD_SIDES)

static struct {
    bool     mounted;
    char     path[STORAGE_MAX_PATH];
    uint32_t len;
    unsigned tracks;
} bd[3];

static uint8_t           cyl_cache[3][CYL_BYTES] __attribute__((section(".psram")));
static volatile int16_t  cyl_of[3] = { -1, -1, -1 };    // the cylinder each cache holds
static volatile bool     cyl_dirty[3];
static volatile uint64_t cyl_wrote_at[3];
static volatile int8_t   req_drive = -1;                // core 1 asks core 0 for a cylinder
static volatile uint8_t  req_cyl;
static uint8_t           deferred_cmd;                  // the command waiting for it
static bool              deferred;

static inline unsigned cur_drive(void);

static bool drive_mounted(const unsigned d) { return d ? bd[d - 1].mounted : mounted; }

bool     beta_mounted(void)    { return mounted; }
bool     beta_drive_mounted(const unsigned d) { return d < 4 && drive_mounted(d); }
const char *beta_drive_path(const unsigned d) {
    return d == 0 ? image_path : d < 4 ? bd[d - 1].path : "";
}
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
    if (!drive_mounted(cur_drive()) || !wd.type1 || (wd.status & ST_BUSY) || wd.left) return 0;
    const uint32_t us = (uint32_t)(time_us_64() % 200000u);     // 300 rpm
    return us < 4000u ? ST_INDEX : 0;
}

void beta_reset(void) {
    memset(&wd, 0, sizeof wd);
    wd.type1  = true;
    wd.status = ST_TRACK0 | ST_HEADLOAD;
    deferred = false;
    if (!mounted) wd.status |= ST_NOTREADY;
}

bool beta_mount(const uint32_t len, const char *path, char *detail, const unsigned detail_len) {
    image_len = len;
    tracks    = len / (TRD_TRACK * TRD_SIDES);
    mounted   = true;
    image_dirty = false;
    snprintf(image_path, sizeof image_path, "%s", path ? path : "");
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

bool beta_save(char *detail, const unsigned detail_len) {
    if (!mounted)        { snprintf(detail, detail_len, "No disk is in the drive."); return false; }
    if (!image_path[0])  { snprintf(detail, detail_len, "This disk came from nowhere to put it back."); return false; }
    if (!image_dirty)    { snprintf(detail, detail_len, "Nothing has changed."); return true; }

    if (!storage_write(image_path, trd_image, image_len)) {
        snprintf(detail, detail_len, "The card would not take it.");
        return false;
    }
    image_dirty = false;
    snprintf(detail, detail_len, "Saved %s", image_path);
    return true;
}

bool beta_mount_drive(const unsigned d, const char *path, char *detail, const unsigned detail_len) {
    if (d < 1 || d > 3) return false;
    FILINFO fi;
    if (f_stat(path, &fi) != FR_OK || fi.fsize < CYL_BYTES || fi.fsize % CYL_BYTES) {
        snprintf(detail, detail_len, "Drives B-D take a .trd");
        return false;
    }
    beta_eject_drive(d);
    snprintf(bd[d - 1].path, sizeof bd[d - 1].path, "%s", path);
    bd[d - 1].len = (uint32_t)fi.fsize;
    bd[d - 1].tracks = (unsigned)(fi.fsize / CYL_BYTES);
    cyl_of[d - 1] = -1;
    bd[d - 1].mounted = true;
    snprintf(detail, detail_len, "drive %c: %u tracks", 'A' + d, bd[d - 1].tracks);
    return true;
}

/* A cylinder of drive d back to the card, if it has changed. Core 0. */
static void cyl_flush(const unsigned i) {
    if (!cyl_dirty[i] || cyl_of[i] < 0) return;
    cyl_dirty[i] = false;
    FIL f;
    if (f_open(&f, bd[i].path, FA_WRITE | FA_OPEN_EXISTING) != FR_OK) return;
    UINT put = 0;
    if (f_lseek(&f, (FSIZE_t)cyl_of[i] * CYL_BYTES) == FR_OK)
        f_write(&f, cyl_cache[i], CYL_BYTES, &put);
    f_close(&f);
}

void beta_eject_drive(const unsigned d) {
    if (d == 0) { beta_eject(); return; }
    if (d > 3) return;
    cyl_flush(d - 1);
    bd[d - 1].mounted = false;
    bd[d - 1].path[0] = 0;
    cyl_of[d - 1] = -1;
}

void beta_task(void) {
    const int8_t d = req_drive;
    if (d >= 1 && d <= 3) {
        const unsigned i = (unsigned)d - 1u;
        cyl_flush(i);
        memset(cyl_cache[i], 0xFF, CYL_BYTES);
        FIL f;
        if (f_open(&f, bd[i].path, FA_READ) == FR_OK) {
            UINT got = 0;
            if (f_lseek(&f, (FSIZE_t)req_cyl * CYL_BYTES) == FR_OK)
                f_read(&f, cyl_cache[i], CYL_BYTES, &got);
            f_close(&f);
        }
        cyl_of[i] = req_cyl;
        req_drive = -1;
        return;
    }
    // A drive that has been written to and then left alone goes back to the card.
    for (unsigned i = 0; i < 3; i++)
        if (cyl_dirty[i] && !wd.left && time_us_64() - cyl_wrote_at[i] > 1500000u) cyl_flush(i);
}

void beta_eject(void) {
    mounted = false;
    image_dirty = false;
    image_path[0] = 0;
    beta_reset();
}

/*
 * What to type at the TR-DOS prompt to start this disk.
 *
 * TR-DOS runs a file called `boot` by itself, and most disks have one --
 * but a .scl is very often a single file with the game's own name, and
 * a disk with no boot leaves the user at an A> prompt with no way to
 * find out what is on it short of knowing the TR-DOS keyword for CAT.
 *
 * So: if there is a boot, say nothing and let TR-DOS get on with it. If
 * there is not, take the first real file and produce `RUN "NAME"`, which
 * is what a person would have typed. The R is a single key at the prompt
 * -- TR-DOS's command line is the BASIC editor in keyword mode -- and
 * the quotes are SYMBOL SHIFT and P, which src/spectrum/spectrum.c
 * already knows how to press.
 *
 * Returns false when nothing should be typed.
 */
bool beta_autorun(char *out, const unsigned len) {
    if (!mounted) return false;

    int first = -1;

    for (unsigned i = 0; i < 128; i++) {
        const uint8_t *e = &trd_image[i * 16];
        if (e[0] == 0x00) break;                // nothing beyond here
        if (e[0] == 0x01) continue;             // deleted

        // A boot file, whatever its extension, and the rest of the name
        // has to be blank -- "bootleg" is not a boot.
        if ((e[0] | 0x20) == 'b' && (e[1] | 0x20) == 'o' &&
            (e[2] | 0x20) == 'o' && (e[3] | 0x20) == 't') {
            bool blank = true;
            for (unsigned k = 4; k < 8; k++) if (e[k] != ' ') blank = false;
            if (blank) return false;
        }

        if (first < 0) first = (int)i;
    }

    if (first < 0) return false;

    const uint8_t *e = &trd_image[(unsigned)first * 16];
    char name[9] = {0};
    memcpy(name, e, 8);
    for (int k = 7; k >= 0 && (name[k] == ' ' || name[k] == 0); k--) name[k] = 0;
    if (!name[0]) return false;

    snprintf(out, len, "r\"%s\"\r", name);
    return true;
}

// ---------------------------------------------------------------------------

/*
 * Where a sector lives in the image.
 *
 * Sectors are numbered from one on a TR-DOS disk and from zero in the
 * file, which is the single most common off-by-one in this whole format.
 */
static inline unsigned cur_drive(void) { return wd.sysreg & 3u; }

/* Where a transfer reads and writes: drive A's image, or a drive's cylinder cache. */
static uint8_t *xfer_base;
static uint32_t xfer_len;

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
    // (xfer_base and xfer_len are set by the caller, for the drive in use)
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
                          (drive_mounted(cur_drive()) ? 0 : ST_NOTREADY));
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
static uint16_t last_port;
static uint32_t n_read, n_write, n_notfound;

void beta_stats(char *out, const unsigned len) {
    snprintf(out, len, "%02X@%04X t%u s%u/%u r%lu w%lu e%lu",
             last_cmd, last_port, last_trk, last_sec, last_side,
             (unsigned long)n_read, (unsigned long)n_write,
             (unsigned long)n_notfound);
}

/*
 * Point the transfer at the sector the registers name, on the selected
 * drive: false when it is a B-D cylinder not yet in its cache, after
 * asking core 0 for it.
 */
static uint32_t locate_off;

static bool locate(void) {
    const unsigned d = cur_drive();
    if (d == 0) {
        xfer_base  = trd_image;
        xfer_len   = image_len;
        locate_off = sector_offset(wd.track, wd.side, wd.sector);
        return true;
    }
    const unsigned i = d - 1u;
    if (!bd[i].mounted || wd.track >= bd[i].tracks) {
        xfer_base = cyl_cache[i]; xfer_len = 0; locate_off = UINT32_MAX;
        return true;                                // nothing to wait for: not found
    }
    if (cyl_of[i] != wd.track) {
        req_cyl   = wd.track;
        req_drive = (int8_t)d;
        return false;
    }
    xfer_base  = cyl_cache[i];
    xfer_len   = CYL_BYTES;
    locate_off = (wd.sector < 1 || wd.sector > TRD_SPT) ? UINT32_MAX
               : ((uint32_t)wd.side * TRD_SPT + (wd.sector - 1u)) * TRD_SECTOR;
    return true;
}

/* A read or write waiting for its cylinder: BUSY, no DRQ, until it comes. */
static void hold(const uint8_t cmd) {
    deferred_cmd = cmd;
    deferred = true;
    wd.type1  = false;
    wd.status = ST_BUSY;
    wd.drq    = false;
    wd.intrq  = false;
}

static void command(const uint8_t cmd);

/* The held command, once core 0 has brought its cylinder in. */
static void run_deferred(void) {
    if (deferred && req_drive < 0) {
        deferred = false;
        command(deferred_cmd);
    }
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
            if (!locate()) { hold(cmd); return; }
            const uint32_t off = locate_off;
            if (!drive_mounted(cur_drive()) || off == UINT32_MAX) {
                wd.type1  = false;
                wd.status = (uint8_t)(ST_NOTFOUND | (drive_mounted(cur_drive()) ? 0 : ST_NOTREADY));
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
            if (!locate()) { hold(cmd); return; }
            const uint32_t off = locate_off;
            if (!drive_mounted(cur_drive()) || off == UINT32_MAX) {
                wd.type1  = false;
                wd.status = (uint8_t)(ST_NOTFOUND | (drive_mounted(cur_drive()) ? 0 : ST_NOTREADY));
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
            deferred = false;
            end_transfer();
            type1_done();
            return;

        case 0xE:                                   // Read track
        case 0xF:                                   // Write track (format)
            /*
             * Accepted, and finished before it began.
             *
             * A .trd is sectors with no gaps and no address marks, so
             * there is nothing for either of these to read or write. The
             * first version set up a track's worth of DRQ and waited for
             * the bytes, which is closer to what the chip does and is a
             * trap: a caller that changes its mind leaves the controller
             * permanently busy and the machine hangs on the next status
             * poll. Reporting completion immediately cannot hang anyone.
             */
            wd.type1  = false;
            wd.status = 0;
            wd.drq    = false;
            wd.left   = 0;
            wd.intrq  = true;
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

/*
 * The controller's ports open without the TR-DOS ROM: the Profi's CP/M
 * mode reaches the WD1793 through its own port numbers, which spectrum.c
 * translates to these, with no ROM switch at all.
 */
volatile bool beta_ports_open;

bool __not_in_flash_func(beta_port_out)(const uint16_t port, const uint8_t value) {
    // Only while the ROM is in. That is the real interface's rule, and it
    // is what lets a Kempston joystick share #1F with the status register.
    if (!settings.betadisk || !(rom_paged || beta_ports_open) || !BETA_SELECTED(port)) return false;

    switch (BETA_REG(port)) {
        case 0: last_port = port; command(value); break;         // #1F
        case 1: wd.track  = value; break;                       // #3F
        case 2: wd.sector = value; break;                       // #5F

        case 3:                                                 // #7F, data
            wd.data = value;
            if (wd.left && wd.writing) {
                if (!wd.from_idfield && wd.addr < xfer_len
                    && !settings.disk_wrprot) {
                    xfer_base[wd.addr] = value;
                    const unsigned d = cur_drive();
                    if (d) { cyl_dirty[d - 1] = true; cyl_wrote_at[d - 1] = time_us_64(); }
                    else   image_dirty = true;
                }
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

bool __not_in_flash_func(beta_port_in)(const uint16_t port, uint8_t *value) {
    if (!settings.betadisk || !(rom_paged || beta_ports_open) || !BETA_SELECTED(port)) return false;

    switch (BETA_REG(port)) {
        case 0:                                                 // #1F, status
            // Reading the status is what clears the interrupt request,
            // and TR-DOS's wait loops depend on that.
            run_deferred();
            wd.intrq = false;
            *value = (uint8_t)(wd.status | index_pulse());
            break;

        case 1: *value = wd.track;  break;                      // #3F
        case 2: *value = wd.sector; break;                      // #5F

        case 3:                                                 // #7F, data
            if (wd.left && !wd.writing) {
                wd.data = wd.from_idfield ? wd.idfield[sizeof wd.idfield - wd.left]
                        : wd.addr < xfer_len ? xfer_base[wd.addr] : 0xFF;
                wd.addr++;
                if (--wd.left == 0) end_transfer();
            }
            *value = wd.data;
            break;

        case 7:                                                 // #FF, system
            // Only two bits, and they are the two that matter: TR-DOS
            // polls this rather than the status register in its tight
            // transfer loops.
            run_deferred();
            *value = (uint8_t)((wd.intrq ? 0x80 : 0) | (wd.drq ? 0x40 : 0));
            break;

        default: *value = 0xFF; break;
    }
    return true;
}
