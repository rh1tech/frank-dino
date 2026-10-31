/*
 * frank-dino — an RP2350B acting as the whole machine around a real Z80
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-dino
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Two PWM pins, a left and a right.
 *
 * GP46 is the left channel and GP44 the right. Each goes through a 1K
 * and 10 nF low-pass (R17/C5, R18/C6) into one half of an LM358 wired as
 * a follower, and the follower outputs -- AUDIO_L and AUDIO_R -- feed
 * everything at once: the 3.5 mm jack (J2) always, the MS9288A's audio
 * inputs when S2 connects them, and the PAM8403 and its speaker, which
 * sum the two back to mono, when S2 enables it.
 *
 * Each pin is a PWM DAC -- a carrier of a couple of MHz whose duty is
 * rewritten by DMA -- and the beeper joins the mix as a level rather
 * than driving a pin itself.
 *
 * Losing that directness is a real cost and worth naming: the beeper
 * would reach the transducer with the exact edges the CPU produced if it
 * drove a pin, and here it is sampled like everything else.
 *
 * The samples are produced by core 1, in step with the Z80.
 *
 * That is the part worth understanding. Core 1 does not run a frame and
 * then render audio for it; it runs the frame in sample-sized slices,
 * and after each slice it reads the beeper's level and advances the AYs
 * by exactly that many cycles. So the audio clock and the CPU clock are
 * the same clock, and 882 samples come out of every 70908-T-state frame
 * because both are 20 ms — no resampling, no drift, and a beeper edge
 * lands in the sample it happened in.
 *
 * Core 0 only moves bytes: it copies finished samples out of the ring
 * into whichever DMA block has drained.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

// 44.1 kHz, and not an arbitrary choice: a 128K frame is 70908 T-states
// at 3.5469 MHz, which is 20 ms, which is 882 samples exactly. Anything
// that does not divide the frame evenly puts a seam in every frame.
#define AUDIO_RATE_HZ 44100u
#define AUDIO_SAMPLES_PER_FRAME 882u

/* The rate the DAC actually runs at, in millihertz. See audio.c. */
uint32_t audio_rate_millihz(void);

// Duty values per DMA block. 2048 at the oversampled rate is ~5.8 ms of
// latency and ~170 refills a second, comfortably slower than anything
// else core 0 does -- and it is 256 audio samples, because the duty is
// rewritten eight times per sample. See AUDIO_OSR in audio.c.
#define AUDIO_BLOCK 2048u

/*
 * Take over GP46 and GP44 and start both DACs.
 *
 * Claims four DMA channels -- a chained pair per side -- and one DMA
 * pacing timer that both pairs share, and must run before core 1 starts
 * producing samples.
 */
void audio_init(void);

/*
 * Move finished samples into the DMA blocks. Core 0 only.
 *
 * Cheap when nothing has drained. On an underrun it holds the last
 * sample rather than inserting silence: a held level is a click at worst,
 * and a gap of silence is a click at both ends.
 */
void audio_task(void);

/*
 * Push one finished stereo sample. Core 1 only, called once per audio
 * slice.
 *
 * Signed, roughly +/-15000 a side before clipping. Dropped silently if
 * the ring is full, which means core 0 has stopped draining and the
 * sound is already broken.
 */
void audio_push(int32_t left, int32_t right);

/* How many sample pairs are queued, for the underrun/overrun counters. */
uint32_t audio_queued(void);

/* Silence without stopping the carrier: no click, no half-configured state. */
void audio_set_enabled(bool on);

/* Diagnostics, for the report and for knowing whether to believe the sound. */
extern volatile uint32_t audio_underruns;
extern volatile uint32_t audio_overruns;

/*
 * Samples that did not fit in the DAC's range.
 *
 * A clip is not a fault in itself -- the mix is allowed to be loud --
 * but a steady stream of them is the difference between "this tune is
 * distorted" and "this firmware is distorting it", and that should not
 * be a guess.
 */
extern volatile uint32_t audio_clips;

/*
 * Zero the three counters.
 *
 * Called when the machine resumes, because the interesting number is
 * "since it started running" and not "since power-on". While the menu
 * is open the machine is paused and produces nothing, so the DAC
 * underruns continuously and by design -- without this, that one figure
 * swamps everything and the counter cannot answer the question it
 * exists for.
 */
void audio_reset_stats(void);

/*
 * How many finished samples are waiting to be played.
 *
 * The machine uses this to decide how many samples to cut the next frame
 * into. Nothing else should: it is a queue depth, and it moves.
 */
uint32_t audio_ring_level(void);

// Samples the ring can hold. The producer aims for half of it.
#define AUDIO_RING_SIZE 4096u
