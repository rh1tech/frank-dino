/*
 * frank-dino — an RP2350B acting as the whole machine around a real Z80
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-dino
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * The microSD card, as a file browser wants to see it.
 *
 * Everything here runs on core 0 and only on core 0. FatFs is not
 * re-entrant across cores, and the Z80 does not stop of its own accord —
 * so the rule is that the machine is paused whenever the filesystem is
 * touched, which is the same moment a browser is on the screen anyway.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

// Long filenames are on in the vendored ffconf.h, and Spectrum software
// is full of them. 64 is longer than the screen can show and shorter than
// anything worth truncating.
#define STORAGE_MAX_NAME 64
#define STORAGE_MAX_PATH 256

typedef enum : uint8_t {
    FILE_OTHER = 0,
    FILE_DIR,
    FILE_TAP,          // tape image
    FILE_TZX,          // tape image, the richer format
    FILE_PZX,          // tape image, the one that describes pulses
    FILE_SNA,          // snapshot
    FILE_Z80,          // snapshot, the common one
    FILE_TRD,          // TR-DOS disk image
    FILE_SCL,          // the same disk with the empty space left out
    FILE_SCR,          // a bare screen dump
    FILE_DSK,          // a +3 disk image
    FILE_DCK,          // a Timex dock cartridge
    FILE_MMC,          // an esxDOS card image (.mmc / .hdf)
    FILE_ZIP,          // an archive; what is inside decides what it is
    FILE_CSW,          // tape image: a sampled signal, run-length coded
    FILE_WAV,          // tape image: a recording of a real cassette
    FILE_GZ,           // one file, gzipped; the name under the .gz decides
    FILE_ROM,          // an Interface 2 cartridge
    FILE_FDI,          // a TR-DOS disk as a Full Disk Image
    FILE_UDI,          // ...as an Ultra Disk Image (raw MFM tracks)
    FILE_TD0,          // ...as a Teledisk image
} file_kind_t;

typedef struct {
    char        name[STORAGE_MAX_NAME];
    uint32_t    size;
    file_kind_t kind;
} storage_entry_t;

bool storage_mount(void);
bool storage_mounted(void);

/*
 * List a directory into `out`, at most `max` entries.
 *
 * Sorted with directories first and then by name, because a browser that
 * shows a card's directory in whatever order FAT happens to store it is
 * a browser nobody can find anything in. Returns the number written, or
 * -1 if the directory could not be opened.
 *
 * Entries this firmware cannot do anything with are included rather than
 * hidden: a card with one unrecognised file on it should look like a
 * card with one unrecognised file on it, not like an empty one.
 */
int storage_list(const char *dir, storage_entry_t *out, int max);

/* What the extension says a file is. */
file_kind_t storage_kind(const char *name);

/*
 * Write `len` bytes over `path`, replacing whatever was there.
 *
 * Used to put a disk image back after the guest has written to it. Truncates
 * on failure rather than leaving a half-written file looking whole: a .trd
 * that is the right length and the wrong contents is worse than one that is
 * obviously broken.
 */
bool storage_write(const char *path, const void *buf, uint32_t len);

/* Read a whole file into `buf`. Returns bytes read, or -1. */
int32_t storage_read(const char *path, void *buf, uint32_t max);

/* Size of a file, or -1 if it is not there. */
int32_t storage_size(const char *path);
