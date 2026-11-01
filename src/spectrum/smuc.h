/*
 * frank-dino — an RP2350B acting as the whole machine around a real Z80
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-dino
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * SMUC, the Scorpion's multi-unit controller: an IDE port, a 24LC16 NVRAM
 * and an MC146818 clock on one card. See smuc.c.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Read the NVRAM from the card. Core 0, at boot, after the card is up. */
void smuc_init(void);

/* Write the NVRAM back a moment after it changes. Core 0's main loop. */
void smuc_task(void);

/* Machine reset: the bus idle, the latches clear; the NVRAM keeps its contents. */
void smuc_reset(void);

/* Whether this machine has the card in it. */
bool smuc_fitted(void);

/* The ports. Core 1; true when the card answered. */
bool smuc_port_out(uint16_t port, uint8_t v);
bool smuc_port_in(uint16_t port, uint8_t *v);

/* The 24LC16 alone, for the host tests. */
void    nvram24_write(uint8_t sys);
uint8_t nvram24_read(void);
uint8_t *nvram24_mem(void);
