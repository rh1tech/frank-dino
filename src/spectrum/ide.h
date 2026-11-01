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
    IDE_SMUC,       // the Scorpion's card; its ports live in smuc.c
    IDE_COUNT,
} ide_scheme_t;

bool        ide_mount(const char *path, char *detail, unsigned detail_len);
void        ide_eject(void);
bool        ide_mounted(void);
const char *ide_path(void);
void        ide_reset(void);

/* The Evo's NEMO: #10 alone moves whole words (ide.c). */
extern bool ide_nemo_toggle;

bool ide_port_out(uint16_t port, uint8_t v);
bool ide_port_in(uint16_t port, uint8_t *v);

/* The register file for an interface that decodes its own ports (SMUC):
 * register 0 is the 16-bit data register's low byte, its high byte goes
 * through the latch. */
uint8_t ide_smuc_read(unsigned reg);
void    ide_smuc_write(unsigned reg, uint8_t v);
uint8_t ide_smuc_latch_read(void);
void    ide_smuc_latch_write(uint8_t v);
void    ide_smuc_control(uint8_t v);    // Device Control: SRST on D2
