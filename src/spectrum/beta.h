/*
 * frank-dino — an RP2350B acting as the whole machine around a real Z80
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-dino
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * The Beta Disk interface: a WD1793, a 16 KB ROM and one .trd.
 *
 * This is the other half of the Spectrum world. Britain loaded from
 * tape; the USSR, where a Pentagon cost a month's wages and a tape deck
 * was the least of anybody's problems, loaded from disk -- and the
 * interface everyone standardised on was Technology Research's Beta
 * Disk, with a Western Digital FD1793 on it and Nemo's TR-DOS in ROM. A
 * .trd is a raw dump of such a disk: two sides, forty or eighty tracks,
 * sixteen sectors of 256 bytes each, and nothing else.
 *
 * Three pieces have to be right.
 *
 * The paging is the clever bit of the hardware, and it is entirely
 * automatic: the TR-DOS ROM replaces the BASIC ROM when the CPU fetches
 * an opcode from 0x3Dxx, and goes away again on the first opcode fetch
 * at 0x4000 or above. That is why entering TR-DOS is spelled RANDOMIZE
 * USR 15616 -- 15616 is 0x3D00 -- and why no paging port is involved.
 * Here that rule lives in the bus engine's opcode-fetch hook, which is
 * the one place that sees the same thing the interface's address
 * comparator did.
 *
 * The ports only answer while that ROM is paged in, which is also true
 * of the real interface and is what keeps the controller's status
 * register from colliding with a Kempston joystick: both are port #1F,
 * and on real hardware they coexisted for exactly this reason.
 *
 * The controller is modelled by result rather than by timing. A real
 * WD1793 rotates a disk under a head and an emulator that wants the
 * copy protection to work models the flux; this one has the sectors in
 * memory and hands them over as fast as the Z80 asks, because the Z80 is
 * real and there is nothing to synchronise it to.
 *
 * What that costs: protection that depends on timing, weak sectors or
 * non-standard formats will not work. Ordinary software does not care.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/*
 * The largest .trd: eighty tracks, two sides, sixteen 256-byte sectors.
 *
 * In the PSRAM, like the tape, and for the same reasons -- it is 640 KB,
 * and it is read with the CPU stopped inside an IN.
 */
#define TRD_MAX_BYTES (640 * 1024)
extern uint8_t trd_image[TRD_MAX_BYTES];

/*
 * Take an image that is already in trd_image.
 *
 * `path` is where it came from, remembered so the guest's writes can go
 * back to it. NULL means nowhere -- the image is then read-only in the
 * sense that matters, because there is no file to put it back into.
 */
bool beta_mount(uint32_t len, const char *path, char *detail, unsigned detail_len);

/*
 * Whether the guest has written to the disk since it was mounted or last
 * saved, and putting those writes back on the card.
 *
 * Not automatic. A .trd is somebody's disk image and overwriting it is
 * not a thing to do quietly -- so the controller records that it is
 * dirty, the UI says so, and the saving is asked for. It is also what
 * makes a write-protect switch mean something: with it on the writes
 * never reach the image at all, which is what the switch does on the
 * real interface.
 */
bool beta_dirty(void);
bool beta_save(char *detail, unsigned detail_len);
void beta_eject(void);

/* Drives B-D (1-3): a .trd read from the card a cylinder at a time. */
bool beta_mount_drive(unsigned drive, const char *path, char *detail, unsigned detail_len);
void beta_eject_drive(unsigned drive);
bool beta_drive_mounted(unsigned drive);
const char *beta_drive_path(unsigned drive);

/* Core 0's half: fetch a cylinder a drive asked for, write back what changed. */
void beta_task(void);
bool beta_mounted(void);

/* How many tracks the mounted image turned out to have. */
unsigned beta_tracks(void);

/* Reset the controller, as the interface's own reset line does. */
void beta_reset(void);

/*
 * Is the TR-DOS ROM in the bottom window at this moment?
 *
 * The paging rule is applied by spectrum.c's opcode-fetch hook, which is
 * where the address of every fetch already is; this is the state it
 * keeps, and what apply_paging() consults.
 */
bool beta_rom_paged(void);
void beta_set_rom_paged(bool on);

/*
 * Page the ROM in for the next reset.
 *
 * A Beta Disk 128 comes up with its own ROM at 0x0000 and TR-DOS decides
 * what happens next, which is how a disk with a `boot` file on it starts
 * by itself. This is that, for when a .trd has just been mounted.
 */
void beta_boot_next_reset(void);
bool beta_take_boot_request(void);

/*
 * What to type at the TR-DOS prompt to start the mounted disk.
 *
 * A disk with a `boot` file starts itself and this says so by returning
 * false. One without -- which most single-file .scl images are -- needs
 * RUN "NAME", and this builds it out of the first catalogue entry.
 */
bool beta_autorun(char *out, unsigned len);

/*
 * The last command and the running counts, as a short line of text.
 *
 * A load that fails produces a black screen whatever the reason, so
 * this is the only way to tell a sector that was not found from a
 * program that crashed on its own.
 */
void beta_stats(char *out, unsigned len);

/* Ports, called from the bus handlers. True if this was ours. */
bool beta_port_out(uint16_t port, uint8_t value);
/* Set while something other than the TR-DOS ROM has the ports open. */
extern volatile bool beta_ports_open;

bool beta_port_in(uint16_t port, uint8_t *value);
