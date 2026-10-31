/*
 * frank-dino — an RP2350B acting as the whole machine around a real Z80
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-dino
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Ported from pico-speccy's machines/Atm.{h,cpp}
 * (https://github.com/drewpo28/pico-speccy, GPL-3.0) by drew.po28, which
 * reads its hardware out of UnrealSpeccy (memory.cpp MM_ATM450 / MM_ATM710,
 * io.cpp, atm.cpp) and MAME's sinclair/atm.cpp.
 */

/*
 * MicroART ATM-Turbo 1, 2 and 2+, and NedoPC's ATM-Turbo 3.
 *
 * A memory manager that can put any ROM or RAM page in any of the four
 * windows, an address latch on the ATM-Turbo 1's #FE, a palette of sixteen
 * that colours every video mode, and three video modes beside the ZX one:
 * EGA 320x200 in sixteen colours, a 640x200 hires with an attribute per
 * byte, and 80x25 text.
 *
 * Run at 3.5 MHz: the #77 turbo bit is latched and ignored, because the
 * clock here is the real CPU's and does not go to 7 MHz.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "spectrum.h"

static inline bool spec_is_atm(const machine_t m) {
    return m == MACHINE_ATM1 || m == MACHINE_ATM2 || m == MACHINE_ATM2P
        || m == MACHINE_ATM2X || m == MACHINE_ATM3;
}

typedef enum : uint8_t { ATM_VM_ZX = 0, ATM_VM_EGA, ATM_VM_HIRES, ATM_VM_TEXT } atm_vmode_t;

/* The ROM image the machine boots from, and how many 16 KB pages it is. */
const uint8_t *atm_rom_image(machine_t m, unsigned *pages);

void atm_reset(void);
void atm_remap(void);

/* Port hooks, ahead of everything else in spectrum.c. True = consumed. */
bool atm_port_write(uint16_t port, uint8_t value);
bool atm_port_read(uint16_t port, uint8_t *value);

/* The ATM-Turbo 1's #FE address latch, and the 4-bit border it carries. */
void atm_fe_write(uint16_t port, uint8_t value);

/* TR-DOS in and out, by where the CPU fetches. Replaces the Beta rule. */
void atm_opfetch(uint16_t addr);

/* #77 D5 gates the frame interrupt on the 2+ and the 3. */
bool atm_int_enabled(void);

atm_vmode_t atm_video_mode(void);

/* The palette as 2-bit-a-channel RGB, for the video driver. */
extern volatile bool atm_pal_dirty;
void atm_palette_rgb2(uint8_t rgb[16][3]);

/* The character generator for the 80x25 text mode, 8 bytes a glyph. */
const uint8_t *atm_font_ptr(void);

/* The border's palette index: four bits, BRIGHT from #FE's A3. */
uint8_t atm_border_index(void);
