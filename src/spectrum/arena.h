/*
 * frank-dino — an RP2350B acting as the whole machine around a real Z80
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-dino
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * The PSRAM arena: the big buffers no machine needs all at once, in one
 * place. See arena.c.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define ARENA_KB(n) ((uint32_t)(n) * 1024u)

/*
 *   0       RAM pages 8-63 -- every machine with more than 128 KB
 *   896 KB  the raw tape capture (tapein.c, a tool)
 *   1920 KB the +3's disk image (fdc.c)
 *   2944 KB the ALF cartridge
 *   ...     and on a 4 MB machine, pages 64-255 over all three
 */
#define ARENA_RAM_OFF   0u
#define ARENA_CAP_OFF   ARENA_KB(896)
#define ARENA_DSK_OFF   ARENA_KB(1920)
#define ARENA_ALF_OFF   ARENA_KB(2944)
#define ARENA_SIZE      ARENA_KB(3968)          // 248 pages: 4 MB less the 8 in SRAM

extern uint8_t psram_arena[ARENA_SIZE];

/* Whether the space above the first 1 MB of RAM is the RAM's (a 4 MB
 * machine) rather than the capture's, the disk's and the cartridge's. */
extern bool arena_big_ram;
