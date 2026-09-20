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
// The carrier
// ---------------------------------------------------------------------------
//
// 8-bit duty at clk_sys/256 — about 1.5 MHz at 378 MHz of system clock.
// Well above anything audible, and well above what C67/C68 and the
// MS9288A's analogue input will pass, so the pin's average is what comes
// out and the carrier itself does not.
#define PWM_WRAP 255u
#define PWM_MID  128u

static uint  slice, chan;
static int   dma_a = -1, dma_b = -1;
static int   pacer = -1;
static bool  running;
static volatile bool enabled = true;

static uint16_t buf[2][AUDIO_BLOCK];
static volatile int refill = -1;

volatile uint32_t audio_underruns;
volatile uint32_t audio_overruns;

// ---------------------------------------------------------------------------
// The ring between the cores
// ---------------------------------------------------------------------------
//
// Core 1 writes, core 0 reads, and neither locks: one producer, one
// consumer, and a power-of-two size so head and tail can run free and be
// masked. 4096 samples is 93 ms, which is far more slack than either side
// needs — the point of the depth is not latency, it is that core 1 makes
// samples in 20 ms bursts and core 0 takes them in 5.8 ms ones.
#define RING_SIZE 4096u
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

void audio_task(void) {
    const int which = refill;
    if (which < 0) return;
    refill = -1;

    const bool mute = !enabled;

    uint32_t t = ring_tail;
    for (uint32_t i = 0; i < AUDIO_BLOCK; i++) {
        if (t != ring_head) {
            last_sample = ring[t & RING_MASK];
            t++;
        } else {
            // Hold, do not silence. See audio.h.
            audio_underruns++;
        }

        // Signed 16-bit down to an 8-bit duty around mid-scale, clipped
        // rather than wrapped: a wrap turns a loud passage into noise,
        // and a clip at least sounds like the thing that was too loud.
        int32_t s = last_sample >> 8;
        if (s >  127) s =  127;
        if (s < -128) s = -128;
        // Muting writes the idle level but still consumes the samples:
        // stopping the drain would back the ring up and make un-muting
        // play a second of stale audio.
        buf[which][i] = mute ? PWM_MID : (uint16_t)(s + PWM_MID);
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
    const uint32_t den = (clock_get_hz(clk_sys) + AUDIO_RATE_HZ / 2) / AUDIO_RATE_HZ;
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

    printf("[audio] PWM DAC on GP%d, %lu Hz, %u-sample blocks\n",
           SPEAKER_PIN, (unsigned long)(clock_get_hz(clk_sys) / den), (unsigned)AUDIO_BLOCK);
}
