/*
 * frank-dino — an RP2350B acting as the whole machine around a real Z80
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-dino
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "storage.h"

#include <stdio.h>
#include <string.h>

#include "ff.h"

static FATFS fs;
static bool  mounted;

bool storage_mounted(void) { return mounted; }

bool storage_mount(void) {
    mounted = (f_mount(&fs, "", 1) == FR_OK);
    return mounted;
}

// ---------------------------------------------------------------------------

static bool ext_is(const char *name, const char *ext) {
    const size_t n = strlen(name), e = strlen(ext);
    if (n < e + 1 || name[n - e - 1] != '.') return false;
    for (size_t i = 0; i < e; i++) {
        char a = name[n - e + i];
        if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
        if (a != ext[i]) return false;
    }
    return true;
}

file_kind_t storage_kind(const char *name) {
    if (ext_is(name, "tap")) return FILE_TAP;
    if (ext_is(name, "tzx")) return FILE_TZX;
    if (ext_is(name, "pzx")) return FILE_PZX;
    if (ext_is(name, "csw")) return FILE_CSW;
    if (ext_is(name, "wav")) return FILE_WAV;
    if (ext_is(name, "gz"))  return FILE_GZ;
    if (ext_is(name, "rom")) return FILE_ROM;
    if (ext_is(name, "fdi")) return FILE_FDI;
    if (ext_is(name, "udi")) return FILE_UDI;
    if (ext_is(name, "td0")) return FILE_TD0;
    if (ext_is(name, "sna")) return FILE_SNA;
    if (ext_is(name, "z80")) return FILE_Z80;
    if (ext_is(name, "trd")) return FILE_TRD;
    if (ext_is(name, "scl")) return FILE_SCL;
    if (ext_is(name, "scr")) return FILE_SCR;
    if (ext_is(name, "dsk")) return FILE_DSK;
    if (ext_is(name, "dck")) return FILE_DCK;
    if (ext_is(name, "mmc")) return FILE_MMC;
    if (ext_is(name, "hdf")) return FILE_MMC;
    if (ext_is(name, "zip")) return FILE_ZIP;
    return FILE_OTHER;
}

// ---------------------------------------------------------------------------

/*
 * Insertion sort, directories first then by name, case-insensitively.
 *
 * Insertion rather than anything cleverer because the list is at most a
 * few hundred entries and it is sorted once per directory change, on a
 * core that is otherwise idle. Case-insensitively because a card written
 * on a PC has MANIC.TAP next to jetpac.tap and a browser that separates
 * them by case is a browser that looks broken.
 */
static int cmp(const storage_entry_t *a, const storage_entry_t *b) {
    if ((a->kind == FILE_DIR) != (b->kind == FILE_DIR))
        return a->kind == FILE_DIR ? -1 : 1;

    const char *p = a->name, *q = b->name;
    for (;; p++, q++) {
        char x = *p, y = *q;
        if (x >= 'A' && x <= 'Z') x = (char)(x + 32);
        if (y >= 'A' && y <= 'Z') y = (char)(y + 32);
        if (x != y) return x < y ? -1 : 1;
        if (!x) return 0;
    }
}

int storage_list(const char *dir, storage_entry_t *out, const int max) {
    if (max <= 0) return -1;
    /*
     * The card has to be mounted for its own volume, but not for the
     * USB stick's: a board with no card in it should still browse a
     * stick, and f_opendir knows which volume "1:/" means.
     */
    if (!mounted && (dir[0] != '1' || dir[1] != ':')) return -1;

    DIR d;
    if (f_opendir(&d, dir) != FR_OK) return -1;

    int n = 0;
    FILINFO fi;
    while (n < max && f_readdir(&d, &fi) == FR_OK && fi.fname[0]) {
        // Hidden and system entries are FAT bookkeeping far more often
        // than they are anything a person put there on purpose.
        if (fi.fattrib & (AM_HID | AM_SYS)) continue;

        storage_entry_t *e = &out[n];
        // Bounded copy with an explicit terminator. strncpy does not
        // terminate when it truncates and snprintf warns about the same
        // case; FatFs's long-name buffer is 255 bytes and this is 64, so
        // truncation is normal rather than exceptional.
        const size_t len = strnlen(fi.fname, STORAGE_MAX_NAME - 1);
        memcpy(e->name, fi.fname, len);
        e->name[len] = 0;
        e->size = (uint32_t)fi.fsize;
        e->kind = (fi.fattrib & AM_DIR) ? FILE_DIR : storage_kind(fi.fname);
        n++;
    }
    f_closedir(&d);

    for (int i = 1; i < n; i++) {
        const storage_entry_t key = out[i];
        int j = i - 1;
        while (j >= 0 && cmp(&out[j], &key) > 0) { out[j + 1] = out[j]; j--; }
        out[j + 1] = key;
    }
    return n;
}

int32_t storage_size(const char *path) {
    FILINFO fi;
    if (!mounted || f_stat(path, &fi) != FR_OK) return -1;
    return (int32_t)fi.fsize;
}

bool storage_write(const char *path, const void *buf, const uint32_t len) {
    FIL f;
    if (f_open(&f, path, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) return false;

    /*
     * In chunks, because FatFs takes a UINT and a Profi-sized image is
     * comfortably past what one call should be asked to do in one go on
     * a card that may be slow.
     */
    const uint8_t *p = (const uint8_t *)buf;
    uint32_t left = len;
    bool ok = true;
    while (left && ok) {
        const UINT want = left > 32768u ? 32768u : (UINT)left;
        UINT put = 0;
        ok = (f_write(&f, p, want, &put) == FR_OK && put == want);
        p += put;
        left -= put;
    }
    ok = (f_close(&f) == FR_OK) && ok;
    if (!ok) f_unlink(path);
    return ok;
}

int32_t storage_read(const char *path, void *buf, const uint32_t max) {
    if (!mounted) return -1;

    FIL f;
    if (f_open(&f, path, FA_READ) != FR_OK) return -1;

    UINT got = 0;
    const FRESULT r = f_read(&f, buf, max, &got);
    f_close(&f);

    return (r == FR_OK) ? (int32_t)got : -1;
}
