/*
 * frank-dino — an RP2350B acting as the whole machine around a real Z80
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-dino
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * The 8 MB of PSRAM on XIP CS1 (U7, an ESP-PSRAM64H on GP47).
 *
 * Not a peripheral to talk to: once the QMI's second chip select is
 * configured the chip is memory, mapped at 0x11000000 and read and
 * written like any other address. What this file does is the
 * configuration -- probe for the chip, put it into QPI mode, set the
 * read and write commands and time the interface to the system clock.
 *
 * Why it matters here: the on-chip 512 KB is nearly full. 128 KB of
 * guest RAM, four ROMs, a patched ROM page, a 64 KB tape buffer, two
 * video buffers and the code itself came to 90 %, and every feature
 * after that was a question of what to take out. Anything large and not
 * on the Z80's critical path belongs over here instead.
 *
 * What does NOT belong here is anything the bus engine touches while the
 * CPU is clocked. A PSRAM read is a cache miss away from a microsecond,
 * and a microsecond is three T-states at 3.5 MHz.
 *
 * The sequence is pico-speccy's, which is the same chip on the same
 * family of board, including the two things that are easy to get wrong:
 * the ID probe has to be retried because the first transaction after a
 * chip reset comes back as noise, and nothing inside the direct-mode
 * window may touch flash -- not a function call into XIP, not an
 * interrupt whose handler lives there -- because CS0 is unavailable
 * while direct mode is on and the core would stall for ever.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define PSRAM_BASE 0x11000000u

/*
 * Configure CS1 and find out how much is there.
 *
 * Call once, early, from core 0, and before anything reads a `.psram`
 * object. Returns the size in bytes, or zero if there is no chip -- in
 * which case nothing has been configured and the region must not be
 * touched.
 */
uint32_t psram_init(void);

/* The size found by psram_init(), without probing again. */
uint32_t psram_size(void);

/*
 * Write a pattern over `bytes` of it and read it back.
 *
 * Returns the number of words that did not match. The interface runs at
 * a divided system clock with a receive delay that is calculated rather
 * than measured, and a chip that is marginal does not fail -- it returns
 * the wrong byte now and then, which upstream looks exactly like a
 * program that crashed by itself. This is how that is told apart.
 *
 * Destroys whatever was in the region tested, so it takes a length: the
 * disk and tape images live at the bottom of the window.
 */
uint32_t psram_test(uint32_t from, uint32_t bytes);
