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

#include <string.h>

#include <hardware/gpio.h>
#include <hardware/uart.h>
#include <pico/stdlib.h>
#include <pico/time.h>

#include "dino.h"

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

    const uint32_t at = fill_bit;
    if ((at >> 3) >= TAPEIN_BUF_BYTES) return true;     /* frame ran long */

    const uint8_t v = gpio_get(TAPEIN_PIN) ? 1u : 0u;

    unsigned wait = 20;
    while (lock_in && wait) --wait;

    uint8_t *const p = (a_in ? buf_b : buf_a) + (at >> 3);
    const unsigned bit = at & 7u;
    /* The first bit of a byte writes it rather than ORs into it, which
     * is what clears the previous frame's contents as it goes. */
    *p = (uint8_t)((bit == 0 ? 0u : *p) | ((unsigned)v << bit));

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
     * Take the pin off the UART properly.
     *
     * gpio_init() alone points the mux at SIO, which is enough to READ
     * the pin -- but the UART is still enabled behind it with this as
     * its RX, and anything that re-runs stdio setup takes it back. So
     * the receive side is shut down first and the pin claimed second.
     * The console can still print: that is GP44, which is untouched.
     *
     * No pull, because the comparator on J9 drives the line both ways.
     * A pull would also hide the one fault worth seeing -- a pin that
     * nothing is driving at all, which tapein_probe() tests for.
     */
    uart_set_fifo_enabled(uart0, false);
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
    /* The pin goes back to the UART, and the receive side with it. */
    gpio_set_function(TAPEIN_PIN, GPIO_FUNC_UART);
    uart_set_fifo_enabled(uart0, true);
}

/*
 * Is anything actually driving the pin?
 *
 * The fault this exists for is silence: a sampler that runs perfectly
 * and reads the same level for ever tells you nothing about whether it
 * is looking at the right pin. A line a comparator is driving holds its
 * level against an internal pull; a line nothing is connected to
 * follows the pull wherever it is put. That distinction is the whole
 * test, and it is the one pico-speccy's own pin-connection probes make.
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

void tapein_frame_started(const uint32_t tstates) {
    if (!running) return;

    frame_bits    = fill_bit;
    frame_tstates = tstates ? tstates : 1u;

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
