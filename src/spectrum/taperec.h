/*
 * frank-dino — an RP2350B acting as the whole machine around a real Z80
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-dino
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * The tape recorder: what SAVE puts on the MIC line, decoded back into
 * .tap blocks. See taperec.c. Everything here runs on core 0.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* The MIC level changed at absolute T-state t. */
void taperec_edge(uint64_t t);

/* Time has passed with no edge; ends a block that is finished. */
void taperec_poll(uint64_t now);

uint32_t       taperec_blocks(void);
uint32_t       taperec_bytes(void);
const uint8_t *taperec_data(void);
void           taperec_clear(void);

/* Write what has been recorded to /tapes/SAVEnnnn.tap. Core 0, paused. */
bool taperec_write(char *path, unsigned path_len);
