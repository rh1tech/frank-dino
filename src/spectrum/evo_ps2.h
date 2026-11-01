/*
 * frank-dino — an RP2350B acting as the whole machine around a real Z80
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-dino
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * The ZX Evolution's keyboard as its AVR passes it on: PS/2 set-2
 * scancodes in a log the Z80 reads through the Gluk clock's cells
 * #F0-#FF, and the modifier keys' state in cells #0D and #0E. See
 * evo_ps2.c.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Core 0: a key by its USB usage, pressed or let go (0xE0-0xE7 are the
 * modifiers). */
void evo_ps2_hid(uint8_t usage, bool down);

/* Core 0: two boot-protocol keyboard reports, before and now: every key
 * and modifier that changed goes into the log. */
void evo_ps2_report(const uint8_t now[8], const uint8_t before[8]);

/* Core 0: a console character as the keys that type it, down and up. */
void evo_ps2_ascii(char c);

/* Core 1: the next byte of the log -- 0 when it is empty, 0xFF once after
 * it overflowed. */
uint8_t evo_ps2_pop(void);

/* Either core: empty the log (cell #0C D0, and a machine reset). */
void evo_ps2_clear(void);

/* Cells #0D and #0E: LCtrl RCtrl LAlt RAlt LShift RShift F12, and
 * LWin RWin Menu, one bit each. */
uint8_t evo_ps2_mods0(void);
uint8_t evo_ps2_mods1(void);
