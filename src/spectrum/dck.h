/*
 * frank-dino — an RP2350B acting as the whole machine around a real Z80
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-dino
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * A Timex cartridge: the .dck that goes in the dock.
 *
 * Ported from pico-speccy src/Timex.h's dckParse() and Timex.cpp's
 * mountDck(). Without it the machine's HSR selects chunks that are not
 * there and the dock reads as an open bus, which is a TC2068 with
 * nothing plugged into it -- true, but not much use.
 *
 * The format is a sequence of blocks, each a nine-byte header -- a bank
 * number and eight chunk types, one per 8 KB of the address space --
 * followed by the chunks the header said are in the file, in order.
 * The type is two bits:
 *
 *   0  nothing there
 *   1  RAM the file does not carry, so it starts as zeroes
 *   2  ROM, in the file
 *   3  RAM, in the file
 *
 * Only bank 0, the DOCK, is used. Bank 1 is the EX-ROM, which this
 * machine has in its own ROM already, and banks above that do not
 * exist.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define DCK_CHUNK  8192
#define DCK_CHUNKS 8

/*
 * The cartridge's contents, in the PSRAM.
 *
 * Sixty-four kilobytes, which is the whole address space and so the
 * largest a dock cartridge can be. Read by the bus engine through the
 * page table, which means a chunk that is mapped has to be in SRAM --
 * so mounting copies what is used into the window below rather than
 * pointing at this.
 */
bool dck_mount(const char *path, char *detail, unsigned detail_len);
void dck_eject(void);
bool dck_mounted(void);

/* Which chunks the cartridge fills, as a bitmap, for the settings page. */
uint8_t dck_present(void);

/*
 * The 8 KB to map at chunk `i`, or NULL when the cartridge does not
 * fill it -- the caller then shows an open bus, as a real dock does.
 */
uint8_t *dck_chunk(unsigned i);

/* Whether that chunk is RAM. A ROM chunk's writes go nowhere. */
bool dck_writable(unsigned i);
