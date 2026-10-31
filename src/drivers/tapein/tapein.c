/*
 * frank-dino — an RP2350B acting as the whole machine around a real Z80
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Ported from pico-speccy's LoadWavStream.h and the pcm_data_in() half
 * of pwm_audio.cpp (GPL-3.0-or-later).
 */

#include "tapein.h"

#include <stdio.h>
#include <string.h>

#include <hardware/gpio.h>
#include <pico/stdlib.h>
#include <pico/time.h>

#include "dino.h"

#include <hardware/structs/timer.h>

/*
 * Their MAX_IN_SAMPLES, and their arithmetic for it: 128+128+128 bytes
 * is 3072 bits, and a frame at 125 kHz and fifty frames a second is
 * 2500 of them. The headroom is what absorbs a frame that runs long --
 * this machine's do, because the clock tuner lands about one and a half
 * per cent short of 3.5469 MHz and a frame really takes 20.3 ms.
 */
#define TAPEIN_BUF_BYTES (128 + 128 + 128)

static uint8_t  buf_a[TAPEIN_BUF_BYTES];
static uint8_t  buf_b[TAPEIN_BUF_BYTES];
static volatile bool     a_in;          /* which buffer is being FILLED */
static volatile bool     lock_in;       /* the swap is in progress */
static volatile uint32_t fill_bit;      /* bits written into this frame */
static uint32_t frame_bits;             /* what the last frame collected */
static uint32_t frame_tstates;          /* how long this frame is */

static repeating_timer_t timer;
static bool running;

/*
 * What the line has been doing lately, counted as it arrives.
 *
 * Measuring this from the buffer afterwards would mean walking 2500
 * bits, and the only place that could happen is the frame boundary on
 * core 1 -- which is the one place on this machine with nothing to
 * spare. Counting a transition as it is sampled is two compares.
 */
static volatile uint32_t tr_count;      /* transitions since last read */
static volatile uint32_t low_count;     /* samples that were low */
static volatile uint32_t all_count;     /* samples taken */
static volatile uint32_t run_sum;       /* total length of closed runs */
static volatile uint32_t run_n;         /* how many closed runs */
static uint32_t cur_run;
static uint8_t  last_level;

/*
 * Whether the samples arrive on time, for tools/dino.py: the longest gap
 * between two ticks in microseconds, how many gaps were over 12 us (a
 * sample lost), and the fewest and most samples a frame collected.
 */
volatile uint32_t tapein_gap_max_us, tapein_gaps_late;
volatile uint32_t tapein_frame_min = 0xFFFFFFFFu, tapein_frame_max;
/* Levels that lasted one or two samples (8-16 us): a chattering edge. */
volatile uint32_t tapein_glitch1, tapein_glitch2;
static uint32_t   last_tick_us;

/* Debounce the level the machine sees (0 = the raw pin), and a capture of
 * the raw pin into the PSRAM -- both for tools/dino.py. */
volatile uint8_t  tapein_debounce = 3;
#define TAPEIN_CAP_BITS (8u * 1024u * 1024u)
uint8_t           tapein_cap[TAPEIN_CAP_BITS / 8u] __attribute__((section(".psram")));
volatile uint8_t  tapein_cap_on;
volatile uint32_t tapein_cap_bits;

uint8_t tapein_probe(void);

/*
 * One sample, from the timer.
 *
 * Theirs, including the spin on lock_in: the swap below is three stores
 * and this is the only other writer, so a bounded wait is cheaper and
 * more predictable than a critical section on a path that runs 125000
 * times a second. Bounded, because dropping one sample is a pulse edge
 * moved by 8 microseconds and the alternative is a timer callback that
 * can sit.
 */
static bool __not_in_flash_func(tapein_tick)(repeating_timer_t *rt) {
    (void)rt;

    const uint32_t now = timer0_hw->timerawl;
    const uint32_t gap = now - last_tick_us;
    last_tick_us = now;
    if (gap > 12u) tapein_gaps_late++;
    if (gap > tapein_gap_max_us && gap < 100000u) tapein_gap_max_us = gap;

    const uint32_t at = fill_bit;
    if ((at >> 3) >= TAPEIN_BUF_BYTES) return true;     /* frame ran long */

    /*
     * The level, debounced: a change counts once the pin has held it for
     * three samples. The shaper has no hysteresis, so a slow edge chatters:
     * a capture of a real tape had 7% of its half-waves 0-3 samples long,
     * each an extra edge to the ROM's loop and a byte lost. A Spectrum's
     * EAR input is filtered and never sees them. Every edge moves by the
     * same 24 us, so no pulse changes length; the shortest the ROM writes
     * is 244 us.
     */
    const uint8_t raw = gpio_get(TAPEIN_PIN) ? 1u : 0u;
    static uint8_t level, held;
    if (!tapein_debounce) { level = raw; held = 0; }
    else if (raw == level) held = 0;
    else if (++held >= tapein_debounce) { level = raw; held = 0; }
    const uint8_t v = raw;      // the statistics below are about the line itself

    /* The raw line, for tools: one bit a sample while armed. */
    if (tapein_cap_on && tapein_cap_bits < TAPEIN_CAP_BITS) {
        const uint32_t n = tapein_cap_bits;
        uint8_t *const c = &tapein_cap[n >> 3];
        *c = (uint8_t)(((n & 7u) ? *c : 0u) | (raw << (n & 7u)));
        tapein_cap_bits = n + 1;
    }

    unsigned wait = 20;
    while (lock_in && wait) --wait;

    all_count++;
    if (!v) low_count++;

    if (v != last_level) {
        tr_count++;
        if (cur_run == 1u) tapein_glitch1++;
        else if (cur_run == 2u) tapein_glitch2++;
        /* Runs of one sample are noise, and very long ones are silence;
         * neither says anything about the pitch of a tone. */
        if (cur_run >= 2u && cur_run <= 600u) { run_sum += cur_run; run_n++; }
        cur_run = 1;
        last_level = v;
    } else if (cur_run < 0xFFFFu) {
        cur_run++;
    }

    uint8_t *const p = (a_in ? buf_b : buf_a) + (at >> 3);
    const unsigned bit = at & 7u;
    /* The first bit of a byte writes it rather than ORs into it, which
     * is what clears the previous frame's contents as it goes. */
    *p = (uint8_t)((bit == 0 ? 0u : *p) | ((unsigned)level << bit));

    fill_bit = at + 1;
    return true;
}

void tapein_start(void) {
    if (running) return;

    memset(buf_a, 0, sizeof buf_a);
    memset(buf_b, 0, sizeof buf_b);
    a_in = true;
    lock_in = false;
    fill_bit = 0;
    frame_bits = 0;

    /*
     * No pull, because the shaper JP7 selects drives the line itself --
     * R63 on Q4's collector, or U15's output. A pull would also hide the
     * one fault worth seeing -- a pin that nothing is driving at all,
     * which tapein_probe() tests for.
     */
    gpio_set_function(TAPEIN_PIN, GPIO_FUNC_SIO);
    gpio_init(TAPEIN_PIN);
    gpio_set_dir(TAPEIN_PIN, GPIO_IN);

    /* Ask once, on the way in, whether anything is out there. A silent
     * input and an unconnected one look identical afterwards. */
    (void)tapein_probe();

    /* Negative period: fixed rate between starts, not between end and
     * start, so a late callback does not push the whole train along. */
    running = add_repeating_timer_us(-(int64_t)(1000000 / TAPEIN_SAMPLE_HZ),
                                     tapein_tick, NULL, &timer);
}

void tapein_stop(void) {
    if (!running) return;
    cancel_repeating_timer(&timer);
    running = false;
}

/*
 * Is anything actually driving the pin?
 *
 * The fault this exists for is silence: a sampler that runs perfectly
 * and reads the same level for ever tells you nothing about whether it
 * is looking at the right pin. A line something is driving holds its
 * level against an internal pull; a line nothing is connected to
 * follows the pull wherever it is put. That distinction is the whole
 * test, and it is the one pico-speccy's own pin-connection probes make.
 * On this board it mostly answers for JP7: with neither side of it
 * bridged, GP45 is connected to nothing.
 *
 * Bit 0: the level with a pull-DOWN. Bit 1: with a pull-UP.
 *   0b10  follows the pulls   -- floating, nothing is driving it
 *   0b11  stays high          -- something is holding it high
 *   0b00  stays low           -- something is holding it low
 * Then the pin is left as it was.
 */
uint8_t tapein_last_probe;

uint8_t tapein_probe(void) {
    gpio_set_function(TAPEIN_PIN, GPIO_FUNC_SIO);
    gpio_init(TAPEIN_PIN);
    gpio_set_dir(TAPEIN_PIN, GPIO_IN);

    gpio_pull_down(TAPEIN_PIN);
    sleep_ms(2);
    const uint8_t low = gpio_get(TAPEIN_PIN) ? 1u : 0u;

    gpio_pull_up(TAPEIN_PIN);
    sleep_ms(2);
    const uint8_t high = gpio_get(TAPEIN_PIN) ? 1u : 0u;

    gpio_disable_pulls(TAPEIN_PIN);
    /* Four milliseconds of disturbance. The sampler, if it is running,
     * loses half a pulse of a header tone and nothing notices. */
    tapein_last_probe = (uint8_t)(low | (high << 1));
    return tapein_last_probe;
}

bool tapein_running(void) { return running; }

void tapein_stats(char *out, const unsigned len) {
    if (!running) { snprintf(out, len, "off"); return; }

    const uint32_t tr = tr_count, sum = run_sum, n = run_n;
    const uint32_t lo = low_count, all = all_count;
    tr_count = 0; run_sum = 0; run_n = 0; low_count = 0; all_count = 0;

    if (tapein_last_probe == 2u) { snprintf(out, len, "nothing connected"); return; }

    /*
     * How much of the time the line is being pulled down.
     *
     * This is the number to turn the volume by. The shaper idles
     * high, so silence is 0% and a signal big enough to swing it fully
     * is near 50%. Anything in between is a level too low to load with,
     * and -- unlike a frequency, which needs a signal before it means
     * anything -- it moves as soon as the peaks start crossing. Which
     * makes it the one reading that is useful while there is nothing
     * to read.
     */
    const unsigned pct = all ? (unsigned)((lo * 100u) / all) : 0u;

    if (!n || tr < 8u) { snprintf(out, len, "idle, %u%% low", pct); return; }

    /*
     * A run is half a cycle, so the frequency is half the reciprocal of
     * its length. The named ones are what the ROM is listening for: its
     * leader is 807 Hz and its bits are about 2.1 kHz.
     */
    const uint32_t half_us = (sum * 1000000u / TAPEIN_SAMPLE_HZ) / n;
    const uint32_t hz = half_us ? 500000u / half_us : 0u;

    const char *what = (hz > 700u  && hz < 950u)  ? " header"
                     : (hz > 1700u && hz < 2600u) ? " data"
                                                  : "";
    snprintf(out, len, "%lu Hz%s, %u%% low", (unsigned long)hz, what, pct);
}

void tapein_frame_started(const uint32_t tstates) {
    if (!running) return;

    frame_bits    = fill_bit;
    frame_tstates = tstates ? tstates : 1u;
    if (frame_bits < tapein_frame_min) tapein_frame_min = frame_bits;
    if (frame_bits > tapein_frame_max) tapein_frame_max = frame_bits;

    lock_in  = true;
    a_in     = !a_in;
    fill_bit = 0;
    lock_in  = false;
}

bool __not_in_flash_func(tapein_level)(const uint32_t tstates) {
    if (!running || !frame_bits) return false;

    /*
     * Where in the previous frame's buffer this moment falls.
     *
     * Theirs divides T-states elapsed by T-states in a frame and scales
     * by however many samples that frame actually collected -- not by
     * the nominal rate. That is what makes it survive a frame that ran
     * long or a timer that missed: the buffer is read across whatever
     * it holds, so the tape plays at the right speed either way.
     */
    uint32_t at = (uint32_t)(((uint64_t)tstates * frame_bits) / frame_tstates);
    if (at >= frame_bits) at = frame_bits - 1;

    const uint32_t idx = at >> 3;
    if (idx >= TAPEIN_BUF_BYTES) return false;

    return (((a_in ? buf_a : buf_b)[idx] >> (at & 7u)) & 1u) != 0u;
}
