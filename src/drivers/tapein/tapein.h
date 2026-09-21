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
 * a phone on J9, and the Spectrum's own ROM doing the decoding, pulse by
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
 * This costs the console's receive side, and it is the board, not a
 * choice. J9's comparator output and J1's UART RX are the same net --
 * the schematic calls it TAPE_UART_RX -- so GP45 is either one or the
 * other. Turning this on takes the pin; the console can still print,
 * but nothing typed at it arrives.
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

/* Start sampling, taking GP45 off the console. Idempotent. */
void tapein_start(void);

/* Stop, and give the pin back. Idempotent. */
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
