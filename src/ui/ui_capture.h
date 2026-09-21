/*
 * frank-dino — an RP2350B acting as the whole machine around a real Z80
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-dino
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * The Spectrum's screen, as a .bmp on the card.
 *
 * Ported from pico-speccy src/CaptureBMP.cpp: the 8-bit indexed BMP with
 * a 256-entry palette, the /captures directory, and the numbered
 * filename found by reading what is already in it.
 *
 * What is different is where the pixels come from. Theirs copies a
 * framebuffer, because their ULA renders into one; this board's video
 * driver builds each scanline out of the guest's RAM as it goes and
 * there is no frame to copy -- so this renders the screen again, at 1:1
 * rather than the doubled size that goes on the wire.
 *
 * It captures the machine's screen and not the menu standing in front
 * of it, which is the one people want: the picture is still in spec_ram
 * while the UI is up.
 */
#pragma once

#include <stdbool.h>

/* Write the next /captures/DINO####.bmp. `detail` is filled either way. */
bool ui_capture_screen(char *detail, unsigned detail_len);
