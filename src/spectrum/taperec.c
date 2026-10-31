/*
 * frank-dino — an RP2350B acting as the whole machine around a real Z80
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-dino
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * SAVE, heard rather than trapped.
 *
 * An emulator saves by catching the CPU at SA-BYTES and copying memory
 * out. There is no catching this CPU, so instead the recorder listens to
 * what SAVE actually does: toggle the MIC bit of port #FE, a pilot tone
 * and then two edges per bit. Core 1 already passes every change of that
 * bit to core 0 with its T-state, for the sound; the recorder takes the
 * same events and decodes the ROM's timings back into the bytes, a block
 * at a time, into a .tap that can be written to the card.
 *
 * Only the ROM's timings are understood -- 2168 for the pilot, 855 and
 * 1710 for the bits -- with a generous window either side, because that
 * is what SAVE and every program that calls SA-BYTES produce. A turbo
 * saver's output is noted as noise and skipped.
 */
#include "taperec.h"

#include <stdio.h>
#include <string.h>

#ifndef TAPEREC_HOST_TEST
#include "ff.h"
#define PSRAM_BSS __attribute__((section(".psram")))
#else
#define PSRAM_BSS
#endif

#define REC_MAX (256u * 1024u)
static uint8_t  rec[REC_MAX] PSRAM_BSS;
static uint32_t rec_len, rec_blocks;

/* The block being heard. Its length goes in front when it ends. */
#define BLOCK_MAX 65536u
static uint8_t  blk[BLOCK_MAX] PSRAM_BSS;
static uint32_t blk_len;

typedef enum : uint8_t { R_IDLE, R_PILOT, R_SYNC2, R_DATA } rstate_t;
static rstate_t st;
static uint64_t last_edge;
static uint32_t pilot_n;
static uint32_t half;           // the first half of a bit, waiting for the second
static uint32_t bits;
static uint8_t  byte;

/* Windows around the ROM's own numbers. */
static inline bool is_pilot(const uint32_t h) { return h > 1700u && h < 2700u; }
static inline bool is_sync(const uint32_t h)  { return h > 350u  && h < 1100u; }

static void finish_block(void) {
    if (blk_len >= 2 && rec_len + 2 + blk_len <= REC_MAX) {
        rec[rec_len++] = (uint8_t)blk_len;
        rec[rec_len++] = (uint8_t)(blk_len >> 8);
        memcpy(&rec[rec_len], blk, blk_len);
        rec_len += blk_len;
        rec_blocks++;
    }
    blk_len = 0;
    st = R_IDLE;
    pilot_n = 0;
}

void taperec_edge(const uint64_t t) {
    const uint64_t d = t - last_edge;
    last_edge = t;
    const uint32_t h = d > 0xFFFFFFu ? 0xFFFFFFu : (uint32_t)d;

    switch (st) {
        case R_IDLE:
            pilot_n = is_pilot(h) ? pilot_n + 1u : 0u;
            if (pilot_n >= 256u) st = R_PILOT;
            break;
        case R_PILOT:
            if (is_pilot(h)) break;
            st = is_sync(h) ? R_SYNC2 : R_IDLE;
            pilot_n = 0;
            break;
        case R_SYNC2:
            if (is_sync(h)) { st = R_DATA; half = 0; bits = 0; byte = 0; blk_len = 0; }
            else st = R_IDLE;
            break;
        case R_DATA:
            if (!half) {
                if (h > 2600u) { finish_block(); break; }
                half = h;
                break;
            }
            {
                const uint32_t total = half + h;
                half = 0;
                uint32_t bit;
                if (total > 1100u && total < 2565u)      bit = 0;
                else if (total >= 2565u && total < 4600u) bit = 1;
                else { finish_block(); break; }
                byte = (uint8_t)((byte << 1) | bit);
                if (++bits == 8) {
                    if (blk_len < BLOCK_MAX) blk[blk_len++] = byte;
                    bits = 0;
                    byte = 0;
                }
            }
            break;
    }
}

void taperec_poll(const uint64_t now) {
    // A tenth of a second of silence ends a block that ended cleanly.
    if (st == R_DATA && now - last_edge > 350000u) finish_block();
}

uint32_t taperec_blocks(void) { return rec_blocks; }
uint32_t taperec_bytes(void)  { return rec_len; }

void taperec_clear(void) {
    rec_len = 0;
    rec_blocks = 0;
    blk_len = 0;
    st = R_IDLE;
    pilot_n = 0;
}

const uint8_t *taperec_data(void) { return rec; }

#ifndef TAPEREC_HOST_TEST
bool taperec_write(char *path, const unsigned path_len) {
    if (!rec_len) return false;
    f_mkdir("/tapes");
    FILINFO fi;
    for (unsigned n = 1; n < 10000; n++) {
        snprintf(path, path_len, "/tapes/SAVE%04u.tap", n);
        if (f_stat(path, &fi) != FR_OK) break;
    }
    FIL f;
    if (f_open(&f, path, FA_WRITE | FA_CREATE_NEW) != FR_OK) return false;
    UINT put = 0;
    const bool ok = f_write(&f, rec, rec_len, &put) == FR_OK && put == rec_len;
    return f_close(&f) == FR_OK && ok;
}
#endif
