/*
 * frank-dino — an RP2350B acting as the whole machine around a real Z80
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-dino
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * A USB keyboard, straight onto the Spectrum's key matrix.
 *
 * The TinyUSB host plumbing is frank-xt8086's, which already solved
 * enumeration and boot-protocol reports on this chip. What is different
 * is the far end: that firmware translates to XT scancodes because a PC
 * BIOS wants them, and translating a USB usage into a PS/2 scancode in
 * order to turn it into a Spectrum half-row would be two conversions to
 * do the work of none. This goes straight there.
 *
 * ---------------------------------------------------------------------
 * This costs the console, and it is a physical switch, not a setting.
 *
 * S1 selects what the TS3USB221 (U2) connects the RP2350's D+/D- to: the
 * USB-C socket, or the MW7211A hub and the two USB-A ports. Device or
 * host, one at a time. So a build with a keyboard has no USB CDC console
 * and no `picotool reboot -u -f`, and must be flashed over SWD.
 *
 * That is why this is a build option rather than something the firmware
 * decides: both halves cannot be compiled in usefully, and the hardware
 * cannot be asked which way the switch is pointing.
 * ---------------------------------------------------------------------
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Start the host stack. Core 0, before core 1 runs a machine. */
void usbkbd_init(void);

/* Service the host stack. Core 0 only, from the main loop. */
void usbkbd_task(void);

/* Whether anything has enumerated, for the boot report. */
bool usbkbd_present(void);
