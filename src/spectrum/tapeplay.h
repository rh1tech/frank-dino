/*
 * frank-dino — an RP2350B acting as the whole machine around a real Z80
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-dino
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * A tape playing in real time, onto the EAR bit.
 *
 * pulse.c makes the signal on core 0; this carries it to core 1 and
 * answers the ULA's EAR bit from it at the exact T-state of each IN.
 *
 * The motor follows the program, the way a careful person at a cassette
 * deck would. A loader reads port #FE thousands of times a frame; a
 * program that has stopped loading reads it a few times, for the
 * keyboard. So a second of quiet stops the tape and a frame of busy
 * reading starts it again -- which is what makes a multi-load game work
 * without anyone pressing Play between levels.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "pico.h"

#include "pulse.h"

/* Is a real-time tape mounted? Core 1 reads this on every IN #FE. */
extern volatile bool tapeplay_active;

/* Mount an image the generator can read in place. Core 0, machine paused. */
bool tapeplay_mount(const uint8_t *image, uint32_t len, pulse_fmt_t fmt);
void tapeplay_eject(void);

/* Motor control, for the browser and the menu. Core 0, machine paused. */
void tapeplay_play(void);
void tapeplay_stop(void);
void tapeplay_rewind(void);
bool tapeplay_seek(uint32_t block);
bool tapeplay_motor_on(void);

/* Keep the ring full. Core 0's main loop. */
void tapeplay_task(void);

/* The EAR level at frame T-state t. Core 1, from the ULA's IN. */
bool tapeplay_ear(uint32_t t);

/* The frame is over: move the clock on and run the motor heuristic. Core 1. */
void tapeplay_frame_end(uint32_t frame_tstates, uint32_t turbo_num);

/* For the settings page. */
uint32_t tapeplay_underruns(void);
