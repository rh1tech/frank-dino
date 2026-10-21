/*
 * frank-dino — an RP2350B acting as the whole machine around a real Z80
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Ported from pico-speccy's LoadWavStream.h and the pcm_data_in() half
 * of pwm_audio.cpp (GPL-3.0-or-later).
 */

/*
 * Loading from a real tape: the EAR line, sampled.
 *
 * Everything else here loads from an image through a ROM trap, which is
 * instant and cannot fail. This is the other thing -- a cassette deck or
 * a phone on J8, and the Spectrum's own ROM doing the decoding, pulse by
 * pulse, exactly as it did in 1982.
 *
 * Their arrangement, kept: the input pin is sampled at a fixed high rate
 * into a bit buffer by a timer, and the guest reads back out of it at
 * whatever point in the frame it happens to ask. Two buffers, one being
 * filled while the other is read, swapped at the frame boundary. The
 * sampling is therefore decoupled from the Z80 entirely -- which is the
 * point, because a loader's timing loop reads #FE at intervals that have
 * nothing to do with when a sampler would like to run.
 *
 * ---------------------------------------------------------------------
 * GP45 is the tape input and nothing else. JP7 chooses what drives it:
 * Q4 with R63 pulling its collector up, or U15, a CD4069 run as a
 * linear amplifier. With JP7 open the pin is connected to nothing, and
 * tapein_probe() is what says so.
 * ---------------------------------------------------------------------
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/*
 * Their sample rate, and their reason for it: it must not be a multiple
 * of the video DMA's rate, or the DMA -- which has the higher priority
 * -- delays the same tick every time and the jitter becomes a pattern
 * the ROM's timing loop can beat against.
 */
#define TAPEIN_SAMPLE_HZ 125000u

/* Start sampling GP45. Idempotent. */
void tapein_start(void);

/* Stop sampling. Idempotent. */
void tapein_stop(void);

bool tapein_running(void);

/*
 * Swap the buffers. Call once per frame, before the machine runs, with
 * the number of T-states that frame will take.
 */
void tapein_frame_started(uint32_t frame_tstates);

/*
 * The EAR level this far into the frame.
 *
 * `tstates` is how far the machine has got through the frame it was
 * given, so the answer comes out of the PREVIOUS frame's buffer at the
 * matching position. A frame of lag, which no loader can tell from a
 * tape head a frame further along.
 */
bool tapein_level(uint32_t tstates);

/*
 * Whether anything is driving the input pin.
 *
 * Bit 0 is the level read with an internal pull-down, bit 1 with a
 * pull-up. 0b10 means the pin followed both -- it is floating, and
 * nothing outside the chip is connected to it. Anything else means
 * something is holding it. Restores the sampler if it was running.
 */
uint8_t tapein_probe(void);

/* What the probe said last, including the one tapein_start() runs. */
extern uint8_t tapein_last_probe;

/*
 * What the line is doing, as a short line of text for the menu.
 *
 * "idle" when nothing is crossing the threshold, "nothing connected"
 * when the pin is not even being driven, and a frequency otherwise --
 * named when it is one the ROM is listening for. Reading it clears the
 * counters, so it shows the moment rather than the session.
 */
void tapein_stats(char *out, unsigned len);
