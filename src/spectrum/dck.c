/*
 * frank-dino — an RP2350B acting as the whole machine around a real Z80
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-dino
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "dck.h"

#include <stdio.h>
#include <string.h>

#include "ff.h"
#include "spectrum.h"

/*
 * Where the cartridge lives: in the guest RAM banks a Timex does not have.
 *
 * It has to be in SRAM -- the pointers go into the bus engine's page
 * table and the CPU fetches opcodes straight out of them, so a
 * cartridge in the PSRAM would put an XIP miss in the middle of an
 * instruction fetch. But sixty-four kilobytes of its own would be an
 * eighth of the chip for something most boards never plug in.
 *
 * A TC2068 is a 48K machine. It uses banks 5, 2 and 0 and nothing else,
 * so four of the eight 16 KB banks sit idle for as long as that machine
 * is selected -- which is exactly the sixty-four kilobytes a dock
 * cartridge can be. The cost is that leaving the Timex ejects the
 * cartridge, because those banks become somebody's RAM again.
 */
static const uint8_t kHostBank[DCK_CHUNKS / 2] = { 1, 3, 4, 6 };

static uint8_t *chunk_at(const unsigned i) {
    return spec_ram[kHostBank[i >> 1]] + (i & 1u) * DCK_CHUNK;
}

static uint8_t present;         /* bitmap: this chunk is filled */
static uint8_t writable;        /* bitmap: ...and it is RAM */

bool     dck_mounted(void) { return present != 0; }
uint8_t  dck_present(void) { return present; }
bool     dck_writable(const unsigned i) { return i < DCK_CHUNKS && (writable & (1u << i)); }

/*
 * Always a page, never NULL.
 *
 * A chunk the cartridge does not fill has to read as an open bus, and
 * that used to be a separate 8 KB of 0xFF in the SRAM. It does not need
 * to be: the host bank for that chunk is sitting there unused, so it
 * holds the 0xFF instead. One fewer special case in apply_paging(), and
 * eight kilobytes back.
 */
uint8_t *dck_chunk(const unsigned i) {
    if (i >= DCK_CHUNKS) return NULL;
    return chunk_at(i);
}

/* Fill the chunks nothing has claimed with what an empty socket reads. */
static void fill_open(const uint8_t claimed) {
    for (unsigned i = 0; i < DCK_CHUNKS; i++)
        if (!(claimed & (1u << i))) memset(chunk_at(i), 0xFF, DCK_CHUNK);
}

void dck_eject(void) {
    present = 0;
    writable = 0;
    fill_open(0);
}

bool dck_mount(const char *path, char *detail, const unsigned detail_len) {
    dck_eject();

    FIL f;
    if (f_open(&f, path, FA_READ) != FR_OK) {
        snprintf(detail, detail_len, "cannot open that file");
        return false;
    }
    const uint32_t size = (uint32_t)f_size(&f);

    /*
     * Walk the blocks looking for bank 0. Theirs does the same, and
     * skips a block by counting only the chunks that are IN THE FILE --
     * a type-1 chunk is RAM the file does not carry and occupies no
     * space, so adding it to the stride would desynchronise everything
     * after it.
     */
    uint8_t  types[DCK_CHUNKS] = { 0 };
    uint32_t data_off[DCK_CHUNKS] = { 0 };
    uint8_t  found = 0, in_file = 0;
    bool     ok = false;

    for (uint32_t off = 0; off + 9 <= size; ) {
        uint8_t h[9];
        UINT got = 0;
        if (f_lseek(&f, off) != FR_OK || f_read(&f, h, 9, &got) != FR_OK || got != 9) break;

        unsigned n_in_file = 0;
        for (unsigned i = 0; i < DCK_CHUNKS; i++) if (h[1 + i] & 2) n_in_file++;

        if (h[0] == 0 && !ok) {                 /* bank 0 = the dock */
            uint32_t src = off + 9;
            uint8_t p = 0, w = 0, inf = 0;
            for (unsigned i = 0; i < DCK_CHUNKS; i++) {
                const uint8_t t = h[1 + i];
                types[i] = t;
                if (t & 3) p |= (uint8_t)(1u << i);
                if (t & 1) w |= (uint8_t)(1u << i);
                if (t & 2) { inf |= (uint8_t)(1u << i); data_off[i] = src; src += DCK_CHUNK; }
            }
            /*
             * A block whose data runs off the end of the file is a
             * truncated download, not a cartridge. Refuse it here
             * rather than reading short chunks later.
             */
            if (src <= size && p) {
                found = p; writable = w; in_file = inf; ok = true;
            }
        }

        off += 9u + (uint32_t)n_in_file * DCK_CHUNK;
    }

    if (!ok) {
        f_close(&f);
        snprintf(detail, detail_len, "not a DOCK cartridge");
        return false;
    }

    for (unsigned i = 0; i < DCK_CHUNKS; i++) {
        if (!(found & (1u << i))) continue;
        if (in_file & (1u << i)) {
            UINT got = 0;
            if (f_lseek(&f, data_off[i]) != FR_OK
                || f_read(&f, chunk_at(i), DCK_CHUNK, &got) != FR_OK || got != DCK_CHUNK) {
                f_close(&f);
                dck_eject();
                snprintf(detail, detail_len, "truncated file");
                return false;
            }
        } else {
            memset(chunk_at(i), 0, DCK_CHUNK);  /* type 1: RAM the file does not carry */
        }
    }
    f_close(&f);

    present = found;
    fill_open(found);
    unsigned kb = 0;
    for (unsigned i = 0; i < DCK_CHUNKS; i++) if (present & (1u << i)) kb += 8;
    snprintf(detail, detail_len, "%u KB in %u chunk%s", kb,
             kb / 8, kb == 8 ? "" : "s");
    (void)types;
    return true;
}
