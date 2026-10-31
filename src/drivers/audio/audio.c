/*
 * frank-dino — an RP2350B acting as the whole machine around a real Z80
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-dino
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The PWM/DMA half of this is frank-xt8086's audio driver, which solved
 * the same problem for a PC speaker and an OPL2 on one pin. What is
 * different here is where the samples come from -- there, core 0
 * rendered them on demand; here core 1 produces them in step with the
 * Z80 and this only moves them -- and that there are two pins.
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
 * The carrier has to stay up there. The only reconstruction filter on
 * this board is a single pole per side -- 1K and 10 nF ahead of each
 * LM358 follower, about 16 kHz -- which takes 20 dB a decade off a
 * carrier and nothing like enough off one that is close. Dropping the
 * carrier to 246 kHz to buy three more bits of duty was measured, on
 * the board before this one, and was a bad trade: the idle noise on the
 * HDMI path went from -72 dBFS to -35, because a full-scale square wave
 * at 246 kHz aliases straight into the audible band.
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

/*
 * One of these per pin.
 *
 * Each side is its own PWM slice -- GP46 is slice 11 and GP44 slice 10,
 * both on channel A -- so a single DMA transfer cannot write both duty
 * registers, and each side gets its own pair of chained channels. Both
 * pairs are paced by the same DMA timer and started in the same write,
 * so they take every request together and stay sample-locked for ever:
 * nothing re-aligns them because nothing can move them apart.
 */
typedef struct {
    uint     pin, slice, chan;
    int      dma_a, dma_b;
    uint16_t buf[2][AUDIO_BLOCK];
    int32_t  shape_err;     // the quantisation error carried forward
    int16_t  last;          // held through an underrun
} side_t;

enum { LEFT = 0, RIGHT = 1 };

static side_t side[2] = {
    [LEFT]  = { .pin = AUDIO_L_PIN, .dma_a = -1, .dma_b = -1 },
    [RIGHT] = { .pin = AUDIO_R_PIN, .dma_a = -1, .dma_b = -1 },
};

static int   pacer = -1;
static bool  running;
static volatile bool enabled = true;

static volatile int refill = -1;

volatile uint32_t audio_underruns;
volatile uint32_t audio_overruns;
volatile uint32_t audio_clips;

// ---------------------------------------------------------------------------
// The ring between the cores
// ---------------------------------------------------------------------------
//
// Core 1 writes, core 0 reads, and neither locks: one producer, one
// consumer, and a power-of-two size so head and tail can run free and be
// masked. 4096 samples is 93 ms, which is far more slack than either side
// needs — the point of the depth is not latency, it is that core 1 makes
// samples in 20 ms bursts and core 0 takes them in 5.8 ms ones.
//
// A slot is one stereo pair packed into a word, left in the low half, so
// that a pair is published by one store and can never be read torn.
#define RING_SIZE AUDIO_RING_SIZE
#define RING_MASK (RING_SIZE - 1u)

static uint32_t          ring[RING_SIZE];
static volatile uint32_t ring_head;    // core 1
static volatile uint32_t ring_tail;    // core 0

uint32_t audio_queued(void) { return ring_head - ring_tail; }

__force_inline static uint16_t clamp16(int32_t s) {
    if (s >  32767) s =  32767;
    if (s < -32768) s = -32768;
    return (uint16_t)(int16_t)s;
}

void audio_push(const int32_t left, const int32_t right) {
    const uint32_t h = ring_head;
    if (h - ring_tail >= RING_SIZE) { audio_overruns++; return; }

    ring[h & RING_MASK] = (uint32_t)clamp16(left) | ((uint32_t)clamp16(right) << 16);
    __asm volatile ("" ::: "memory");   // sample before the index publishing it
    ring_head = h + 1;
}

void audio_set_enabled(const bool on) { enabled = on; }

// ---------------------------------------------------------------------------
// Moving them out
// ---------------------------------------------------------------------------

/*
 * A block has drained: point its channel back at the start of it.
 *
 * Every channel re-arms itself, but only the left side says which block
 * wants refilling. The right side drains the same block on the same
 * DMA request, so by the time core 0 gets round to refilling, both
 * halves of it are free.
 */
static void __not_in_flash_func(dma_isr)(void) {
    for (int k = 0; k < 2; k++) {
        side_t *const sd = &side[k];
        if (dma_hw->ints1 & (1u << sd->dma_a)) {
            dma_hw->ints1 = 1u << sd->dma_a;
            dma_channel_set_read_addr(sd->dma_a, sd->buf[0], false);
            if (k == LEFT) refill = 0;
        }
        if (dma_hw->ints1 & (1u << sd->dma_b)) {
            dma_hw->ints1 = 1u << sd->dma_b;
            dma_channel_set_read_addr(sd->dma_b, sd->buf[1], false);
            if (k == LEFT) refill = 1;
        }
    }
}

uint32_t audio_ring_level(void) { return ring_head - ring_tail; }

void audio_reset_stats(void) {
    audio_underruns = audio_overruns = audio_clips = 0;
}

/*
 * Sixteen bits down to eight, with the error carried forward.
 *
 * A plain shift throws eight bits away and leaves their noise spread
 * flat across the band. Adding what was thrown away back into the next
 * sample -- first-order error feedback -- does not make the noise
 * smaller, it moves it: the loop is a high-pass on the error, so most of
 * it ends up near the sample rate where nothing can hear it and the
 * transducer cannot reproduce it. A few adds per sample for several
 * decibels in the part of the band that matters.
 */
__force_inline static uint16_t shape(side_t *const sd) {
    int32_t v = sd->last + sd->shape_err;
    if (v >  32767) v =  32767;
    if (v < -32768) v = -32768;

    // Clipped rather than wrapped: a wrap turns a loud passage into
    // noise, and a clip at least sounds like the thing that was too
    // loud. Counted, because "is it clipping" should not be a guess.
    int32_t s = v >> PWM_SHIFT;
    const int32_t hi = (int32_t)PWM_MID - 1, lo = -(int32_t)PWM_MID;
    if (s > hi) { s = hi; audio_clips++; }
    if (s < lo) { s = lo; audio_clips++; }

    sd->shape_err = v - (s << PWM_SHIFT);
    return (uint16_t)(s + (int32_t)PWM_MID);
}

void __not_in_flash_func(audio_task)(void) {
    const int which = refill;
    if (which < 0) return;
    refill = -1;

    const bool mute = !enabled;
    side_t *const l = &side[LEFT], *const r = &side[RIGHT];

    uint32_t t = ring_tail;
    for (uint32_t i = 0; i < AUDIO_BLOCK; i++) {
        // One input sample feeds AUDIO_OSR outputs. The shaper is what
        // turns those eight identical values into eight different
        // duties whose average is finer than any one of them.
        if ((i % AUDIO_OSR) != 0) {
            /* keep the last pair */
        } else if (t != ring_head) {
            const uint32_t pair = ring[t & RING_MASK];
            l->last = (int16_t)(pair & 0xFFFFu);
            r->last = (int16_t)(pair >> 16);
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

        const uint16_t dl = shape(l), dr = shape(r);
        // Muting writes the idle level but still consumes the samples:
        // stopping the drain would back the ring up and make un-muting
        // play a second of stale audio.
        l->buf[which][i] = mute ? (uint16_t)PWM_MID : dl;
        r->buf[which][i] = mute ? (uint16_t)PWM_MID : dr;
    }
    ring_tail = t;
}

// ---------------------------------------------------------------------------
// Bring-up
// ---------------------------------------------------------------------------

static void side_init(side_t *const sd) {
    sd->slice = pwm_gpio_to_slice_num(sd->pin);
    sd->chan  = pwm_gpio_to_channel(sd->pin);

    gpio_set_function(sd->pin, GPIO_FUNC_PWM);

    pwm_config c = pwm_get_default_config();
    pwm_config_set_clkdiv_int(&c, 1);
    pwm_config_set_wrap(&c, PWM_WRAP);
    pwm_init(sd->slice, &c, true);
    pwm_set_chan_level(sd->slice, sd->chan, PWM_MID);

    for (int i = 0; i < 2; i++)
        for (uint32_t s = 0; s < AUDIO_BLOCK; s++) sd->buf[i][s] = PWM_MID;

    sd->dma_a = dma_claim_unused_channel(true);
    sd->dma_b = dma_claim_unused_channel(true);

    volatile void *cc = &((uint16_t *)&pwm_hw->slice[sd->slice].cc)[sd->chan];

    for (int i = 0; i < 2; i++) {
        const int ch    = i ? sd->dma_b : sd->dma_a;
        const int other = i ? sd->dma_a : sd->dma_b;
        dma_channel_config cfg = dma_channel_get_default_config(ch);
        channel_config_set_transfer_data_size(&cfg, DMA_SIZE_16);
        channel_config_set_read_increment(&cfg, true);
        channel_config_set_write_increment(&cfg, false);
        channel_config_set_dreq(&cfg, dma_get_timer_dreq(pacer));
        channel_config_set_chain_to(&cfg, other);
        dma_channel_configure(ch, &cfg, cc, sd->buf[i], AUDIO_BLOCK, false);
        dma_channel_set_irq1_enabled(ch, true);
    }
}

/*
 * The rate the DAC really runs at, which is not quite AUDIO_RATE_HZ.
 *
 * The pacing timer divides the system clock by a whole number, and 504
 * MHz over 44100 x 8 is 1428.6 -- so it is 1429, and the DAC takes
 * 44086.8 samples a second. A machine that hands it 44100 a second
 * overfills the ring by thirteen a second, for ever. The frame pacer
 * asks this instead, and since the timer and time_us_64() run off the
 * same crystal the two cannot drift apart.
 */
static uint32_t pacer_den;

uint32_t audio_rate_millihz(void) {
    if (!pacer_den) return AUDIO_RATE_HZ * 1000u;
    return (uint32_t)(((uint64_t)clock_get_hz(clk_sys) * 1000u) / ((uint64_t)pacer_den * AUDIO_OSR));
}

void audio_init(void) {
    // A DMA pacing timer rather than a PWM wrap interrupt, so nothing
    // needs servicing per sample: each side's two channels chain to each
    // other for ever and only raise an interrupt when a block has
    // drained. One timer for both sides, so they cannot drift apart.
    pacer = dma_claim_unused_timer(true);
    // Paced at the oversampled rate: the duty is rewritten AUDIO_OSR
    // times per audio sample, which is where the extra resolution comes
    // from. See the top of this file.
    const uint32_t out_hz = AUDIO_RATE_HZ * AUDIO_OSR;
    const uint32_t den = (clock_get_hz(clk_sys) + out_hz / 2) / out_hz;
    dma_timer_set_fraction(pacer, 1, (uint16_t)(den > 0xFFFFu ? 0xFFFFu : den));
    pacer_den = den > 0xFFFFu ? 0xFFFFu : den;

    side_init(&side[LEFT]);
    side_init(&side[RIGHT]);

    irq_set_exclusive_handler(DMA_IRQ_1, dma_isr);
    irq_set_enabled(DMA_IRQ_1, true);

    // In one write, so both sides take the first request together.
    dma_start_channel_mask((1u << side[LEFT].dma_a) | (1u << side[RIGHT].dma_a));
    running = true;

    printf("[audio] PWM DACs on GP%d (L) and GP%d (R): %u-bit duty at %lu Hz carrier, "
           "rewritten %lu times a second\n",
           AUDIO_L_PIN, AUDIO_R_PIN, (unsigned)PWM_BITS,
           (unsigned long)(clock_get_hz(clk_sys) / (PWM_WRAP + 1u)),
           (unsigned long)(clock_get_hz(clk_sys) / den));
}
