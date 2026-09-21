/*
 * frank-dino — an RP2350B acting as the whole machine around a real Z80
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-dino
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "fdc.h"

#include <stdio.h>
#include <string.h>

#include <pico.h>

#include "settings.h"
#include "spectrum.h"
#include "storage.h"

uint8_t dsk_image[DSK_MAX_BYTES] __attribute__((section(".psram")));

/* ── the disk image ──────────────────────────────────────────────────── */
/*
 * CPCEMU's .dsk, in both flavours, from pico-speccy's DskImage.cpp.
 *
 * A 256-byte file header, then one 256-byte Track-Info header per track
 * followed by that track's sector data. The two flavours differ only in
 * where a track's size comes from: the standard one has a single size
 * for every track at 0x32, and the extended one has a table of sizes in
 * units of 256 bytes from 0x34 -- which is what lets it hold tracks with
 * different numbers of sectors, and unformatted tracks with none.
 */
#define DSK_HDR       256
#define TRK_HDR       256
#define DSK_MAX_TRACK 84
#define DSK_MAX_SIDES 2

static bool     mounted;
static bool     extended;
static uint8_t  n_tracks, n_sides;
static uint32_t track_off[DSK_MAX_TRACK * DSK_MAX_SIDES];  /* 0 = unformatted */
static uint32_t image_len;

/* Counters, for the settings page: a load that fails is a black screen. */
static uint32_t n_reads, n_writes, n_errors;
static char     image_path[STORAGE_MAX_PATH];
static bool     image_dirty;

bool fdc_dirty(void) { return image_dirty; }
static char     last_cmd[24] = "idle";

unsigned fdc_tracks(void) { return n_tracks; }
unsigned fdc_sides(void)  { return n_sides; }
bool     fdc_mounted(void) { return mounted; }

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

bool fdc_mount(const uint32_t len, const char *path, char *detail, const unsigned detail_len) {
    mounted = false;
    if (len < DSK_HDR + TRK_HDR) {
        snprintf(detail, detail_len, "that file is too short to be a .dsk");
        return false;
    }

    /*
     * Their dskProbe()'s rule, and the reason for it: both signatures
     * are 34 bytes and both put "Disk-Info\r\n" at 0x17, but real-world
     * images vary in the vendor half ahead of it -- some writers put
     * their own name there. So the marker decides whether this is a disk
     * and the leading word only picks the flavour.
     */
    if (memcmp(&dsk_image[0x17], "Disk-Info\r\n", 11) != 0) {
        snprintf(detail, detail_len, "that is not a .dsk");
        return false;
    }
    extended = !memcmp(dsk_image, "EXTENDED", 8);

    n_tracks = dsk_image[0x30];
    n_sides  = dsk_image[0x31];
    if (!n_tracks || !n_sides || n_sides > DSK_MAX_SIDES
        || n_tracks > DSK_MAX_TRACK) {
        snprintf(detail, detail_len, "%u tracks and %u sides is not a disk",
                 n_tracks, n_sides);
        return false;
    }

    /*
     * Walk the tracks once and remember where each begins.
     *
     * An unformatted track is a zero in the size table and occupies no
     * space at all in the file, so the offsets cannot be computed from
     * the track number -- which is the whole reason the extended format
     * exists and the one thing that makes this loop necessary.
     */
    const uint16_t fixed = rd16(&dsk_image[0x32]);
    uint32_t at = DSK_HDR;
    const unsigned n = (unsigned)n_tracks * n_sides;

    for (unsigned i = 0; i < n; i++) {
        uint32_t size;
        if (extended) {
            size = (uint32_t)dsk_image[0x34 + i] * 256u;
        } else {
            size = fixed;
        }
        if (!size) { track_off[i] = 0; continue; }          /* unformatted */
        if (at + size > len) {
            snprintf(detail, detail_len, "the image ends inside track %u", i);
            return false;
        }
        track_off[i] = at;
        at += size;
    }

    image_len = len;
    mounted = true;
    image_dirty = false;
    snprintf(image_path, sizeof image_path, "%s", path ? path : "");
    n_reads = n_writes = n_errors = 0;
    snprintf(last_cmd, sizeof last_cmd, "mounted");
    snprintf(detail, detail_len, "%u tracks, %u side%s, %s",
             n_tracks, n_sides, n_sides == 1 ? "" : "s",
             extended ? "extended" : "standard");
    return true;
}

bool fdc_save(char *detail, const unsigned detail_len) {
    if (!mounted)       { snprintf(detail, detail_len, "No disk is in the drive."); return false; }
    if (!image_path[0]) { snprintf(detail, detail_len, "This disk came from nowhere to put it back."); return false; }
    if (!image_dirty)   { snprintf(detail, detail_len, "Nothing has changed."); return true; }

    if (!storage_write(image_path, dsk_image, image_len)) {
        snprintf(detail, detail_len, "The card would not take it.");
        return false;
    }
    image_dirty = false;
    snprintf(detail, detail_len, "Saved %s", image_path);
    return true;
}

void fdc_eject(void) {
    mounted = false;
    image_dirty = false;
    image_path[0] = 0;
    image_len = 0;
    snprintf(last_cmd, sizeof last_cmd, "ejected");
}

/* The Track-Info header of a physical track, or NULL if there is none. */
static const uint8_t *track_hdr(const unsigned cyl, const unsigned head) {
    if (!mounted || cyl >= n_tracks || head >= n_sides) return NULL;
    const unsigned i = cyl * n_sides + head;
    const uint32_t off = track_off[i];
    if (!off) return NULL;
    if (memcmp(&dsk_image[off], "Track-Info", 10) != 0) return NULL;
    return &dsk_image[off];
}

/*
 * Find a sector by its recorded ID, which is what the controller does.
 *
 * Not by position: a protected disk renumbers its sectors and +3DOS
 * itself uses 0xC1.. on a 180K disk and 0x41.. on a 720K one, so the R
 * in the sector list is the only thing that identifies it.
 */
static bool find_sector(const unsigned cyl, const unsigned head, const uint8_t r,
                        uint32_t *data_off, uint32_t *data_len,
                        uint8_t *st1, uint8_t *st2, uint8_t *nsize) {
    const uint8_t *t = track_hdr(cyl, head);
    if (!t) return false;

    const uint8_t nsec = t[0x15];
    const uint8_t tn   = t[0x14];               /* the track's default N */
    uint32_t at = (uint32_t)(t - dsk_image) + TRK_HDR;

    for (unsigned i = 0; i < nsec; i++) {
        const uint8_t *si = &t[0x18 + i * 8];
        const uint8_t n = si[3];
        /*
         * An extended image records each sector's ACTUAL length, which
         * is how it stores a sector whose header lies about its size --
         * the standard one can only say 128<<N.
         */
        const uint32_t len = extended ? rd16(&si[6]) : (128u << (tn > 8 ? 8 : tn));
        if (si[2] == r) {
            if (at + len > image_len) return false;
            *data_off = at;
            *data_len = len;
            *st1 = si[4];
            *st2 = si[5];
            *nsize = n;
            return true;
        }
        at += len;
    }
    return false;
}

/* ── the controller ──────────────────────────────────────────────────── */
/*
 * The command table is pico-speccy's, from Upd765.cpp, unchanged: the
 * mask and value that identify a command byte, how many parameter bytes
 * follow it, and how many result bytes come back.
 *
 * The mask matters. A READ DATA is 0x06 but the top three bits are MT,
 * MF and SK -- multi-track, double density and skip-deleted -- so it is
 * matched under 0x1F and not compared for equality. Getting a mask
 * wrong makes one command answer for another, which +3DOS survives for
 * exactly as long as it takes to reach the one that differs.
 */
enum {
    C_READ_DATA = 0, C_READ_DIAG, C_RECALIBRATE, C_SEEK, C_WRITE_DATA,
    C_WRITE_ID, C_SCAN, C_READ_ID, C_SENSE_INT, C_SPECIFY, C_SENSE_DRIVE,
    C_VERSION, C_INVALID,
};

typedef struct { uint8_t id, mask, value, cmd_len, res_len; } upd_cmd_t;

static const upd_cmd_t kCmd[] = {
    { C_READ_DATA,   0x1f, 0x06, 8, 7 },
    { C_READ_DATA,   0x1f, 0x0c, 8, 7 },   /* read DELETED data */
    { C_READ_DIAG,   0x9f, 0x02, 8, 7 },
    { C_RECALIBRATE, 0xff, 0x07, 1, 0 },
    { C_SEEK,        0xff, 0x0f, 2, 0 },
    { C_WRITE_DATA,  0x3f, 0x05, 8, 7 },
    { C_WRITE_DATA,  0x3f, 0x09, 8, 7 },   /* write DELETED data */
    { C_WRITE_ID,    0xbf, 0x0d, 5, 7 },   /* FORMAT TRACK */
    { C_SCAN,        0x1f, 0x11, 8, 7 },   /* equal */
    { C_SCAN,        0x1f, 0x19, 8, 7 },   /* low or equal */
    { C_SCAN,        0x1f, 0x1d, 8, 7 },   /* high or equal */
    { C_READ_ID,     0xbf, 0x0a, 1, 7 },
    { C_SENSE_INT,   0xff, 0x08, 0, 2 },
    { C_SPECIFY,     0xff, 0x03, 2, 0 },
    { C_SENSE_DRIVE, 0xff, 0x04, 1, 1 },
    { C_VERSION,     0x1f, 0x10, 0, 1 },
    { C_INVALID,     0x00, 0x00, 0, 1 },
};
#define CMD_COUNT   (sizeof kCmd / sizeof kCmd[0])
#define CMD_INVALID (CMD_COUNT - 1)

/* Main status register bits, as the CPU reads them at #2FFD. */
#define MS_D0B  0x01    /* drive 0 seeking */
#define MS_D1B  0x02
#define MS_CB   0x10    /* controller busy */
#define MS_EXM  0x20    /* execution phase, non-DMA */
#define MS_DIO  0x40    /* 1 = controller to CPU */
#define MS_RQM  0x80    /* ready for a byte */

typedef enum { PH_IDLE = 0, PH_CMD, PH_EXEC, PH_RESULT } phase_t;

static struct {
    uint8_t main;               /* main status register */
    phase_t phase;
    uint8_t cmd_idx;
    uint8_t cmd_reg;            /* the command byte itself */
    /*
     * The parameter bytes, in pico-speccy's dataReg[] layout: [0] is
     * HD/US and then C H R N EOT GPL DTL. The command byte is NOT one of
     * them -- it has its own field above, because putting it here shifts
     * every index by one and the controller then reads R where it meant
     * to read C, which is a fault that looks exactly like a bad image.
     */
    uint8_t param[9];
    uint8_t n_param;
    uint8_t result[7];
    uint8_t n_result, res_at;

    uint8_t st0, st1, st2, st3;
    uint8_t pcn[2];             /* where each head physically is */
    uint8_t seek_done[2];       /* a SENSE INTERRUPT is owed for this drive */
    uint8_t us, hd;             /* unit and head from the last command */

    /* The execution phase: a window into dsk_image, walked a byte at a time. */
    uint32_t data_off, data_len, data_at;
    bool     writing;
    uint8_t  fmt_n, fmt_count, fmt_at;   /* FORMAT TRACK collects C H R N */
} f;

void fdc_reset(void) {
    memset(&f, 0, sizeof f);
    f.main = MS_RQM;
    f.phase = PH_IDLE;
    snprintf(last_cmd, sizeof last_cmd, "reset");
}

void fdc_stats(char *out, const unsigned len) {
    snprintf(out, len, "%s  %lu rd %lu wr %lu err", last_cmd,
             (unsigned long)n_reads, (unsigned long)n_writes,
             (unsigned long)n_errors);
}

/* Which command a byte is. Walks the table in order, as theirs does. */
static uint8_t cmd_identify(const uint8_t reg) {
    for (unsigned i = 0; i < CMD_COUNT; i++)
        if ((reg & kCmd[i].mask) == kCmd[i].value) return (uint8_t)i;
    return CMD_INVALID;
}

static void enter_result(const uint8_t n) {
    f.n_result = n;
    f.res_at = 0;
    if (!n) { f.phase = PH_IDLE; f.main = MS_RQM; return; }
    f.phase = PH_RESULT;
    f.main = MS_RQM | MS_DIO | MS_CB;
}

/* The seven-byte result every data command ends with. */
static void result_rw(void) {
    f.result[0] = f.st0;
    f.result[1] = f.st1;
    f.result[2] = f.st2;
    f.result[3] = f.param[0 + 1];   /* C */
    f.result[4] = f.param[0 + 2];   /* H */
    f.result[5] = f.param[0 + 3];   /* R */
    f.result[6] = f.param[0 + 4];   /* N */
    enter_result(7);
}

static bool drive_ready(void) {
    /* One motor line for both drives, from #1FFD bit 3. */
    return mounted && (spec_port_1ffd & 0x08u) && (f.us & 1u) == 0u;
}

static void abort_nr(void) {
    f.st0 = (uint8_t)(0x48u | (f.hd << 2) | (f.us & 3u));   /* AT + NR */
    f.st1 = f.st2 = 0;
    n_errors++;
    result_rw();
}

/* ── executing a command ─────────────────────────────────────────────── */

/*
 * Start a READ or WRITE. The sector is found by its recorded R, and the
 * window into the image is what the execution phase walks.
 */
static void start_data(const bool writing) {
    f.us = (uint8_t)(f.param[0] & 3u);
    f.hd = (uint8_t)((f.param[0] >> 2) & 1u);

    if (!drive_ready()) { abort_nr(); return; }

    uint8_t st1 = 0, st2 = 0, nsize = 0;
    if (!find_sector(f.param[1], f.hd, f.param[3],
                     &f.data_off, &f.data_len, &st1, &st2, &nsize)) {
        /* No such sector on this track: ND, and the CPU retries or gives up. */
        f.st0 = (uint8_t)(0x40u | (f.hd << 2) | f.us);
        f.st1 = 0x04u;                          /* ND */
        f.st2 = 0;
        n_errors++;
        result_rw();
        return;
    }

    /*
     * DTL shortens the transfer when N is zero, which is how a 128-byte
     * sector is read out of a track formatted for more. Theirs honours
     * it the same way and it is the one parameter easy to ignore and
     * hard to notice: +3DOS only uses it on the boot sector.
     */
    if (f.param[4] == 0 && f.param[7] && f.param[7] < f.data_len)
        f.data_len = f.param[7];

    f.data_at = 0;
    f.writing = writing;
    f.st0 = (uint8_t)((f.hd << 2) | f.us);
    f.st1 = (uint8_t)(st1 & 0x25u);             /* keep DE, ND, MA if recorded */
    f.st2 = (uint8_t)(st2 & 0x61u);             /* ...and CM, DD, MD */
    f.phase = PH_EXEC;
    f.main = (uint8_t)(MS_RQM | MS_CB | MS_EXM | (writing ? 0 : MS_DIO));

    if (writing) n_writes++; else n_reads++;
    snprintf(last_cmd, sizeof last_cmd, "%s C%u H%u R%02X",
             writing ? "write" : "read", f.param[1], f.hd, f.param[3]);
}

static void execute(void) {
    const upd_cmd_t *c = &kCmd[f.cmd_idx];

    switch (c->id) {
        case C_SPECIFY:
            /* Step rate and head-load times. Nothing here has a head to
             * load, so they are accepted and discarded, as theirs does
             * for the non-DMA bit it also cannot use. */
            enter_result(0);
            snprintf(last_cmd, sizeof last_cmd, "specify");
            break;

        case C_RECALIBRATE:
            f.us = (uint8_t)(f.param[0] & 3u);
            f.pcn[f.us & 1] = 0;
            f.st0 = (uint8_t)(0x20u | f.us);            /* seek end */
            if (!drive_ready()) f.st0 |= 0x48u;
            f.seek_done[f.us & 1] = 1;
            enter_result(0);
            snprintf(last_cmd, sizeof last_cmd, "recalibrate");
            break;

        case C_SEEK:
            f.us = (uint8_t)(f.param[0] & 3u);
            f.hd = (uint8_t)((f.param[0] >> 2) & 1u);
            f.pcn[f.us & 1] = f.param[1];
            f.st0 = (uint8_t)(0x20u | (f.hd << 2) | f.us);
            if (!drive_ready()) f.st0 |= 0x48u;
            f.seek_done[f.us & 1] = 1;
            enter_result(0);
            snprintf(last_cmd, sizeof last_cmd, "seek C%u", f.param[1]);
            break;

        case C_SENSE_INT:
            /*
             * The CPU asks what the last interrupt was about. A seek that
             * has finished answers once and then stops answering, which
             * is what tells +3DOS the drive has settled; with nothing
             * outstanding the controller reports an invalid command, and
             * that is how the boot ROM counts its drives.
             */
            if (f.seek_done[0]) {
                f.seek_done[0] = 0;
                f.result[0] = f.st0;
                f.result[1] = f.pcn[0];
                enter_result(2);
            } else if (f.seek_done[1]) {
                f.seek_done[1] = 0;
                f.result[0] = f.st0;
                f.result[1] = f.pcn[1];
                enter_result(2);
            } else {
                f.result[0] = 0x80u;                    /* invalid */
                enter_result(1);
            }
            break;

        case C_SENSE_DRIVE: {
            f.us = (uint8_t)(f.param[0] & 3u);
            f.hd = (uint8_t)((f.param[0] >> 2) & 1u);
            /* Two-sided, track 0 if the head is there, ready if a disk is. */
            f.st3 = (uint8_t)((f.hd << 2) | f.us | 0x08u);
            if (f.pcn[f.us & 1] == 0) f.st3 |= 0x10u;   /* T0 */
            if (drive_ready())        f.st3 |= 0x20u;   /* RDY */
            f.result[0] = f.st3;
            enter_result(1);
            break;
        }

        case C_VERSION:
            f.result[0] = 0x80u;                        /* a plain uPD765A */
            enter_result(1);
            break;

        case C_READ_ID: {
            f.us = (uint8_t)(f.param[0] & 3u);
            f.hd = (uint8_t)((f.param[0] >> 2) & 1u);
            if (!drive_ready()) { abort_nr(); break; }
            const uint8_t *t = track_hdr(f.pcn[f.us & 1], f.hd);
            if (!t || !t[0x15]) {
                f.st0 = (uint8_t)(0x40u | (f.hd << 2) | f.us);
                f.st1 = 0x01u;                          /* MA: no address mark */
                f.st2 = 0;
                n_errors++;
                result_rw();
                break;
            }
            /*
             * The first sector of the track will do. A real controller
             * returns whichever passes the head next, which is how some
             * protection measures rotation -- and is the part of this
             * that is modelled by result rather than by surface.
             */
            const uint8_t *si = &t[0x18];
            f.st0 = (uint8_t)((f.hd << 2) | f.us);
            f.st1 = f.st2 = 0;
            f.param[1] = si[0]; f.param[2] = si[1];
            f.param[3] = si[2]; f.param[4] = si[3];
            result_rw();
            snprintf(last_cmd, sizeof last_cmd, "read id");
            break;
        }

        case C_READ_DATA:  start_data(false); break;
        case C_WRITE_DATA: start_data(true);  break;

        case C_WRITE_ID:
            /*
             * FORMAT TRACK. The image's geometry is fixed by the file and
             * this cannot grow it, so the sectors are collected and the
             * existing ones are filled with the filler byte -- a format
             * of a disk that is already formatted, which is what +3DOS
             * does to a blank.
             */
            f.us = (uint8_t)(f.param[0] & 3u);
            f.hd = (uint8_t)((f.param[0] >> 2) & 1u);
            if (!drive_ready()) { abort_nr(); break; }
            f.fmt_n = f.param[1];
            f.fmt_count = f.param[2];
            f.fmt_at = 0;
            f.phase = PH_EXEC;
            f.main = MS_RQM | MS_CB | MS_EXM;
            f.writing = true;
            snprintf(last_cmd, sizeof last_cmd, "format %u sec", f.fmt_count);
            break;

        case C_READ_DIAG:
        case C_SCAN:
        default:
            /*
             * READ TRACK and the three SCANs. +3DOS issues none of them;
             * software that does gets a clean "invalid command" rather
             * than a controller that stops answering.
             */
            f.st0 = 0x80u;
            f.st1 = f.st2 = 0;
            n_errors++;
            result_rw();
            snprintf(last_cmd, sizeof last_cmd, "unsupported %02X", f.cmd_reg);
            break;
    }
}

/* ── the two ports ───────────────────────────────────────────────────── */
/*
 * #2FFD reads the main status register and #3FFD is the data register.
 *
 * Decoded on A15..A12 and A1, the way the +3's own gate array does:
 * both are A15=0, A14=0, A13=1, A1=0, with A12 telling them apart. A
 * narrower test catches ordinary #FD-family writes that were meant for
 * the AY.
 */
#define FDC_PORT_MASK   0xF002u
#define FDC_STATUS_PORT 0x2000u
#define FDC_DATA_PORT   0x3000u

/* The byte the CPU is handing over, in whatever phase we are in. */
bool __not_in_flash_func(fdc_port_out)(const uint16_t port, const uint8_t value) {
    if (spec_machine != MACHINE_PLUS3) return false;
    if ((port & FDC_PORT_MASK) != FDC_DATA_PORT) return false;

    if (f.phase == PH_EXEC && f.writing) {
        if (f.fmt_count) {
            /* FORMAT TRACK's C H R N, four bytes per sector. */
            if (++f.fmt_at >= 4) {
                f.fmt_at = 0;
                if (--f.fmt_count == 0) {
                    /* Fill every sector of the track with the filler. */
                    const uint8_t *t = track_hdr(f.pcn[f.us & 1], f.hd);
                    if (t) {
                        const uint8_t filler = t[0x17];
                        uint32_t at = (uint32_t)(t - dsk_image) + TRK_HDR;
                        const uint8_t nsec = t[0x15];
                        for (unsigned i = 0; i < nsec; i++) {
                            const uint8_t *si = &t[0x18 + i * 8];
                            const uint32_t len = extended ? rd16(&si[6])
                                                          : (128u << (t[0x14] > 8 ? 8 : t[0x14]));
                            if (at + len > image_len) break;
                            if (!settings.disk_wrprot) {
                                memset(&dsk_image[at], filler, len);
                                image_dirty = true;
                            }
                            at += len;
                        }
                    }
                    f.st0 = (uint8_t)((f.hd << 2) | f.us);
                    f.st1 = f.st2 = 0;
                    result_rw();
                }
            }
            return true;
        }

        if (!settings.disk_wrprot) {
            dsk_image[f.data_off + f.data_at] = value;
            image_dirty = true;
        }
        if (++f.data_at >= f.data_len) result_rw();
        return true;
    }

    if (f.phase == PH_IDLE) {
        /* The command byte: it decides how many parameters follow. */
        f.cmd_idx = cmd_identify(value);
        f.cmd_reg = value;
        f.n_param = 0;
        if (kCmd[f.cmd_idx].cmd_len == 0) { execute(); return true; }
        f.phase = PH_CMD;
        f.main = MS_RQM | MS_CB;
        return true;
    }

    if (f.phase == PH_CMD) {
        if (f.n_param < sizeof f.param) f.param[f.n_param++] = value;
        if (f.n_param >= kCmd[f.cmd_idx].cmd_len) execute();
        return true;
    }

    return true;        /* a byte in the result phase: the +3 ROM does this */
}

bool __not_in_flash_func(fdc_port_in)(const uint16_t port, uint8_t *value) {
    if (spec_machine != MACHINE_PLUS3) return false;

    if ((port & FDC_PORT_MASK) == FDC_STATUS_PORT) {
        *value = f.main;
        return true;
    }
    if ((port & FDC_PORT_MASK) != FDC_DATA_PORT) return false;

    if (f.phase == PH_EXEC && !f.writing) {
        *value = dsk_image[f.data_off + f.data_at];
        if (++f.data_at >= f.data_len) result_rw();
        return true;
    }

    if (f.phase == PH_RESULT) {
        *value = f.result[f.res_at];
        if (++f.res_at >= f.n_result) { f.phase = PH_IDLE; f.main = MS_RQM; }
        return true;
    }

    *value = 0xFF;
    return true;
}
