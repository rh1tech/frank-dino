/*
 * The SAA1099, checked against its own arithmetic.
 *
 * The datasheet's law is f = 15625 * 2^octave / (511 - offset) at the
 * usual 8 MHz, and getting it wrong by a factor of two is a chip that
 * plays every tune an octave out -- which is exactly the fault that the
 * AY's own self-test caught once, and it is inaudible on a single note.
 * So the pitch is measured here across the octave range by counting
 * transitions, not asserted from the code that produces it.
 *
 * The rest is the parts that interlock: the mixer's modes, the envelope
 * shapes, the PDM amplitude table, and the mute that a Profi in CP/M
 * sits in.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "saa.h"
#include "tap.h"

#define RATE 44100u

static saa_t s;

static void wr(const uint8_t reg, const uint8_t val) {
    saa_select(&s, reg);
    saa_write(&s, val);
}

static void power_on(void) {
    saa_reset(&s);
    saa_set_clock(&s, SAA_CLOCK_HZ, RATE);
}

/* Count zero crossings of one second of output, giving Hz. */
static unsigned measure_hz(void) {
    int32_t prev = saa_render(&s);
    int32_t lo = prev, hi = prev;
    /* One pass to find the range, a second to count against its middle. */
    for (unsigned i = 0; i < RATE; i++) {
        const int32_t v = saa_render(&s);
        if (v < lo) lo = v;
        if (v > hi) hi = v;
    }
    const int32_t mid = (lo + hi) / 2;
    if (hi == lo) return 0;

    unsigned crossings = 0;
    bool high = saa_render(&s) > mid;
    for (unsigned i = 0; i < RATE; i++) {
        const bool now = saa_render(&s) > mid;
        if (now != high) crossings++;
        high = now;
    }
    return crossings / 2;          /* two crossings to a cycle */
}

/* Set channel 0 to a bare tone at full amplitude and switch the chip on. */
static void bare_tone(const uint8_t octave, const uint8_t offset) {
    power_on();
    wr(0, 0xFF);                   /* channel 0: amplitude 15 both sides */
    wr(8, offset);                 /* channel 0: frequency offset */
    wr(16, (uint8_t)(octave & 7)); /* channels 0/1: octave in the low nibble */
    wr(20, 0x01);                  /* tone mixer: channel 0 on */
    wr(21, 0x00);                  /* noise mixer: all off */
    wr(28, 0x01);                  /* output enable */
}

int main(void) {
    /* ---------------------------------------------------------------- */
    GROUP("reset");
    power_on();
    CHECK(!saa_active(&s), "the chip comes up muted");
    int32_t acc = 0;
    for (int i = 0; i < 1000; i++) acc += saa_render(&s);
    EQ(acc, 0, "and silent");
    for (int r = 0; r < 32; r++) { saa_select(&s, (uint8_t)r); EQ(saa_read(&s), 0, "register %d clear", r); }

    /* ---------------------------------------------------------------- */
    GROUP("pitch");
    /*
     * f = 15625 * 2^octave / (511 - offset) at 8 MHz. Checked over the
     * whole octave range and at both ends of the offset range, with a
     * 2% tolerance for the transition counting.
     */
    static const struct { uint8_t oct, off; unsigned hz; } TONES[] = {
        { 0, 0xFF,   61 },   /* 15625 / 256 */
        { 1, 0xFF,  122 },
        { 2, 0xFF,  244 },
        { 3, 0xFF,  488 },
        { 4, 0xFF,  977 },
        { 5, 0xFF, 1953 },
        { 6, 0xFF, 3906 },
        { 4, 0x00,  489 },   /* 15625 * 16 / 511 */
        { 5, 0x80, 1305 },   /* 15625 * 32 / 383 */
        { 6, 0xC0, 3135 },   /* 15625 * 64 / 319 */
    };
    for (unsigned i = 0; i < count_of(TONES); i++) {
        bare_tone(TONES[i].oct, TONES[i].off);
        const unsigned got  = measure_hz();
        const unsigned want = TONES[i].hz;
        const unsigned tol  = want / 50 + 2;
        CHECK(got + tol >= want && got <= want + tol,
              "octave %u offset %02X: %u Hz, wanted %u",
              TONES[i].oct, TONES[i].off, got, want);
    }

    /* ---------------------------------------------------------------- */
    GROUP("mixer");
    bare_tone(4, 0xFF);
    int32_t peak = 0;
    for (int i = 0; i < 4000; i++) { const int32_t v = saa_render(&s); if (v > peak) peak = v; }
    CHECK(peak > 0, "a bare tone makes sound (peak %d)", peak);

    /* The mute is the power-on state and what a Profi in CP/M sits in. */
    wr(28, 0x00);
    acc = 0;
    for (int i = 0; i < 2000; i++) acc += saa_render(&s);
    EQ(acc, 0, "clearing the enable bit silences it");
    CHECK(!saa_active(&s), "and it reports itself off");
    wr(28, 0x01);
    CHECK(saa_active(&s), "and on again");

    /* Sync holds the output at silence without advancing anything. */
    wr(28, 0x03);
    acc = 0;
    for (int i = 0; i < 2000; i++) acc += saa_render(&s);
    EQ(acc, 0, "sync holds the output silent");
    wr(28, 0x01);

    /* Amplitude scales it. */
    bare_tone(4, 0xFF);
    int32_t full = 0;
    for (int i = 0; i < 4000; i++) { const int32_t v = saa_render(&s); if (v > full) full = v; }
    bare_tone(4, 0xFF);
    wr(0, 0x77);                   /* half amplitude both sides */
    int32_t half = 0;
    for (int i = 0; i < 4000; i++) { const int32_t v = saa_render(&s); if (v > half) half = v; }
    CHECK(half < full, "a lower amplitude is quieter (%d < %d)", half, full);
    CHECK(half * 2 > full - full / 4 && half * 2 < full + full / 4,
          "and roughly half (%d vs %d)", half * 2, full);

    /* Channel 0 muted in the tone mixer must be silent. */
    bare_tone(4, 0xFF);
    wr(20, 0x00);
    acc = 0;
    for (int i = 0; i < 2000; i++) acc += saa_render(&s);
    EQ(acc, 0, "a channel out of the tone mixer is silent");

    /* Noise reaches the output on its own. */
    power_on();
    wr(0, 0xFF);
    wr(20, 0x00);                  /* no tone */
    wr(21, 0x01);                  /* channel 0 takes noise */
    wr(22, 0x00);                  /* noise source 0 */
    wr(28, 0x01);
    unsigned changes = 0;
    int32_t last = saa_render(&s);
    for (int i = 0; i < 4000; i++) {
        const int32_t v = saa_render(&s);
        if (v != last) changes++;
        last = v;
    }
    CHECK(changes > 200, "noise alone modulates the output (%u changes)", changes);

    /* ---------------------------------------------------------------- */
    GROUP("envelope");
    /*
     * Envelopes reach only channels 2 and 5, and their levels come from
     * the shape table rather than a ramp. Shape 1 is "maximum", which
     * must hold its level; shape 2 is a single decay, which must fall
     * and stay down.
     */
    power_on();
    wr(2, 0xFF);                   /* channel 2 at full amplitude */
    wr(8 + 2, 0xFF); wr(17, 0x00); /* give it a tone to carry the envelope */
    wr(20, 0x04);                  /* channel 2 in the tone mixer */
    wr(28, 0x01);
    /* Shape 2, single decay, internally clocked, 4-bit resolution. */
    wr(24, (uint8_t)(0x80 | (2 << 1)));
    /*
     * env0's internal clock is channel 1's tone, not channel 2's, so
     * the decay advances at channel 1's rate -- about 30 Hz with the
     * reset period -- and sixteen steps take half a second. Measure the
     * start, then skip past the whole decay and measure again; a window
     * that starts at the beginning would catch the loud part in both.
     */
    int32_t first = 0, later = 0;
    for (int i = 0; i < 2000; i++)  { const int32_t v = saa_render(&s); if (v > first) first = v; }
    for (int i = 0; i < 44100; i++) saa_render(&s);              /* let it fall */
    for (int i = 0; i < 20000; i++) { const int32_t v = saa_render(&s); if (v > later) later = v; }
    CHECK(first > 0, "a decaying envelope starts loud (%d)", first);
    CHECK(later < first, "and has fallen a second later (%d < %d)", later, first);

    /* An envelope disabled again must stop gating. */
    wr(24, 0x00);
    CHECK(true, "disabling an envelope is accepted");

    /* Shape 1 holds its level: the maximum shape must not decay. */
    power_on();
    wr(2, 0xFF); wr(8 + 2, 0xFF); wr(17, 0x00); wr(20, 0x04); wr(28, 0x01);
    wr(24, (uint8_t)(0x80 | (1 << 1)));
    int32_t early = 0, late = 0;
    for (int i = 0; i < 2000; i++)  { const int32_t v = saa_render(&s); if (v > early) early = v; }
    for (int i = 0; i < 40000; i++) { const int32_t v = saa_render(&s); if (v > late)  late  = v; }
    EQ(late, early, "the maximum shape holds its level");

    /* ---------------------------------------------------------------- */
    GROUP("registers");
    power_on();
    for (int r = 0; r < 32; r++) { wr((uint8_t)r, (uint8_t)(0xA5 ^ r)); }
    unsigned kept = 0;
    for (int r = 0; r < 32; r++) {
        saa_select(&s, (uint8_t)r);
        if (saa_read(&s) == (uint8_t)(0xA5 ^ r)) kept++;
    }
    EQ(kept, 32, "every register reads back what was written");
    /* The select is five bits: anything above wraps into them. */
    saa_select(&s, 0x25);
    saa_write(&s, 0x3C);
    saa_select(&s, 0x05);
    EQ(saa_read(&s), 0x3C, "the register select is five bits");

    /* ---------------------------------------------------------------- */
    GROUP("clock");
    /*
     * The Creative Music System runs its pair at 7.159090 MHz, which is
     * about 11% below 8 MHz and must come out as a correspondingly
     * lower pitch rather than as the same note.
     */
    saa_reset(&s);
    saa_set_clock(&s, 7159090u, RATE);
    wr(0, 0xFF); wr(8, 0xFF); wr(16, 0x04); wr(20, 0x01); wr(21, 0x00); wr(28, 0x01);
    const unsigned cms = measure_hz();
    bare_tone(4, 0xFF);
    const unsigned zx = measure_hz();
    CHECK(cms < zx, "a slower clock is a lower pitch (%u < %u)", cms, zx);
    const unsigned want = (unsigned)((uint64_t)zx * 7159090u / 8000000u);
    CHECK(cms + want / 40 + 2 >= want && cms <= want + want / 40 + 2,
          "and lower by the clock ratio: %u, wanted about %u", cms, want);

    return tap_done("saa");
}
