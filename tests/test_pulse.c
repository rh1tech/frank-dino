/*
 * The tape signal, played and then read back.
 *
 * Each format is built here, turned into pulses by the generator, and
 * the pulses decoded the way the ROM's LD-BYTES would -- so a test passes
 * when a loader would have got the bytes, which is the only thing that
 * matters about a tape signal. The timings are checked on the way.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pulse.h"
#include "tap.h"

static uint32_t pulses[1u << 20];
static uint32_t npulses;

static void play_all(void) {
    npulses = 0;
    while (npulses < (1u << 20)) {
        const uint32_t n = pulse_fill(&pulses[npulses], 997);   // odd, to split stages
        npulses += n;
        if (n < 997) break;
    }
}

static uint32_t len_of(const uint32_t i) { return pulses[i] & PULSE_LEN_MASK; }

/*
 * A decoder in the ROM's terms: skip to a run of pilot, take the two
 * sync pulses, then bits as pairs. Returns bytes decoded into out.
 * `pilot` and `bit0`/`bit1` are what this block should have used.
 */
static uint32_t decode_block(uint32_t *at, uint8_t *out, const uint32_t max,
                             const uint32_t pilot, const uint32_t bit0, const uint32_t bit1) {
    uint32_t i = *at;
    // Find a pilot run.
    uint32_t run = 0;
    while (i < npulses && run < 200) {
        run = (len_of(i) == pilot) ? run + 1 : 0;
        i++;
    }
    while (i < npulses && len_of(i) == pilot) i++;
    i += 2;                                 // the sync pair
    uint32_t n = 0, bits = 0;
    uint8_t byte = 0;
    const uint32_t mid = (bit0 + bit1) / 2;
    while (i + 1 < npulses && n < max) {
        const uint32_t a = len_of(i), b = len_of(i + 1);
        if (a > bit1 + 100 || b > bit1 + 100) break;     // the pause
        byte = (uint8_t)((byte << 1) | (a > mid ? 1 : 0));
        i += 2;
        if (++bits == 8) { out[n++] = byte; bits = 0; byte = 0; }
    }
    *at = i;
    return n;
}

static uint8_t img[65536];

static uint32_t put_tap_block(uint8_t *p, const uint8_t *body, const uint32_t len) {
    p[0] = (uint8_t)len;
    p[1] = (uint8_t)(len >> 8);
    memcpy(p + 2, body, len);
    return len + 2;
}

int main(void) {
    uint8_t got[256];

    GROUP("tap");
    {
        const uint8_t hdr[19] = { 0x00, 0x03, 'h','e','l','l','o',' ',' ',' ',' ',' ',
                                  0x02, 0x00, 0x00, 0x80, 0x00, 0x80, 0x00 };
        const uint8_t dat[4] = { 0xFF, 0xA5, 0x5A, 0x00 };
        uint32_t n = put_tap_block(img, hdr, sizeof hdr);
        n += put_tap_block(img + n, dat, sizeof dat);
        CHECK(pulse_open(img, n, PULSE_TAP), "opens");
        EQ(pulse_block_count(), 2u, "two blocks");
        CHECK(pulse_all_rom_loadable(), "a .tap is all ROM-loadable");
        char line[40];
        pulse_block_describe(0, line, sizeof line);
        STREQ(line, "Bytes: hello     ", "the header names itself");
        play_all();
        EQ(pulse_state(), PULSE_END, "plays to the end");
        EQ(len_of(0), 2168u, "pilot at 2168");

        uint32_t at = 0;
        const uint32_t h = decode_block(&at, got, sizeof got, 2168, 855, 1710);
        EQ(h, 19u, "the header reads back whole");
        CHECK(memcmp(got, hdr, 19) == 0, "and is the header");
        const uint32_t d = decode_block(&at, got, sizeof got, 2168, 855, 1710);
        EQ(d, 4u, "the data block reads back");
        CHECK(memcmp(got, dat, 4) == 0, "and is the data");
        // A header's pilot is 8063 pulses, data's 3223.
        uint32_t p = 0;
        while (len_of(p) == 2168) p++;
        EQ(p, 8063u, "a header has the long pilot");
    }

    GROUP("tzx turbo, tone, sequence, loop");
    {
        uint32_t n = 0;
        memcpy(img, "ZXTape!\x1A\x01\x14", 10); n = 10;
        // 0x12: a tone of 5 pulses of 1000
        img[n++] = 0x12; img[n++] = 0xE8; img[n++] = 0x03; img[n++] = 5; img[n++] = 0;
        // 0x24: loop twice
        img[n++] = 0x24; img[n++] = 2; img[n++] = 0;
        // 0x13: two pulses, 300 and 400
        img[n++] = 0x13; img[n++] = 2; img[n++] = 0x2C; img[n++] = 0x01; img[n++] = 0x90; img[n++] = 0x01;
        img[n++] = 0x25;
        // 0x11: turbo data, pilot 1500 x 300, syncs 400/500, bits 500/1000, 1 byte 0xC3
        const uint8_t t11[] = { 0x11, 0xDC, 0x05, 0x90, 0x01, 0xF4, 0x01, 0xF4, 0x01, 0xE8, 0x03,
                                0x2C, 0x01, 8, 0x00, 0x00, 1, 0, 0, 0xC3 };
        memcpy(img + n, t11, sizeof t11); n += sizeof t11;
        // 0x20 with 0: stop the tape
        img[n++] = 0x20; img[n++] = 0; img[n++] = 0;
        // 0x12 after the stop
        img[n++] = 0x12; img[n++] = 0x10; img[n++] = 0x27; img[n++] = 1; img[n++] = 0;

        CHECK(pulse_open(img, n, PULSE_TZX), "opens");
        CHECK(!pulse_all_rom_loadable(), "turbo data needs the signal");
        play_all();
        EQ(pulse_state(), PULSE_STOPPED, "stops at the stop record");
        EQ(len_of(0), 1000u, "the tone");
        EQ(len_of(5), 300u, "the sequence...");
        EQ(len_of(6), 400u, "...twice through the loop");
        EQ(len_of(7), 300u, "second time round");
        EQ(len_of(8), 400u, "...");
        uint32_t at = 9;
        const uint32_t d = decode_block(&at, got, sizeof got, 1500, 500, 1000);
        EQ(d, 1u, "the turbo byte");
        EQ(got[0], 0xC3, "is C3");
        pulse_resume();
        const uint32_t before = npulses;
        npulses = 0;
        npulses = pulse_fill(pulses, 16);
        EQ(npulses, 1u, "after the stop, one more pulse");
        EQ(len_of(0), 10000u, "the tone that follows");
        (void)before;
    }

    GROUP("levels alternate");
    {
        const uint8_t dat[2] = { 0xFF, 0x00 };
        const uint32_t n = put_tap_block(img, dat, 2);
        pulse_open(img, n, PULSE_TAP);
        play_all();
        bool ok = true;
        for (uint32_t i = 1; i < 100; i++)
            if (((pulses[i] ^ pulses[i - 1]) & PULSE_LEVEL) == 0) ok = false;
        CHECK(ok, "every pilot pulse is an edge");
        EQ(pulses[npulses - 1] & PULSE_LEVEL, 0u, "and the pause ends low");
    }

    GROUP("gdb");
    {
        /*
         * A 0x19 that encodes one byte at the standard bit timings: no
         * pilot, two data symbols of two pulses each (855/855 and
         * 1710/1710), one bit per symbol.
         */
        uint32_t n = 10;
        memcpy(img, "ZXTape!\x1A\x01\x14", 10);
        const uint32_t start = n;
        img[n++] = 0x19;
        const uint32_t lenat = n; n += 4;
        img[n++] = 0; img[n++] = 0;                  // pause
        img[n++] = 0; img[n++] = 0; img[n++] = 0; img[n++] = 0;   // TOTP
        img[n++] = 0; img[n++] = 0;                  // NPP, ASP
        img[n++] = 8; img[n++] = 0; img[n++] = 0; img[n++] = 0;   // TOTD = 8
        img[n++] = 2;                                // NPD
        img[n++] = 2;                                // ASD
        img[n++] = 0; img[n++] = 0x57; img[n++] = 0x03; img[n++] = 0x57; img[n++] = 0x03;
        img[n++] = 0; img[n++] = 0xAE; img[n++] = 0x06; img[n++] = 0xAE; img[n++] = 0x06;
        img[n++] = 0x96;                             // the byte
        const uint32_t blen = n - lenat - 4;
        img[lenat] = (uint8_t)blen; img[lenat + 1] = (uint8_t)(blen >> 8);
        img[lenat + 2] = 0; img[lenat + 3] = 0;
        (void)start;
        CHECK(pulse_open(img, n, PULSE_TZX), "opens");
        play_all();
        EQ(npulses, 16u, "eight symbols of two pulses");
        uint8_t b = 0;
        for (int i = 0; i < 8; i++) b = (uint8_t)((b << 1) | (len_of((uint32_t)i * 2) > 1283 ? 1 : 0));
        EQ(b, 0x96, "the byte comes back");
    }

    GROUP("pzx");
    {
        uint32_t n = 0;
        memcpy(img + n, "PZXT", 4); n += 4;
        img[n++] = 2; img[n++] = 0; img[n++] = 0; img[n++] = 0;
        img[n++] = 1; img[n++] = 0;
        // PULS: 3 x 2168 as a counted entry, then 667
        memcpy(img + n, "PULS", 4); n += 4;
        img[n++] = 6; img[n++] = 0; img[n++] = 0; img[n++] = 0;
        img[n++] = 0x03; img[n++] = 0x80; img[n++] = 0x78; img[n++] = 0x08;
        img[n++] = 0x9B; img[n++] = 0x02;
        // DATA: 8 bits, tail 945, two pulses per bit, one byte
        memcpy(img + n, "DATA", 4); n += 4;
        img[n++] = 17; img[n++] = 0; img[n++] = 0; img[n++] = 0;
        img[n++] = 8; img[n++] = 0; img[n++] = 0; img[n++] = 0;
        img[n++] = 0xB1; img[n++] = 0x03;
        img[n++] = 2; img[n++] = 2;
        img[n++] = 0x57; img[n++] = 0x03; img[n++] = 0x57; img[n++] = 0x03;
        img[n++] = 0xAE; img[n++] = 0x06; img[n++] = 0xAE; img[n++] = 0x06;
        img[n++] = 0x3C;
        CHECK(pulse_open(img, n, PULSE_PZX), "opens");
        play_all();
        EQ(len_of(0), 2168u, "a counted run");
        EQ(len_of(2), 2168u, "three of them");
        EQ(len_of(3), 667u, "then the next entry");
        uint8_t b = 0;
        for (int i = 0; i < 8; i++) b = (uint8_t)((b << 1) | (len_of(4 + (uint32_t)i * 2) > 1283 ? 1 : 0));
        EQ(b, 0x3C, "the data byte");
        EQ(len_of(npulses - 1), 945u, "and the tail pulse");
    }

    GROUP("csw");
    {
        uint32_t n = 0;
        memcpy(img, "Compressed Square Wave\x1A", 23); n = 23;
        img[n++] = 1; img[n++] = 1;                  // v1.01
        img[n++] = 0x44; img[n++] = 0xAC;            // 44100 Hz
        img[n++] = 1;                                // RLE
        img[n++] = 0;                                // flags
        img[n++] = 0; img[n++] = 0; img[n++] = 0;
        img[n++] = 22;                               // 22 samples
        img[n++] = 0; img[n++] = 0x10; img[n++] = 0x27; img[n++] = 0; img[n++] = 0;  // 10000
        CHECK(pulse_open(img, n, PULSE_CSW), "opens");
        play_all();
        EQ(npulses, 2u, "two edges");
        EQ(len_of(0), 1746u, "22 samples at 44.1 kHz in T-states");
        EQ(len_of(1), 793651u, "and a long one");
    }

    return tap_done("pulse");
}
