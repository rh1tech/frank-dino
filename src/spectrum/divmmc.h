/*
 * frank-dino — an RP2350B acting as the whole machine around a real Z80
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-dino
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * DivMMC, and esxDOS on top of it.
 *
 * Ported from pico-speccy src/DivMMC.cpp and DivMMC.h. The interface is
 * an 8 KB EEPROM holding esxDOS, 128 KB of RAM in 8 KB banks, and an SD
 * card on a bit-banged SPI port -- and it pages itself in and out of the
 * bottom of the address space by WATCHING THE CPU rather than by being
 * told, which is the part worth understanding.
 *
 * An opcode fetch at one of six entry addresses -- #0000, #0008, #0038,
 * #0066, #04C6, #0562 -- maps the interface in after that fetch
 * completes, so the instruction at the entry point still comes from the
 * Spectrum's own ROM and everything after it comes from esxDOS. A fetch
 * anywhere in #1FF8-#1FFF maps it out again the same way. A fetch in
 * #3D00-#3DFF maps it in immediately, before the fetch, which is the
 * same rule the Beta Disk uses and the reason the two interfaces cannot
 * both be on.
 *
 * That is why the bus engine pages in 8 KB windows: when mapped, the
 * interface replaces #0000-#1FFF with its ROM and #2000-#3FFF with one
 * of its RAM banks, and a 16 KB table cannot say that.
 *
 * The card behind it is a .mmc or .hdf image on the microSD, read a
 * block at a time. Nothing is held in memory: a card image is hundreds
 * of megabytes and the guest only ever asks for 512 bytes of it.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define DIVMMC_ROM_SIZE  0x2000     /* 8 KB of esxDOS */
#define DIVMMC_BANK_SIZE 0x2000
#define DIVMMC_BANKS     16         /* 128 KB, the usual board */

/* Open a .mmc/.hdf as the card. NULL closes whatever was open. */
bool divmmc_mount(const char *path, char *detail, unsigned detail_len);
void divmmc_eject(void);
bool divmmc_mounted(void);

/* Reset the interface, as the machine's own reset line does. */
void divmmc_reset(void);

/*
 * Whether the interface is paged in at this moment, and the two 8 KB
 * pages it shows when it is. apply_paging() consults these.
 */
bool     divmmc_mapped(void);
uint8_t *divmmc_page_lo(void);      /* #0000-#1FFF: the ROM, or bank 3 */
uint8_t *divmmc_page_hi(void);      /* #2000-#3FFF: the selected bank */
bool     divmmc_lo_writable(void);  /* only when MAPRAM has put RAM there */

/*
 * The automap, from the opcode-fetch hook.
 *
 * Called before the byte is supplied, and applies whatever the PREVIOUS
 * fetch armed -- see the note in divmmc.c about why deferring by one
 * fetch is the same thing as their "after the fetch".
 */
void divmmc_opfetch(uint16_t addr);

/* Ports, called from the I/O handlers. True if this was ours. */
bool divmmc_port_out(uint16_t port, uint8_t value);
/* The Z-Controller's #57/#77, on the same card. True if it was theirs. */
bool divmmc_zc_out(uint16_t port, uint8_t value);
bool divmmc_zc_in(uint16_t port, uint8_t *value);

bool divmmc_port_in(uint16_t port, uint8_t *value);

/* What the card is doing, as a short line of text. */
void divmmc_stats(char *out, unsigned len);
