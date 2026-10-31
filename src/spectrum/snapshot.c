/*
 * frank-dino — an RP2350B acting as the whole machine around a real Z80
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-dino
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The .z80 v3 layout is Gerton Lunter's, as documented with Z80 and as
 * pico-speccy (https://github.com/drewpo28/pico-speccy, GPL-3.0) writes
 * it; quick-save slots after pico-speccy's persistSave/persistLoad.
 */

/*
 * Saving the running machine.
 *
 * The registers come from the NMI dump (see spectrum.c), the memory from
 * the banks directly, the paging latches and the AY's register file from
 * the port handlers' own copies. What is written is a .z80 version 3,
 * uncompressed -- the format every Spectrum emulator reads, and the one
 * this firmware's own loader already knows.
 */
#include "snapshot.h"

#include <stdio.h>
#include <string.h>

#include "ff.h"
#include "launch.h"
#include "sound.h"
#include "spectrum.h"

#define FAIL(...) do { snprintf(detail, detail_len, __VA_ARGS__); return false; } while (0)

z80_state_t snapshot_last;
bool        snapshot_last_valid;

/* The .z80 v3 hardware byte for each machine; 0xFF for none. */
static uint8_t hw_mode(void) {
    switch (spec_machine) {
        case MACHINE_48K:      return 0;
        case MACHINE_128K:     return 4;
        case MACHINE_PLUS2:    return 12;
        case MACHINE_PLUS3:    return 7;
        case MACHINE_PENTAGON: return 9;
        case MACHINE_SCORPION: return 10;
        case MACHINE_TIMEX:    return 15;
        case MACHINE_PLUS2A:   return 13;
        case MACHINE_TC2048:   return 14;
        case MACHINE_16K:
        case MACHINE_GAMA:
        case MACHINE_LENINGRAD1:
        case MACHINE_LENINGRAD2:
        case MACHINE_BYTE:     return 0;        // a 48K, as far as a .z80 can say
        default:               return 0xFF;
    }
}

static void put16(uint8_t *p, const uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }

static bool write_page(FIL *f, const uint8_t id, const uint8_t *data) {
    uint8_t h[3] = { 0xFF, 0xFF, id };           // 0xFFFF: not compressed
    UINT put = 0;
    if (f_write(f, h, 3, &put) != FR_OK || put != 3) return false;
    return f_write(f, data, SPEC_BANK_SIZE, &put) == FR_OK && put == SPEC_BANK_SIZE;
}

bool snapshot_save(const char *path, char *detail, const unsigned detail_len) {
    const uint8_t mode = hw_mode();
    if (mode == 0xFF) FAIL("%s snapshots are not supported yet", spec_machine_name());

    z80_state_t st;
    if (!spec_capture_state(&st)) FAIL("the CPU would not hand over its registers");
    snapshot_last = st;
    snapshot_last_valid = true;

    uint8_t h[32 + 55];
    memset(h, 0, sizeof h);
    h[0]  = (uint8_t)(st.af >> 8);
    h[1]  = (uint8_t)st.af;
    put16(h + 2, st.bc);
    put16(h + 4, st.hl);
    put16(h + 6, 0);                            // PC 0: version 2 or later
    put16(h + 8, st.sp);
    h[10] = st.i;
    h[11] = st.r & 0x7F;
    h[12] = (uint8_t)(((st.r >> 7) & 1u) | ((st.border & 7u) << 1));
    put16(h + 13, st.de);
    put16(h + 15, st.bc_);
    put16(h + 17, st.de_);
    put16(h + 19, st.hl_);
    h[21] = (uint8_t)(st.af_ >> 8);
    h[22] = (uint8_t)st.af_;
    put16(h + 23, st.iy);
    put16(h + 25, st.ix);
    h[27] = st.iff ? 1 : 0;
    h[28] = st.iff ? 1 : 0;
    h[29] = st.im & 3u;

    put16(h + 30, 55);                          // v3, with the 1FFD byte
    put16(h + 32, st.pc);
    h[34] = mode;
    h[35] = st.port7ffd;
    h[38] = snd_mirror.latched[0];
    memcpy(h + 39, snd_mirror.reg[0], 16);
    h[86] = st.port1ffd;

    FIL f;
    if (f_open(&f, path, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) FAIL("cannot create %s", path);
    UINT put = 0;
    bool ok = f_write(&f, h, sizeof h, &put) == FR_OK && put == sizeof h;

    if (spec_is_48k_class(spec_machine) || spec_machine == MACHINE_TIMEX) {
        // A 48K names its pages by address: 8 is 4000, 4 is 8000, 5 is C000.
        ok = ok && write_page(&f, 8, spec_bank_host(5))
                && write_page(&f, 4, spec_bank_host(2))
                && write_page(&f, 5, spec_bank_host(0));
    } else {
        const unsigned banks = spec_machine == MACHINE_SCORPION ? 16u : 8u;
        for (unsigned b = 0; ok && b < banks; b++)
            ok = write_page(&f, (uint8_t)(3u + b), spec_bank_host(b));
    }

    ok = (f_close(&f) == FR_OK) && ok;
    if (!ok) FAIL("could not write %s", path);
    snprintf(detail, detail_len, "Saved %s", path);
    return true;
}

static void slot_path(const unsigned slot, char *out, const unsigned n) {
    snprintf(out, n, "/snapshots/slot%u.z80", slot);
}

bool snapshot_quick_save(const unsigned slot, char *detail, const unsigned detail_len) {
    f_mkdir("/snapshots");
    char path[32];
    slot_path(slot, path, sizeof path);
    return snapshot_save(path, detail, detail_len);
}

bool snapshot_quick_exists(const unsigned slot) {
    char path[32];
    slot_path(slot, path, sizeof path);
    FILINFO fi;
    return f_stat(path, &fi) == FR_OK;
}

void snapshot_quick_path(const unsigned slot, char *out, const unsigned n) {
    slot_path(slot, out, n);
}

bool snapshot_save_new(char *detail, const unsigned detail_len) {
    f_mkdir("/snapshots");
    char path[32];
    FILINFO fi;
    for (unsigned n = 1; n < 10000; n++) {
        snprintf(path, sizeof path, "/snapshots/SNAP%04u.z80", n);
        if (f_stat(path, &fi) != FR_OK) break;
    }
    return snapshot_save(path, detail, detail_len);
}

bool snapshot_peek_registers(char *detail, const unsigned detail_len) {
    z80_state_t st;
    if (!spec_capture_state(&st)) FAIL("the CPU would not hand over its registers");
    snapshot_last = st;
    snapshot_last_valid = true;
    snprintf(detail, detail_len, "PC %04X SP %04X", st.pc, st.sp);
    return true;
}
