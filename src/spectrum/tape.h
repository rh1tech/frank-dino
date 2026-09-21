/*
 * frank-dino — an RP2350B acting as the whole machine around a real Z80
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-dino
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * .TAP loading, by intercepting the ROM's own tape routine.
 *
 * A .TAP is the bytes a tape would have carried with the timing thrown
 * away, so it cannot be replayed as a signal — it has to be handed to
 * whatever asked for it. What asks is LD-BYTES at 0x0556 in the 48K ROM,
 * and the usual emulator trick is to notice the CPU arriving there and
 * satisfy the call from the file instead.
 *
 * On real hardware that trick needs an extra step, and it is the
 * interesting part of this file. An emulator reads IX, DE and A out of
 * its CPU struct; there is no such struct here, and no way to read a
 * Z84C00's registers from outside it. So the CPU is made to hand them
 * over: the ROM window is replaced by a copy with a short routine
 * patched in at 0x0556, and that routine pushes each register out
 * through a port this firmware is listening on. Once the host has IX, DE
 * and the expected block type it writes the block into guest RAM itself,
 * and the routine asks for the verdict with an IN and returns with the
 * carry flag the ROM expects.
 *
 * ---------------------------------------------------------------------
 * What this does not do: custom loaders.
 *
 * Anything that loads its own way — Speedlock, Alkatraz, most commercial
 * protection, and every turbo loader — never calls 0x0556 and never
 * sees any of this. Those need the tape replayed as a real signal on the
 * ULA's EAR bit with the original timings, which is a different feature
 * and a much larger one.
 * ---------------------------------------------------------------------
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/*
 * The largest .TAP that can be mounted.
 *
 * Held whole in RAM because the trap runs while the Z80 is executing,
 * and FatFs may only be entered from core 0 with the machine paused —
 * so reading a block on demand is not available. 64 KB covers a 48K
 * game's tape comfortably; the 8 MB of PSRAM on GP47 is the way past it
 * when something needs more.
 */
#define TAPE_MAX_BYTES (64 * 1024)

/* Take a .TAP image. Returns false if it is too big or malformed. */
bool tape_mount(const uint8_t *image, uint32_t len, char *detail, unsigned detail_len);

/*
 * Take a .TZX that is already sitting in tape_image, and convert it.
 *
 * A .TZX carries its own timings, which a ROM trap has nowhere to put --
 * so only the two record types whose payload is a .TAP block survive the
 * conversion, and a file whose loader is a pulse train comes out empty
 * and is refused by name.
 */
bool tape_mount_tzx(uint32_t len, char *detail, unsigned detail_len);

/* Forget it and put the real ROM back. */
void tape_eject(void);

bool     tape_mounted(void);
uint32_t tape_block_count(void);
uint32_t tape_block_index(void);

/*
 * Where the image is staged, which is in the PSRAM.
 *
 * Exposed so the loader can read a file straight into it rather than
 * through a second copy. Sixty-four kilobytes is an eighth of the
 * RP2350's whole SRAM and only a hundred and twenty-eighth of the chip
 * on CS1, and a tape is read rarely and never while the clock is
 * running -- so this is what that chip is for.
 *
 * Do not touch it before psram_init() has found the chip; tape_mount()
 * checks, and refuses on a board that has none.
 */
extern uint8_t tape_image[TAPE_MAX_BYTES];

/*
 * Re-patch after the machine has changed.
 *
 * The 48K's ROM and the 128K's ROM 1 are not the same image, so the
 * patched copy has to be made again from whichever one is now in use.
 * Does nothing when no tape is mounted.
 */
void tape_machine_changed(void);

/* Ports, called from the bus handlers. True if this was ours. */
bool tape_port_out(uint16_t port, uint8_t value);
bool tape_port_in(uint16_t port, uint8_t *value);
