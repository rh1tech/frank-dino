/*
 * frank-dino — an RP2350B acting as the whole machine around a real Z80
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-dino
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * The picture as the ULA saw it.
 *
 * The HDMI side runs at 60 Hz off its own clock and used to read the
 * guest's screen memory live, whenever its scanline happened to come
 * round. That shows the screen as it is at some arbitrary moment, which
 * is right for a still picture and wrong for anything that changes the
 * screen while the beam is on it: multicolour engines rewrite the
 * attributes a line ahead of the beam, and border effects change the
 * border colour dozens of times a line. None of that survived.
 *
 * So core 1, which knows the T-state of every write, builds the frame as
 * the ULA would have fetched it. A write to the screen after the beam has
 * passed a byte means the beam saw the byte as it was -- so the old value
 * is frozen into the frame. Whatever is never frozen is copied from RAM
 * at the end of the frame, when every byte the ULA fetched is final. The
 * border is a list of (T-state, colour) changes.
 *
 * Three frames: one being built, one finished, one on the screen. Core 0
 * takes the finished one at the top of each HDMI frame.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "pico.h"

#define ULA_EV_MAX 512u

/* A border change: the T-state in bits 31-4, the colour in 2-0. */
typedef uint32_t ula_ev_t;
#define ULA_EV(t, c)     (((uint32_t)(t) << 4) | ((c) & 7u))
#define ULA_EV_T(e)      ((e) >> 4)
#define ULA_EV_COL(e)    ((e) & 7u)

typedef struct {
    uint8_t  bmp[192][32];      // the byte the ULA fetched for each cell
    uint8_t  att[192][32];      // ...and its attribute, line by line
    uint32_t paper_t0;          // T-state of the first paper pixel
    uint32_t line_t;            // T-states a raster line
    uint8_t  border_at_start;
    uint16_t nev;
    ula_ev_t ev[ULA_EV_MAX];
} ula_frame_t;

/* Is the builder on? Core 1 reads it on every frame and screen write. */
extern volatile bool ula_beam;

/* Core 1: the frame starts and ends. */
void ula_frame_begin(void);
void ula_frame_end(void);

/* Core 1: the paging or the displayed bank changed; re-aim the watch. */
void ula_rewatch(void);

/* Core 1: the border changed at frame T-state t. */
void ula_border(uint32_t t, uint8_t colour);

/* Core 0: the newest finished frame, or NULL if there is none. */
const ula_frame_t *ula_frame_latest(void);

/* Turn it on or off, from the settings. */
void ula_set_enabled(bool on);
