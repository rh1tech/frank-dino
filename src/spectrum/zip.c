/*
 * frank-dino — an RP2350B acting as the whole machine around a real Z80
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-dino
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "zip.h"

#include <stdio.h>
#include <string.h>

#include "ff.h"
#include "miniz.h"

/* Their LocalFileHeader, read field by field rather than as a packed
 * struct: this is a little-endian file format and this is a
 * little-endian machine, but one of those is a property of the format
 * and the other is not. */
#define ZIP_LOCAL_SIGNATURE 0x04034B50u

/*
 * The inflate state and the window, both over in the PSRAM.
 *
 * tinfl_decompressor is about eleven kilobytes and the dictionary is
 * thirty-two more. Theirs borrows them from the Gigascreen framebuffer
 * when the heap is tight; here there is no heap to be tight and forty
 * kilobytes of SRAM does not exist, so they live where the tape and the
 * disk images already do. Nothing touches them with the Z80 clocked.
 */
static tinfl_decompressor inflator __attribute__((section(".psram")));
static uint8_t            dict[TINFL_LZ_DICT_SIZE] __attribute__((section(".psram")));
static uint8_t            inbuf[2048] __attribute__((section(".psram")));

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* ── listing ─────────────────────────────────────────────────────────── */

int zip_list(const char *path, zip_member_t *out, const int max) {
    FIL f;
    if (f_open(&f, path, FA_READ) != FR_OK) return -1;

    int n = 0;
    FSIZE_t at = 0;
    bool any_signature = false;

    while (n < max && n < ZIP_MAX_MEMBERS) {
        uint8_t h[30];
        UINT got = 0;
        if (f_lseek(&f, at) != FR_OK) break;
        if (f_read(&f, h, sizeof h, &got) != FR_OK || got != sizeof h) break;
        if (rd32(h) != ZIP_LOCAL_SIGNATURE) break;     /* end of the local headers */
        any_signature = true;

        const uint16_t flags    = rd16(h + 6);
        const uint32_t csize    = rd32(h + 18);
        const uint32_t usize    = rd32(h + 22);
        const uint16_t name_len = rd16(h + 26);
        const uint16_t extra    = rd16(h + 28);

        char name[STORAGE_MAX_NAME];
        const UINT want = name_len < sizeof name - 1 ? name_len : sizeof name - 1;
        if (f_read(&f, name, want, &got) != FR_OK || got != want) break;
        name[want] = 0;
        if (name_len >= sizeof name) f_lseek(&f, f_tell(&f) + (name_len - want));

        const FSIZE_t data = f_tell(&f) + extra;

        /*
         * Bit 3 says the sizes are in a data descriptor after the data,
         * which a walk of local headers cannot find without inflating
         * the member to get there. Theirs refuses these too.
         */
        const bool sizes_known = !(flags & 0x0008);

        /* A directory entry is a name ending in '/', with no data. */
        const char *base = strrchr(name, '/');
        base = base ? base + 1 : name;
        if (*base && sizes_known) {
            const file_kind_t k = storage_kind(base);
            if (k != FILE_OTHER && k != FILE_DIR) {
                snprintf(out[n].name, sizeof out[n].name, "%s", base);
                out[n].size   = usize;
                out[n].kind   = k;
                out[n].offset = (uint32_t)at;
                n++;
            }
        }

        at = data + csize;
        if (!sizes_known) break;            /* nowhere safe to go next */
    }

    f_close(&f);
    return any_signature ? n : -1;
}

/* ── extraction ──────────────────────────────────────────────────────── */

static bool copy_stored(FIL *in, FIL *out, uint32_t left) {
    while (left) {
        const UINT want = left < sizeof inbuf ? (UINT)left : (UINT)sizeof inbuf;
        UINT got = 0, put = 0;
        if (f_read(in, inbuf, want, &got) != FR_OK || got != want) return false;
        if (f_write(out, inbuf, got, &put) != FR_OK || put != got) return false;
        left -= got;
    }
    return true;
}

/*
 * Raw deflate through tinfl, in their shape: feed the input a block at a
 * time, let it write into a 32 KB ring, and flush whatever came out.
 *
 * The ring is the dictionary -- inflate needs the last 32 KB of output
 * to resolve back-references, so the output buffer and the history are
 * the same memory and the flush has to happen at the wrap.
 */
static bool copy_deflate(FIL *in, FIL *out, uint32_t left) {
    tinfl_init(&inflator);
    size_t dict_ofs = 0;
    UINT have = 0, used = 0;

    for (;;) {
        if (used == have) {
            if (!left) { have = used = 0; }
            else {
                const UINT want = left < sizeof inbuf ? (UINT)left : (UINT)sizeof inbuf;
                if (f_read(in, inbuf, want, &have) != FR_OK || have != want) return false;
                left -= have;
                used = 0;
            }
        }

        size_t in_bytes  = have - used;
        size_t out_bytes = TINFL_LZ_DICT_SIZE - dict_ofs;
        const mz_uint32 flags = (left ? TINFL_FLAG_HAS_MORE_INPUT : 0);

        const tinfl_status st = tinfl_decompress(&inflator, inbuf + used, &in_bytes,
                                                 dict, dict + dict_ofs, &out_bytes,
                                                 flags);
        used += (UINT)in_bytes;

        if (out_bytes) {
            UINT put = 0;
            if (f_write(out, dict + dict_ofs, (UINT)out_bytes, &put) != FR_OK
                || put != out_bytes) return false;
            dict_ofs = (dict_ofs + out_bytes) & (TINFL_LZ_DICT_SIZE - 1);
        }

        if (st == TINFL_STATUS_DONE) return true;
        if (st < TINFL_STATUS_DONE)  return false;      /* FAILED / BAD_PARAM / ADLER */
        if (st == TINFL_STATUS_NEEDS_MORE_INPUT && !left && used == have) return false;
    }
}

bool zip_extract(const char *path, const zip_member_t *m,
                 char *detail, const unsigned detail_len) {
    FIL in, out;
    if (f_open(&in, path, FA_READ) != FR_OK) {
        snprintf(detail, detail_len, "Could not open the archive.");
        return false;
    }

    uint8_t h[30];
    UINT got = 0;
    if (f_lseek(&in, m->offset) != FR_OK
        || f_read(&in, h, sizeof h, &got) != FR_OK || got != sizeof h
        || rd32(h) != ZIP_LOCAL_SIGNATURE) {
        f_close(&in);
        snprintf(detail, detail_len, "The archive moved under us.");
        return false;
    }

    const uint16_t method = rd16(h + 8);
    const uint32_t csize  = rd32(h + 18);
    const uint16_t nlen   = rd16(h + 26);
    const uint16_t extra  = rd16(h + 28);

    if (method != 0 && method != 8) {
        f_close(&in);
        /* The only two the format promised; everything else is an
         * extension somebody's archiver chose and this cannot read. */
        snprintf(detail, detail_len, "%s uses compression method %u.", m->name, method);
        return false;
    }

    if (f_lseek(&in, m->offset + sizeof h + nlen + extra) != FR_OK) {
        f_close(&in);
        snprintf(detail, detail_len, "The archive is truncated.");
        return false;
    }

    if (f_open(&out, ZIP_TEMP_PATH, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) {
        f_close(&in);
        snprintf(detail, detail_len, "The card would not take a temporary file.");
        return false;
    }

    const bool ok = (method == 0) ? copy_stored(&in, &out, csize)
                                  : copy_deflate(&in, &out, csize);

    f_close(&out);
    f_close(&in);

    if (!ok) {
        f_unlink(ZIP_TEMP_PATH);
        snprintf(detail, detail_len, "%s would not unpack.", m->name);
        return false;
    }
    snprintf(detail, detail_len, "%s", m->name);
    return true;
}

/* ── gzip ────────────────────────────────────────────────────────────── */

bool zip_gunzip(const char *path, file_kind_t *kind, char *detail, const unsigned detail_len) {
    // What it will be is the name without the .gz.
    char inner[STORAGE_MAX_NAME];
    const char *slash = strrchr(path, '/');
    snprintf(inner, sizeof inner, "%s", slash ? slash + 1 : path);
    const size_t n = strlen(inner);
    if (n > 3) inner[n - 3] = 0;
    *kind = storage_kind(inner);
    if (*kind == FILE_OTHER || *kind == FILE_GZ || *kind == FILE_ZIP) {
        snprintf(detail, detail_len, "%s is nothing this can open.", inner);
        return false;
    }

    FIL in, out;
    if (f_open(&in, path, FA_READ) != FR_OK) {
        snprintf(detail, detail_len, "Could not open the file.");
        return false;
    }
    const uint32_t size = (uint32_t)f_size(&in);

    uint8_t h[10];
    UINT got = 0;
    if (f_read(&in, h, sizeof h, &got) != FR_OK || got != sizeof h
        || h[0] != 0x1F || h[1] != 0x8B || h[2] != 8) {
        f_close(&in);
        snprintf(detail, detail_len, "That is not a gzip file.");
        return false;
    }

    // The optional fields, in the order RFC 1952 puts them.
    uint32_t at = 10;
    const uint8_t flg = h[3];
    if (flg & 0x04) {                               // FEXTRA
        uint8_t x[2];
        f_read(&in, x, 2, &got);
        at += 2u + (uint32_t)x[0] + ((uint32_t)x[1] << 8);
    }
    f_lseek(&in, at);
    for (int field = 0; field < 2; field++) {       // FNAME, then FCOMMENT
        if (!(flg & (field ? 0x10 : 0x08))) continue;
        uint8_t c = 1;
        while (c && f_read(&in, &c, 1, &got) == FR_OK && got == 1) at++;
    }
    if (flg & 0x02) at += 2;                        // FHCRC
    f_lseek(&in, at);

    // Eight bytes of CRC and length at the end are not deflate.
    if (size < at + 8) {
        f_close(&in);
        snprintf(detail, detail_len, "The gzip file is truncated.");
        return false;
    }

    if (f_open(&out, ZIP_TEMP_PATH, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) {
        f_close(&in);
        snprintf(detail, detail_len, "The card would not take a temporary file.");
        return false;
    }
    const bool ok = copy_deflate(&in, &out, size - at - 8);
    f_close(&out);
    f_close(&in);

    if (!ok) {
        f_unlink(ZIP_TEMP_PATH);
        snprintf(detail, detail_len, "%s would not unpack.", inner);
        return false;
    }
    return true;
}
