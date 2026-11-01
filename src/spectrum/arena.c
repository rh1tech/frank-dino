/*
 * frank-dino — an RP2350B acting as the whole machine around a real Z80
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-dino
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * One PSRAM array for the buffers that never meet.
 *
 * The 8 MB PSRAM was full: the tape images, the disk staging, the
 * capture, the +3's disk, the ALF cartridge and a megabyte of RAM pages
 * left some 800 KB. An ATM-Turbo 3 wants 4 MB of RAM. It has no +3 disk
 * drive and no cartridge slot, and the raw tape capture is a bench tool
 * -- so its upper 3 MB of RAM live where those three do on every other
 * machine, and the arena is the one place that says so.
 */
#include "arena.h"

uint8_t psram_arena[ARENA_SIZE] __attribute__((section(".psram"), aligned(4)));
bool    arena_big_ram;
