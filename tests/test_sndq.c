/*
 * The sound queue, from post to sample.
 *
 * Core 1 posts timestamped writes and core 0 turns them into samples.
 * What can go wrong is arithmetic: a frame cut into the wrong number of
 * samples is a pitch error, an edge landing in the wrong sample is a
 * beeper out of phase, and a queue that loses a frame marker is a
 * renderer that never finishes a frame. Each is checked here against
 * numbers worked out by hand.
 */
#include <stdio.h>
#include <string.h>

#include "sound.h"
#include "settings.h"
#include "tap.h"

/* --- what the renderer hands the DAC --------------------------------- */
static int32_t got_l[4096], got_r[4096];
static unsigned got_n;

void audio_push(const int32_t l, const int32_t r) {
    if (got_n < 4096) { got_l[got_n] = l; got_r[got_n] = r; }
    got_n++;
}

bool spec_have_ay(void) { return false; }

static void reset(void) {
    sound_flush();
    sound_init();
    got_n = 0;
    memset(&settings, 0, sizeof settings);
    settings.volume     = 10;
    settings.beeper_vol = 10;
    settings.output     = OUTPUT_JACK;
}

int main(void) {
    GROUP("frame");
    reset();
    snd_put(1000, SD_BEGIN, 0);
    snd_put(10, SD_SPF, 0);
    snd_put(1000, SD_END, 0);
    sound_task();
    EQ(got_n, 10u, "a frame of ten samples makes ten");
    EQ(got_l[0], -6000, "a silent beeper sits at the bottom");

    GROUP("box filter");
    reset();
    snd_put(1000, SD_BEGIN, 0);
    snd_put(10, SD_SPF, 0);
    snd_put(50, SD_FE, 0x10);       // high halfway through sample 0
    snd_put(250, SD_FE, 0x00);      // low halfway through sample 2
    snd_put(1000, SD_END, 0);
    sound_task();
    EQ(got_n, 10u, "still ten");
    EQ(got_l[0], 0, "half a sample high averages to the middle");
    EQ(got_l[1], 6000, "a whole sample high is the top");
    EQ(got_l[2], 0, "and half again on the way down");
    EQ(got_l[3], -6000, "then the bottom");
    EQ(got_r[1], got_l[1], "the beeper is in the middle");

    GROUP("progressive");
    reset();
    snd_put(1000, SD_BEGIN, 0);
    snd_put(10, SD_SPF, 0);
    snd_put(450, SD_MARK, 0);
    sound_task();
    EQ(got_n, 4u, "a mark at 450 renders the samples ending by it");
    snd_put(1000, SD_END, 0);
    sound_task();
    EQ(got_n, 10u, "and the end renders the rest");

    GROUP("uneven split");
    reset();
    snd_put(70908, SD_BEGIN, 0);
    snd_put(882, SD_SPF, 0);
    snd_put(70908, SD_END, 0);
    sound_task();
    EQ(got_n, 882u, "a 128K frame at 44.1 kHz is 882 samples");

    GROUP("overflow");
    reset();
    for (unsigned i = 0; i < SNDQ_SIZE + 100; i++) snd_put(i, SD_COVOX, i);
    CHECK(sndq_drops > 0, "a flood is counted");
    snd_put(0, SD_BEGIN, 0);
    CHECK(sndq_head - sndq_tail <= SNDQ_SIZE, "and a marker still fits");
    sound_task();
    EQ(sndq_head, sndq_tail, "the queue drains");

    return tap_done("sndq");
}
