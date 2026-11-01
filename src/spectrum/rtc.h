/*
 * frank-dino — an RP2350B acting as the whole machine around a real Z80
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-dino
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * The Mr Gluk clock and its NVRAM. See rtc.c.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Read the NVRAM from the card. Core 0, at boot, after the card is up. */
void rtc_init(void);

/* Write the NVRAM back a moment after it changes. Core 0's main loop. */
void rtc_task(void);

/* The ports: #DFF7 selects, #BFF7 reads and writes. Core 1. */
extern bool rtc_wide;   /* 256 cells, not 64: the ZX Evolution's AVR */
void    rtc_select(uint8_t reg);
uint8_t rtc_read(void);
void    rtc_write(uint8_t v);
