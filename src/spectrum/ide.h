/*
 * frank-dino — an RP2350B acting as the whole machine around a real Z80
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-dino
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * An ATA hard disk behind NEMO, DivIDE or ATM ports. See ide.c.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum : uint8_t {
    IDE_OFF = 0,
    IDE_NEMO,
    IDE_DIVIDE,
    IDE_ATM,
    IDE_COUNT,
} ide_scheme_t;

bool        ide_mount(const char *path, char *detail, unsigned detail_len);
void        ide_eject(void);
bool        ide_mounted(void);
const char *ide_path(void);
void        ide_reset(void);

bool ide_port_out(uint16_t port, uint8_t v);
bool ide_port_in(uint16_t port, uint8_t *v);
