/*
 * frank-dino — an RP2350B acting as the whole machine around a real Z80
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-dino
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The PWM/DMA half of this is frank-xt8086's audio driver, which solved
 * the same problem on the same pin for a PC speaker and an OPL2. What is
 * different here is where the samples come from: there, core 0 rendered
 * them on demand; here core 1 produces them in step with the Z80 and this
 * only moves them.
 */

#include "audio.h"

#include <stdio.h>
#include <hardware/clocks.h>
#include <hardware/dma.h>
#include <hardware/gpio.h>
#include <hardware/irq.h>
#include <hardware/pwm.h>

#include "dino.h"

// ---------------------------------------------------------------------------
// The carrier, and how the eight bits are made to sound like more
// ---------------------------------------------------------------------------
/*
 * Eight bits of duty at clk_sys/256 — 1.97 MHz at 504 MHz.
 *
 * The carrier has to stay up there. There is no reconstruction filter on
 * this board: GP46 goes through R18 to the transistor and the jack, and
 * through a 1 uF coupling cap to the MS9288A's audio input, and that is
 * the whole of it. Dropping the carrier to 246 kHz to buy three more
 * bits of duty was measured and was a bad trade -- the idle noise on the
 * HDMI path went from -72 dBFS to -35, because a full-scale square wave
 * at 246 kHz aliases straight into the audible band and nothing on the
 * board stops it.
 *
 * So the resolution comes from time instead. The duty is rewritten eight
 * times per audio sample, and each rewrite is quantised with the error
 * from the last one fed back into it -- first-order noise shaping at
 * 352.8 kHz. That does not make the quantisation noise smaller, it moves
 * it: the feedback loop is a high-pass on the error, so the noise ends
 * up near 176 kHz instead of spread flat across the audio band. Which is
 * safe where the carrier was not, because this noise is one part in 256
 * of full scale rather than all of it.
 */
#define PWM_BITS  8u
#define PWM_WRAP  ((1u << PWM_BITS) - 1u)
#define PWM_MID   (1u << (PWM_BITS - 1))
#define PWM_SHIFT (16u - PWM_BITS)

// How many times the duty is rewritten per audio sample. Eight is three
// octaves of oversampling, which a first-order shaper turns into about
// twenty-seven decibels in the part of the band that matters.
#define AUDIO_OSR 8u

static uint  slice, chan;
static int   dma_a = -1, dma_b = -1;
static int   pacer = -1;
static bool  running;
static volatile bool enabled = true;

static uint16_t buf[2][AUDIO_BLOCK];
static volatile int refill = -1;

volatile uint32_t audio_underruns;
volatile uint32_t audio_overruns;
volatile uint32_t audio_clips;

// The quantisation error carried from the previous sample. See audio_task().
static int32_t shape_err;

// ---------------------------------------------------------------------------
// The ring between the cores
// ---------------------------------------------------------------------------
//
// Core 1 writes, core 0 reads, and neither locks: one producer, one
// consumer, and a power-of-two size so head and tail can run free and be
// masked. 4096 samples is 93 ms, which is far more slack than either side
// needs — the point of the depth is not latency, it is that core 1 makes
// samples in 20 ms bursts and core 0 takes them in 5.8 ms ones.
#define RING_SIZE AUDIO_RING_SIZE
#define RING_MASK (RING_SIZE - 1u)

static int16_t           ring[RING_SIZE];
static volatile uint32_t ring_head;    // core 1
static volatile uint32_t ring_tail;    // core 0
static int16_t           last_sample;  // held through an underrun

uint32_t audio_queued(void) { return ring_head - ring_tail; }

void audio_push(const int32_t sample) {
    const uint32_t h = ring_head;
    if (h - ring_tail >= RING_SIZE) { audio_overruns++; return; }

    int32_t s = sample;
    if (s >  32767) s =  32767;
    if (s < -32768) s = -32768;

    ring[h & RING_MASK] = (int16_t)s;
    __asm volatile ("" ::: "memory");   // sample before the index publishing it
    ring_head = h + 1;
}

void audio_set_enabled(const bool on) { enabled = on; }

// ---------------------------------------------------------------------------
// Moving them out
// ---------------------------------------------------------------------------

static void dma_isr(void) {
    if (dma_hw->ints1 & (1u << dma_a)) {
        dma_hw->ints1 = 1u << dma_a;
        dma_channel_set_read_addr(dma_a, buf[0], false);
        refill = 0;
    }
    if (dma_hw->ints1 & (1u << dma_b)) {
        dma_hw->ints1 = 1u << dma_b;
        dma_channel_set_read_addr(dma_b, buf[1], false);
        refill = 1;
    }
}

uint32_t audio_ring_level(void) { return ring_head - ring_tail; }

void audio_reset_stats(void) {
    audio_underruns = audio_overruns = audio_clips = 0;
}

void audio_task(void) {
    const int which = refill;
    if (which < 0) return;
    refill = -1;

    const bool mute = !enabled;

    uint32_t t = ring_tail;
    for (uint32_t i = 0; i < AUDIO_BLOCK; i++) {
        // One input sample feeds AUDIO_OSR outputs. The shaper below is
        // what turns those eight identical values into eight different
        // duties whose average is finer than any one of them.
        if ((i % AUDIO_OSR) != 0) {
            /* keep last_sample */
        } else if (t != ring_head) {
            last_sample = ring[t & RING_MASK];
            t++;
        } else if (!mute) {
            /*
             * Hold, do not silence. See audio.h.
             *
             * Not counted while muted: the machine is paused then and
             * produces nothing at all, so the ring is empty by
             * definition and counting it swamps the figure that matters
             * -- how often the running machine falls behind.
             */
            audio_underruns++;
        }

        /*
         * Sixteen bits down to eleven, with the error carried forward.
         *
         * A plain shift throws five bits away and leaves their noise
         * spread flat across the band. Adding what was thrown away back
         * into the next sample -- first-order error feedback -- does not
         * make the noise smaller, it moves it: the loop is a high-pass
         * on the error, so most of it ends up near the sample rate where
         * nothing can hear it and the transducer cannot reproduce it.
         * A few adds per sample for several decibels in the part of the
         * band that matters.
         */
        int32_t v = last_sample + shape_err;
        if (v >  32767) v =  32767;
        if (v < -32768) v = -32768;

        // Clipped rather than wrapped: a wrap turns a loud passage into
        // noise, and a clip at least sounds like the thing that was too
        // loud. Counted, because "is it clipping" should not be a guess.
        int32_t s = v >> PWM_SHIFT;
        const int32_t hi = (int32_t)PWM_MID - 1, lo = -(int32_t)PWM_MID;
        if (s > hi) { s = hi; audio_clips++; }
        if (s < lo) { s = lo; audio_clips++; }

        shape_err = v - (s << PWM_SHIFT);
        // Muting writes the idle level but still consumes the samples:
        // stopping the drain would back the ring up and make un-muting
        // play a second of stale audio.
        buf[which][i] = mute ? (uint16_t)PWM_MID : (uint16_t)(s + (int32_t)PWM_MID);
    }
    ring_tail = t;
}

// ---------------------------------------------------------------------------
// Bring-up
// ---------------------------------------------------------------------------

void audio_init(void) {
    slice = pwm_gpio_to_slice_num(SPEAKER_PIN);
    chan  = pwm_gpio_to_channel(SPEAKER_PIN);

    gpio_set_function(SPEAKER_PIN, GPIO_FUNC_PWM);

    pwm_config c = pwm_get_default_config();
    pwm_config_set_clkdiv_int(&c, 1);
    pwm_config_set_wrap(&c, PWM_WRAP);
    pwm_init(slice, &c, true);
    pwm_set_chan_level(slice, chan, PWM_MID);

    for (int i = 0; i < 2; i++)
        for (uint32_t s = 0; s < AUDIO_BLOCK; s++) buf[i][s] = PWM_MID;

    // A DMA pacing timer rather than a PWM wrap interrupt, so nothing
    // needs servicing per sample: the two channels chain to each other
    // for ever and only raise an interrupt when a block has drained.
    pacer = dma_claim_unused_timer(true);
    // Paced at the oversampled rate: the duty is rewritten AUDIO_OSR
    // times per audio sample, which is where the extra resolution comes
    // from. See the top of this file.
    const uint32_t out_hz = AUDIO_RATE_HZ * AUDIO_OSR;
    const uint32_t den = (clock_get_hz(clk_sys) + out_hz / 2) / out_hz;
    dma_timer_set_fraction(pacer, 1, (uint16_t)(den > 0xFFFFu ? 0xFFFFu : den));

    dma_a = dma_claim_unused_channel(true);
    dma_b = dma_claim_unused_channel(true);

    volatile void *cc = &((uint16_t *)&pwm_hw->slice[slice].cc)[chan];

    for (int i = 0; i < 2; i++) {
        const int ch    = i ? dma_b : dma_a;
        const int other = i ? dma_a : dma_b;
        dma_channel_config cfg = dma_channel_get_default_config(ch);
        channel_config_set_transfer_data_size(&cfg, DMA_SIZE_16);
        channel_config_set_read_increment(&cfg, true);
        channel_config_set_write_increment(&cfg, false);
        channel_config_set_dreq(&cfg, dma_get_timer_dreq(pacer));
        channel_config_set_chain_to(&cfg, other);
        dma_channel_configure(ch, &cfg, cc, buf[i], AUDIO_BLOCK, false);
        dma_channel_set_irq1_enabled(ch, true);
    }

    irq_set_exclusive_handler(DMA_IRQ_1, dma_isr);
    irq_set_enabled(DMA_IRQ_1, true);

    dma_channel_start(dma_a);
    running = true;

    printf("[audio] PWM DAC on GP%d: %u-bit duty at %lu Hz carrier, rewritten %lu times a second\n",
           SPEAKER_PIN, (unsigned)PWM_BITS,
           (unsigned long)(clock_get_hz(clk_sys) / (PWM_WRAP + 1u)),
           (unsigned long)(clock_get_hz(clk_sys) / den));
}
