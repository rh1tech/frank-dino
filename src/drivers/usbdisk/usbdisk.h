/*
 * frank-dino — an RP2350B acting as the whole machine around a real Z80
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-dino
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * A USB stick in the other USB-A port, as a second drive.
 *
 * Ported from pico-speccy src/UsbMsc.cpp -- which is a HOST-side mass
 * storage driver, not the card offered to a PC. The board has a hub and
 * two USB-A ports, so a keyboard and a stick coexist, and the stick
 * becomes FatFs volume 1 beside the card's volume 0.
 *
 * Only in a USB_KEYBOARD build. S1 gives the RP2350's D+/D- either to
 * the USB-C socket or to the hub, and the host stack only exists on the
 * hub side -- so a board switched to device mode has no ports to put a
 * stick in.
 *
 * This file also carries the diskio router. FatFs calls disk_read() with
 * a drive number; the SD driver used to answer for drive 0 and refuse
 * everything else, so the number had nowhere to go. Now the four entry
 * points live here and hand off: drive 0 to the card, drive 1 to this.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Whether a stick is enumerated and answering. */
bool usbdisk_present(void);

/* Its capacity in bytes, or 0. */
uint64_t usbdisk_size(void);

/*
 * Mount or unmount volume 1 as sticks come and go.
 *
 * Called from the main loop, because a stick may be plugged in at any
 * time and FatFs must not be mounted from a TinyUSB callback -- the
 * callback runs inside tuh_task() and f_mount() reads the medium, which
 * needs tuh_task() to run.
 */
void usbdisk_task(void);

/* The name FatFs knows it by, for f_open and friends. */
#define USBDISK_VOLUME "1:"
