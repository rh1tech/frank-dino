/*
 * frank-dino — an RP2350B acting as the whole machine around a real Z80
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-dino
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * A machine's ROM from the card.
 *
 * The ROM windows are refilled from flash on every reset (spectrum.c's
 * rom_load_window), so a ROM on the card is just the other place to fill
 * them from: /roms/<tag>.rom, exactly the machine's size -- 16 KB for the
 * 48K family, 32 KB for the 128K-shaped ones (editor first, then BASIC),
 * 64 KB for the +3 and +2A -- and /roms/trdos.rom for TR-DOS. Nothing is
 * written to flash; take the file away and the next reset is stock.
 * pico-speccy flashes a custom ROM into a reserved slot and reboots
 * (OSD::updateROM); this board can afford to read it each time instead.
 */
#include "customrom.h"

#include <stdio.h>

#include "ff.h"

bool customrom_active, customrom_trdos_active;

const char *customrom_tag(const machine_t m, unsigned *banks) {
    *banks = 1;
    switch (m) {
        case MACHINE_48K:        return "48";
        case MACHINE_16K:        return "16";
        case MACHINE_TC2048:     return "tc2048";
        case MACHINE_GAMA:       return "gama";
        case MACHINE_LENINGRAD1: return "leningrad1";
        case MACHINE_LENINGRAD2: return "leningrad2";
        case MACHINE_BYTE:       return "byte";
        default: break;
    }
    *banks = 2;
    switch (m) {
        case MACHINE_128K:     return "128";
        case MACHINE_PLUS2:    return "plus2";
        case MACHINE_PENTAGON: return "pentagon";
        case MACHINE_PENT512:  return "pentagon512";
        case MACHINE_PENT1024: return "pentagon1024";
        default: break;
    }
    *banks = 4;
    switch (m) {
        case MACHINE_PLUS3:  return "plus3";
        case MACHINE_PLUS2A: return "plus2a";
        default: break;
    }
    *banks = 0;
    return NULL;
}

static bool read_exact(const char *path, uint8_t *const *slot, const unsigned banks) {
    FILINFO fi;
    if (f_stat(path, &fi) != FR_OK || fi.fsize != (FSIZE_t)banks * SPEC_BANK_SIZE) return false;
    FIL f;
    if (f_open(&f, path, FA_READ) != FR_OK) return false;
    bool ok = true;
    for (unsigned i = 0; i < banks && ok; i++) {
        UINT got = 0;
        ok = f_read(&f, slot[i], SPEC_BANK_SIZE, &got) == FR_OK && got == SPEC_BANK_SIZE;
    }
    f_close(&f);
    return ok;
}

bool customrom_load(const machine_t m, uint8_t *const *slot, const unsigned banks) {
    unsigned want;
    const char *tag = customrom_tag(m, &want);
    if (!tag || want != banks) return false;
    char path[40];
    snprintf(path, sizeof path, "/roms/%s.rom", tag);
    return read_exact(path, slot, banks);
}

bool customrom_trdos(uint8_t *dst) {
    uint8_t *const one[1] = { dst };
    return read_exact("/roms/trdos.rom", one, 1);
}
