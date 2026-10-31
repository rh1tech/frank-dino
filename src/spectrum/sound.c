/*
 * frank-dino — an RP2350B acting as the whole machine around a real Z80
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-dino
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "sound.h"

#include <limits.h>

#include <string.h>

#include "audio.h"
#include "ay.h"
#include "dino.h"
#include "saa.h"
#include "settings.h"
#include "spectrum.h"
#include "taperec.h"
#include "chips.h"

uint32_t          sndq[SNDQ_SIZE];
volatile uint32_t sndq_head, sndq_tail;
volatile uint32_t sndq_drops;

snd_mirror_t snd_mirror;

// ---------------------------------------------------------------------------
// The chips, which only core 0 touches
// ---------------------------------------------------------------------------
static ay_t    ay[2];
static saa_t   saa;
static uint8_t ay_sel;
static uint8_t covox  = 0x80;
static uint8_t sd[4]  = { 0x80, 0x80, 0x80, 0x80 };
static uint8_t specdrum = 0x80;
static saa_t   cms[2];                  // the CMS's pair, at the card's 7.16 MHz
static bool    fm_on = true;

/*
 * The Byte's KR580VI53 -- an 8253 -- after pico-speccy's pitWrite and
 * pitGenSound: three counters clocked at the CPU's 3.5 MHz, each a
 * square wave in mode 3. BCD counts are decimal, which is how the
 * Byte's own ROM programs every note.
 */
typedef struct {
    uint16_t count;             // the period, in PIT clocks
    uint32_t phase;             // PIT clocks into the current half, Q16
    uint8_t  rw, lsb, out;
    bool     lsb_loaded, bcd, active;
} pit_ch_t;
static pit_ch_t pit[3];
static uint32_t pit_ticks_q16;          // PIT clocks a sample, Q16

static void pit_write(const unsigned port, const uint8_t data) {
    if (port == 3) {
        const unsigned ch = data >> 6;
        if (ch >= 3) return;
        const unsigned rw = (data >> 4) & 3u;
        if (!rw) return;                        // a latch command: keep counting
        pit_ch_t *p = &pit[ch];
        p->active = false; p->lsb_loaded = false; p->out = 0; p->phase = 0;
        p->rw = (uint8_t)rw;
        p->bcd = (data & 1u) != 0;
        return;
    }
    pit_ch_t *p = &pit[port];
    uint16_t raw = 0;
    bool load = false;
    switch (p->rw) {
        case 1: raw = data; load = true; break;
        case 2: raw = (uint16_t)(data << 8); load = true; break;
        default:
            if (!p->lsb_loaded) { p->lsb = data; p->lsb_loaded = true; }
            else { raw = (uint16_t)(p->lsb | (data << 8)); p->lsb_loaded = false; load = true; }
            break;
    }
    if (!load) return;
    p->count = p->bcd ? (uint16_t)(((raw >> 12) & 15u) * 1000u + ((raw >> 8) & 15u) * 100u
                                   + ((raw >> 4) & 15u) * 10u + (raw & 15u))
                      : raw;
    p->phase = 0;
    p->out = 1;
    p->active = p->count >= 2;
}

/* How long each counter's output was high during one sample, summed. */
static int32_t pit_sample(void) {
    int32_t mix = 0;
    for (int c = 0; c < 3; c++) {
        pit_ch_t *p = &pit[c];
        if (!p->active) continue;
        const uint32_t half = (uint32_t)(p->count >> 1) << 16;
        uint32_t left = pit_ticks_q16, high = 0;
        while (left) {
            const uint32_t to_toggle = half - p->phase;
            if (to_toggle > left) {
                if (p->out) high += left;
                p->phase += left;
                left = 0;
            } else {
                if (p->out) high += to_toggle;
                left -= to_toggle;
                p->phase = 0;
                p->out ^= 1u;
            }
        }
        mix += (int32_t)(((uint64_t)high * 8000u) / pit_ticks_q16) - 4000;
    }
    return mix;
}
#define CMS_CLOCK_HZ 7159090u

/*
 * AY cycles per sample, in 16.16 fixed point, at the rate the DAC really
 * runs. A sample is a fixed slice of *time*, and what the chip does in
 * it does not depend on how fast the CPU beside it is clocked -- so the
 * turbo settings leave the music in tune.
 */
static uint32_t ay_ticks_q16;
static uint32_t ay_tick_frac;

// ---------------------------------------------------------------------------
// Where in the frame the renderer is
// ---------------------------------------------------------------------------
/*
 * Sample i of a frame covers T-states [end(i-1), end(i)), where
 * end(i) = (i+1) * len / spf. The beeper is integrated across that span:
 * `area` is how many of its T-states the speaker spent high.
 */
static struct {
    uint32_t len;       // T-states in the frame
    uint32_t spf;       // samples in the frame
    uint32_t idx;       // the next sample to make
    uint32_t start;     // where it starts
    bool     open;      // inside a frame
} fr;

/*
 * The MIC bit and the frame's place in absolute time, for the tape
 * recorder: SAVE's pulses span frames, so their lengths are differences
 * of absolute T-states.
 */
static bool     mic_hi;
static uint64_t frame_abs;

static bool     beep_hi;
static uint32_t beep_t;          // the area is counted up to here
static uint32_t beep_area;

static volatile bool test_tone_on;
static uint32_t      test_tone_phase;

#define TEST_TONE_HZ      441u
#define TEST_TONE_SAMPLES (AUDIO_RATE_HZ / TEST_TONE_HZ)   /* exactly 100 */

/*
 * A sine and not a square, because this is a measuring instrument.
 *
 * A square wave's harmonics run past Nyquist and fold back, so a
 * spectrum of one is dense whatever the DAC does and there is no way to
 * tell the folding from the distortion. A sine puts everything in one
 * bin and makes every other bin evidence.
 *
 * Amplitude 8000 of 32767, which is -12 dBFS: loud enough to measure,
 * quiet enough that nothing downstream clips.
 */
static const int16_t test_tone_wave[TEST_TONE_SAMPLES] = {
         0,    502,   1003,   1499,   1990,   2472,   2945,   3406,   3854,   4287,
      4702,   5099,   5476,   5832,   6164,   6472,   6755,   7010,   7239,   7438,
      7608,   7749,   7858,   7937,   7984,   8000,   7984,   7937,   7858,   7749,
      7608,   7438,   7239,   7010,   6755,   6472,   6164,   5832,   5476,   5099,
      4702,   4287,   3854,   3406,   2945,   2472,   1990,   1499,   1003,    502,
         0,   -502,  -1003,  -1499,  -1990,  -2472,  -2945,  -3406,  -3854,  -4287,
     -4702,  -5099,  -5476,  -5832,  -6164,  -6472,  -6755,  -7010,  -7239,  -7438,
     -7608,  -7749,  -7858,  -7937,  -7984,  -8000,  -7984,  -7937,  -7858,  -7749,
     -7608,  -7438,  -7239,  -7010,  -6755,  -6472,  -6164,  -5832,  -5476,  -5099,
     -4702,  -4287,  -3854,  -3406,  -2945,  -2472,  -1990,  -1499,  -1003,   -502,
};

void sound_test_tone(const bool on) {
    test_tone_phase = 0;
    test_tone_on    = on;
}

bool sound_test_tone_is_on(void) { return test_tone_on; }

static void all_reset(void) {
    ay_reset(&ay[0]);
    ay_reset(&ay[1]);
    saa_reset(&saa);
    saa_set_clock(&saa, SAA_CLOCK_HZ, audio_rate_millihz() / 1000u);
    ay_sel   = 0;
    covox    = 0x80;
    specdrum = 0x80;
    memset(sd, 0x80, sizeof sd);
    beep_hi  = false;
    for (int i = 0; i < 2; i++) {
        saa_reset(&cms[i]);
        saa_set_clock(&cms[i], CMS_CLOCK_HZ, audio_rate_millihz() / 1000u);
    }
    fm_on = true;
    memset(pit, 0, sizeof pit);
    chips_reset();                      // the FM and PSG cores, chips.cpp
}

void sound_init(void) {
    chips_init((int)(audio_rate_millihz() / 1000u));
    pit_ticks_q16 = (uint32_t)((3500000ull * 1000u << 16) / audio_rate_millihz());
    ay_ticks_q16 = (uint32_t)((((uint64_t)AY_CLOCK_HZ * 1000u) << 16) / audio_rate_millihz());
    all_reset();
    memset(&fr, 0, sizeof fr);
}

void sound_flush(void) {
    sndq_tail = sndq_head;
    memset(&fr, 0, sizeof fr);
    sndq_drops = 0;
}

__force_inline static uint32_t sample_end(const uint32_t i) {
    return (uint32_t)(((uint64_t)(i + 1u) * fr.len) / fr.spf);
}

/* One sample, covering [fr.start, end). */
/*
 * The mix's range since a tool last reset it, before the volume: how a
 * test hears whether a chip is making a sound. Left and right.
 */
volatile int32_t sound_min[2] = { INT32_MAX, INT32_MAX }, sound_max[2] = { INT32_MIN, INT32_MIN };

static void __not_in_flash_func(make_sample)(const uint32_t end) {
    int32_t l = 0, r = 0;

    if (likely(spec_have_ay())) {
        ay_tick_frac += ay_ticks_q16;
        const uint32_t ticks = ay_tick_frac >> 16;
        ay_tick_frac &= 0xFFFFu;
        const ay_stereo_t pan = (ay_stereo_t)settings.ay_stereo;
        int32_t al, ar;
        ay_render_stereo(&ay[0], ticks, pan, &al, &ar);
        if (settings.turbosound) {
            int32_t bl, br;
            ay_render_stereo(&ay[1], ticks, pan, &bl, &br);
            al += bl; ar += br;
        }
        l += al * settings.ay_vol / 10;
        r += ar * settings.ay_vol / 10;
    }

    /* The SAA sits at the AY's scale, so it shares its volume. It is a
     * stereo chip in its own right and pans itself. */
    if (unlikely(settings.saa)) {
        int32_t sl, sr;
        saa_render_stereo(&saa, &sl, &sr);
        l += sl * settings.ay_vol / 10;
        r += sr * settings.ay_vol / 10;
    }

    /*
     * The beeper at about a quarter of full scale, in the middle, as the
     * average of where the speaker was across the whole sample -- a box
     * filter, so an edge part-way through a sample lands part-way.
     *
     * On a real Spectrum the beeper is considerably louder than the AY,
     * and reproducing that faithfully makes every 128K game's music sit
     * under its own sound effects. This is the compromise every emulator
     * makes; it is a mix decision, not a measurement.
     */
    const uint32_t span = end > fr.start ? end - fr.start : 1u;
    if (beep_hi) beep_area += end - beep_t;
    beep_t = end;
    const int32_t beep = (int32_t)(((int64_t)beep_area * 12000) / span) - 6000;
    beep_area = 0;
    l += beep * settings.beeper_vol / 10;
    r += beep * settings.beeper_vol / 10;

    /*
     * The FM and PSG chips, at the AY's scale and sharing its volume --
     * they stand in for, or beside, the AY. Only the ones software has
     * written to exist, so this costs nothing until then.
     */
    if (unlikely(chips_any())) {
        int32_t cl, cr;
        chips_sample(&cl, &cr);
        if (fm_on || !settings.tsfm) {
            l += cl * settings.ay_vol / 10;
            r += cr * settings.ay_vol / 10;
        }
    }
    if (unlikely(spec_machine == MACHINE_BYTE)) {
        const int32_t p = pit_sample() * settings.beeper_vol / 10;
        l += p;
        r += p;
    }

    if (unlikely(settings.cms)) {
        for (int i = 0; i < 2; i++) {
            int32_t sl, sr;
            saa_render_stereo(&cms[i], &sl, &sr);
            l += sl * settings.ay_vol / 10;
            r += sr * settings.ay_vol / 10;
        }
    }

    // The extra DACs, if the machine has been told it has them. Each is a
    // byte centred on 0x80 at a quarter of full scale. Covox and Specdrum
    // are one DAC each and go in the middle; SounDrive is two a side --
    // #0F and #1F left, #4F and #5F right, which is how the card was wired.
    if (unlikely(settings.covox)) {
        const int32_t c = ((int32_t)covox - 128) * 64;
        l += c;
        r += c;
    }
    if (unlikely(settings.specdrum)) {
        const int32_t c = ((int32_t)specdrum - 128) * 64;
        l += c;
        r += c;
    }
    if (unlikely(settings.soundrive)) {
        l += (((int32_t)sd[0] - 128) + ((int32_t)sd[1] - 128)) * 64;
        r += (((int32_t)sd[2] - 128) + ((int32_t)sd[3] - 128)) * 64;
    }

    /*
     * The test tone replaces the mix rather than joining it, so what is
     * measured is the path and not the path plus whatever the machine
     * happened to be doing. Both sides, in phase.
     */
    if (unlikely(test_tone_on)) {
        if (++test_tone_phase >= TEST_TONE_SAMPLES) test_tone_phase = 0;
        l = r = test_tone_wave[test_tone_phase];
    }

    /*
     * Master volume and the output's own headroom, in one step.
     *
     * The second divisor is what the board does not have: there is no
     * attenuator between the LM358 followers and the MS9288A, so a
     * full-scale mix arrives at its line input about three times too big
     * and everything sounds distorted. Headphones and the speaker want
     * it full scale. Which of the two is listening is a switch nothing
     * can read -- see output_t in settings.h.
     */
    /*
     * The mix is built so that everything at once fits sixteen bits,
     * which leaves ordinary music at a quarter of the range: right for a
     * line input, quiet on the PAM8403 and its little speaker. "Loud"
     * spends the headroom, and audio.c clips (and counts) what is left.
     */
    if (l < sound_min[0]) sound_min[0] = l;
    if (l > sound_max[0]) sound_max[0] = l;
    if (r < sound_min[1]) sound_min[1] = r;
    if (r > sound_max[1]) sound_max[1] = r;

    static const uint8_t out_scale[OUTPUT_COUNT] = { 10, 3, 25 };   // over ten
    const int32_t scale = (int32_t)(settings.volume *
                          out_scale[settings.output < OUTPUT_COUNT ? settings.output : 0]);
    audio_push(l * scale / 100, r * scale / 100);

    fr.start = end;
}

/* Every sample that ends at or before t. */
static void __not_in_flash_func(render_until)(const uint32_t t) {
    while (fr.open && fr.idx < fr.spf) {
        const uint32_t end = sample_end(fr.idx);
        if (end > t) break;
        make_sample(end);
        fr.idx++;
    }
}

static void __not_in_flash_func(apply)(const uint32_t dev, const uint32_t val, const uint32_t t) {
    switch (dev) {
        case SD_AY_SEL0: ay_select(&ay[0], (uint8_t)val); break;
        case SD_AY_DAT0: ay_write(&ay[0], (uint8_t)val);  break;
        case SD_AY_SEL1: ay_select(&ay[1], (uint8_t)val); break;
        case SD_AY_DAT1: ay_write(&ay[1], (uint8_t)val);  break;
        case SD_SAA_SEL: saa_select(&saa, (uint8_t)val);  break;
        case SD_SAA_DAT: saa_write(&saa, (uint8_t)val);   break;
        case SD_FE: {
            const bool mic = (val & 0x08u) != 0;
            if (mic != mic_hi) { mic_hi = mic; taperec_edge(frame_abs + t); }
            const bool hi = (val & 0x10u) != 0;
            if (hi != beep_hi) {
                const uint32_t at = t > beep_t ? t : beep_t;
                if (beep_hi) beep_area += at - beep_t;
                beep_t  = at;
                beep_hi = hi;
            }
            break;
        }
        case SD_COVOX:    covox    = (uint8_t)val; break;
        case SD_SD0:      sd[0]    = (uint8_t)val; break;
        case SD_SD1:      sd[1]    = (uint8_t)val; break;
        case SD_SD2:      sd[2]    = (uint8_t)val; break;
        case SD_SD3:      sd[3]    = (uint8_t)val; break;
        case SD_SPECDRUM: specdrum = (uint8_t)val; break;
        case SD_OPN0_A:   chips_opn_write(0, false, (uint8_t)val); break;
        case SD_OPN0_D:   chips_opn_write(0, true,  (uint8_t)val); break;
        case SD_OPN1_A:   chips_opn_write(1, false, (uint8_t)val); break;
        case SD_OPN1_D:   chips_opn_write(1, true,  (uint8_t)val); break;
        case SD_FM_ON:    fm_on = val != 0; break;
        case SD_OPL0: case SD_OPL1: case SD_OPL2: case SD_OPL3:
                          chips_opl_write((int)(dev - SD_OPL0), (uint8_t)val); break;
        case SD_OPLL_A:   chips_opll_write(false, (uint8_t)val); break;
        case SD_OPLL_D:   chips_opll_write(true,  (uint8_t)val); break;
        case SD_SN0:      chips_sn_write(0, (uint8_t)val); break;
        case SD_SN1:      chips_sn_write(1, (uint8_t)val); break;
        case SD_CMS0_SEL: saa_select(&cms[0], (uint8_t)val); break;
        case SD_CMS0_DAT: saa_write(&cms[0], (uint8_t)val);  break;
        case SD_CMS1_SEL: saa_select(&cms[1], (uint8_t)val); break;
        case SD_CMS1_DAT: saa_write(&cms[1], (uint8_t)val);  break;
        case SD_PIT0: case SD_PIT1: case SD_PIT2: case SD_PIT3:
                          pit_write(dev - SD_PIT0, (uint8_t)val); break;
        case SD_RESET:    all_reset();             break;
        default: break;
    }
    (void)ay_sel;
}

void __not_in_flash_func(sound_task)(void) {
    uint32_t tail = sndq_tail;
    const uint32_t head = sndq_head;
    __dmb();

    while (tail != head) {
        const uint32_t e   = sndq[tail & (SNDQ_SIZE - 1u)];
        const uint32_t t   = e >> 14;
        const uint32_t dev = (e >> 8) & 0x3Fu;
        const uint32_t val = e & 0xFFu;
        tail++;

        switch (dev) {
            case SD_BEGIN:
                // Anything left of the last frame was never ended; drop it.
                fr.len   = t ? t : 1u;
                fr.spf   = 0;
                fr.idx   = 0;
                fr.start = 0;
                fr.open  = false;
                beep_t    = 0;
                beep_area = 0;
                break;
            case SD_SPF:
                fr.spf  = t ? t : 1u;
                fr.open = true;
                break;
            case SD_END:
                render_until(fr.len);
                fr.open = false;
                frame_abs += fr.len;
                taperec_poll(frame_abs);
                break;
            default:
                render_until(t);
                apply(dev, val, t);
                break;
        }
    }

    sndq_tail = tail;
}
