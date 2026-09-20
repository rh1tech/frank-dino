/*
 * frank-dino — an RP2350B acting as the whole machine around a real Z80
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-dino
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * 640x480 at 60 Hz, 80x30 characters, and nothing else.
 *
 * frank-xt8086's video driver renders CGA, Tandy, Hercules, EGA and VGA
 * out of one scanline interrupt, because the machine it serves has a
 * video card that changes underneath it. This board's stage 0 has no
 * video card at all — it has a report to show — and the ZX Spectrum it
 * will eventually have needs a renderer with a border and a 256x192
 * bitmap, which is not a mode a CGA driver has. So this is the whole of
 * what is needed and no more: one fixed timing, one font, one buffer.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

#define VGA_COLS 80
#define VGA_ROWS 30

/*
 * Character cell, attribute cell. Two bytes per cell in one buffer, the
 * way every text-mode machine since the MDA has done it, because the
 * renderer wants them in the same cache line.
 *
 * Attribute: bits 0-3 foreground, bits 4-7 background, from the 16-colour
 * table in vga.c. No blink bit — this screen is read by a camera on a
 * capture stick as often as by a person, and a blinking character is a
 * character that is sometimes not in the photograph.
 */
extern uint8_t vga_text[VGA_COLS * VGA_ROWS * 2];

/* Frames output since init. The one honest "is the video alive" signal. */
extern volatile uint32_t vga_frames;

/*
 * Two pictures this driver can put on the wire.
 *
 * TEXT is the stage-0 report: 80x30 characters, built for being read off
 * a capture stick. SPECTRUM is a ZX Spectrum's screen, read straight out
 * of the guest's RAM with no framebuffer in between — there is nowhere
 * for one to live that the CPU would not overwrite anyway.
 */
typedef enum { VGA_MODE_TEXT = 0, VGA_MODE_SPECTRUM } vga_mode_t;

void vga_init(void);
void vga_set_mode(vga_mode_t mode);
