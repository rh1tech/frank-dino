/*
 * frank-dino — an RP2350B acting as the whole machine around a real Z80
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-dino
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * The machine's sound, made on core 0 from what core 1 saw.
 *
 * Core 1 is the Z80's clock, and every cycle it spends on anything else
 * is a cycle the CPU waits. Mixing two AYs and an SAA 882 times a frame
 * used to be most of what it spent -- so now it does not. The port
 * handlers note each write to a sound device as an event, stamped with
 * the T-state it happened at, and core 0 replays the events into the
 * chips and renders the samples between them.
 *
 * The stamp is what makes this better rather than merely cheaper. The
 * old mix read the beeper once per sample, at the end of whatever slice
 * of the frame that sample was; now an OUT lands at its own T-state, and
 * a sample is the average of the beeper over the time it covers.
 *
 * One producer and one consumer, never the same core at once: core 1
 * while the machine runs, core 0 while it is paused and something on
 * core 0 runs frames itself (the clock measurement). Lock-free on that
 * understanding.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "pico.h"
#include "hardware/sync.h"

typedef enum : uint8_t {
    SD_AY_SEL0 = 0, SD_AY_DAT0,     // the first AY: register select, data
    SD_AY_SEL1, SD_AY_DAT1,         // the second, under TurboSound
    SD_SAA_SEL, SD_SAA_DAT,         // the SAA1099
    SD_FE,                          // port #FE bits 3 and 4: MIC and the beeper
    SD_COVOX,
    SD_SD0, SD_SD1, SD_SD2, SD_SD3, // SounDrive: two DACs a side
    SD_SPECDRUM,
    SD_OPN0_A, SD_OPN0_D,           // TurboSound FM: the two YM2203s' FM halves
    SD_OPN1_A, SD_OPN1_D,
    SD_FM_ON,                       // ...and the board's FM mute, val 0/1
    SD_OPL0, SD_OPL1, SD_OPL2, SD_OPL3,  // the VGM card's YMF262, ports #C4-#C7
    SD_OPLL_A, SD_OPLL_D,           // its YM2413, #C0 and #C1
    SD_SN0, SD_SN1,                 // its SN76489 pair
    SD_CMS0_SEL, SD_CMS0_DAT,       // Creative Music System: two SAA1099s
    SD_CMS1_SEL, SD_CMS1_DAT,
    SD_PIT0, SD_PIT1, SD_PIT2, SD_PIT3,  // the Byte's 8253: three counters and control
    SD_MARK,                        // nothing happened; render up to here
    SD_BEGIN,                       // a frame starts: t carries its length
    SD_SPF,                         // ...and t carries its sample count
    SD_END,                         // a frame ends: render the rest of it
    SD_RESET,                       // the machine was reset
    SD_DEV_COUNT,
} snd_dev_t;

/*
 * Sixteen kilobytes. A frame of beeper music is the worst case at a few
 * thousand events, and core 0 drains the queue every couple of
 * milliseconds; this is several frames of slack.
 */
#define SNDQ_SIZE 4096u

extern uint32_t          sndq[SNDQ_SIZE];
extern volatile uint32_t sndq_head, sndq_tail;
extern volatile uint32_t sndq_drops;

/*
 * Post one event. t is the T-state within the frame (eighteen bits is a
 * frame at twice the clock), dev a snd_dev_t, val the byte written.
 *
 * Four slots are kept back for the frame markers, so a flood of writes
 * can lose writes but never lose a frame boundary -- and a lost write is
 * counted, because a beeper that has lost an edge is out of phase until
 * the next one.
 */
__force_inline static void snd_put(const uint32_t t, const uint32_t dev, const uint32_t val) {
    const uint32_t h = sndq_head;
    const uint32_t room = (dev >= SD_MARK) ? SNDQ_SIZE : SNDQ_SIZE - 4u;
    if (h - sndq_tail >= room) { sndq_drops++; return; }
    sndq[h & (SNDQ_SIZE - 1u)] = (t << 14) | (dev << 8) | (val & 0xFFu);
    __dmb();
    sndq_head = h + 1u;
}

/* Bring the chips up. Core 0, after audio_init(). */
void sound_init(void);

/* Replay everything posted so far. Core 0's main loop, often. */
void sound_task(void);

/*
 * Throw away everything queued. Only while neither side is running:
 * after the clock measurement, whose frames nobody is meant to hear.
 */
void sound_flush(void);

/* The test tone replaces the mix while it is on. See spec_test_tone(). */
void sound_test_tone(bool on);
bool sound_test_tone_is_on(void);

/*
 * What the ports read back, kept by core 1.
 *
 * The chips live on core 0 now, and a read cannot wait for core 0 to
 * answer -- so the producer keeps the register files it has written and
 * answers from those, which is all a read of either chip ever returns.
 */
typedef struct {
    uint8_t reg[2][16];
    uint8_t latched[2];
    uint8_t saa_reg[32];
    uint8_t saa_sel;
} snd_mirror_t;
extern snd_mirror_t snd_mirror;
