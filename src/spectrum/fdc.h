/*
 * frank-dino — an RP2350B acting as the whole machine around a real Z80
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-dino
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * The +3's disk drive: a NEC uPD765A and one .dsk.
 *
 * Ported from pico-speccy src/Upd765.cpp and src/DskImage.cpp -- their
 * command table byte for byte, their phase machine, and the CPCEMU disk
 * format both the standard and the extended flavour.
 *
 * Two ports, and they are read-only and write-only respectively on a
 * real machine: #2FFD returns the main status register and #3FFD is the
 * data register, through which every command, its parameters, its data
 * and its results pass one byte at a time. The controller is a state
 * machine over that one byte: command phase, execution phase, result
 * phase, and the main status register tells the CPU which it is in and
 * which way the data is meant to flow.
 *
 * What did not come across is the part of theirs that models the
 * surface rather than the data: weak sectors, the several recorded
 * copies a protected disk keeps of one sector, rotational position, and
 * the Speedlock special case built on top of them. This is the same
 * trade beta.c already makes for the Beta Disk and for the same reason
 * -- the Z80 is real and there is nothing to synchronise a flux model
 * to. The cost is the same too: protection that depends on weak sectors
 * or on timing will not load. Ordinary software does not care.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/*
 * The largest .dsk worth holding: 80 tracks, two sides, and room for the
 * per-track headers an extended image carries. In the PSRAM, like the
 * tape and the .trd, and read with the CPU stopped inside an IN.
 */
#define DSK_MAX_BYTES (1024 * 1024)
extern uint8_t dsk_image[DSK_MAX_BYTES];

/*
 * Take an image that is already in dsk_image.
 *
 * `path` is where it came from, remembered so the guest's writes can go
 * back to it. See beta.h for why saving is asked for rather than
 * automatic.
 */
bool fdc_mount(uint32_t len, const char *path, char *detail, unsigned detail_len);

bool fdc_dirty(void);
bool fdc_save(char *detail, unsigned detail_len);
void fdc_eject(void);
bool fdc_mounted(void);

/* How many tracks and sides the mounted image turned out to have. */
unsigned fdc_tracks(void);
unsigned fdc_sides(void);

/* Reset the controller, as the +3's own reset line does. */
void fdc_reset(void);

/* The last command and the running counts, as a short line of text. */
void fdc_stats(char *out, unsigned len);

/* Ports, called from the I/O handlers. True if this was ours. */
bool fdc_port_out(uint16_t port, uint8_t value);
bool fdc_port_in(uint16_t port, uint8_t *value);
