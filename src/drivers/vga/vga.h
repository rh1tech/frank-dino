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
/*
 * Two pictures this driver can put on the wire, and it used to be three.
 *
 * The third was an 80x30 character mode that carried the stage-0 report
 * before the menus had a surface of their own. Everything is drawn on
 * that surface now -- the report, the browser, the settings, the pages
 * -- so the character generator, its font and its 4,800-byte buffer
 * were nine kilobytes of SRAM for a picture nothing asked for.
 *
 * SPECTRUM is the machine's screen, built scanline by scanline out of
 * the guest's RAM with no framebuffer in between. UI is the menus'
 * 320x240 surface in the PSRAM, doubled.
 */
typedef enum { VGA_MODE_SPECTRUM = 0, VGA_MODE_UI } vga_mode_t;

/*
 * The menu's surface: 320x240, one byte per pixel, doubled to 640x480.
 *
 * pico-speccy's OSD rasteriser draws into a 320x240 8bpp framebuffer and
 * lets the video hardware scale it up, so its layout arithmetic -- a 6x10
 * font, 12-pixel rows, a 1-pixel window border, a 2-pixel margin -- is
 * written against those dimensions and nothing else. Give it the same
 * surface and its drawing code ports across very nearly unchanged; give
 * it a character grid instead and every line of it has to be rewritten,
 * which is how the last two attempts at this UI went.
 *
 * The byte is an index into palette16, so 0..15 are the UI colours in
 * UiColor order and nothing else is ever stored.
 *
 * It lives in the PSRAM because 76,800 bytes do not fit in what is left
 * of the 512 KB, and it can live there because the only thing that reads
 * it is this driver, 320 bytes at a time, with the Z80 stopped and a
 * whole line of DMA in hand. Nothing on the bus engine's critical path
 * ever touches it.
 */
#define VGA_UI_W 320
#define VGA_UI_H 240
extern uint8_t vga_ui_fb[VGA_UI_W * VGA_UI_H];

void vga_init(void);
void vga_set_mode(vga_mode_t mode);
vga_mode_t vga_get_mode(void);

/*
 * Rebuild the menu's dither table from settings.ui_solid.
 *
 * Called at init and again whenever that setting changes, which is the
 * only thing that moves it.
 */
void vga_build_ui_palette(void);

/* Rebuild the ULA+ colour tables. Called from the blanking. */
void vga_build_ulaplus(void);
void vga_refresh_ulaplus_border(void);
