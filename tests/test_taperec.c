/*
 * SAVE, heard: a .tap played through the generator and recorded back.
 *
 * The recorder never sees a file -- it sees edges at T-states, the way
 * the MIC bit of port #FE changes when SAVE runs. So the test plays a
 * tape into it as edges and checks that what comes out is the tape.
 */
#include <stdio.h>
#include <string.h>

#include "pulse.h"
#include "taperec.h"
#include "tap.h"

static uint8_t img[4096];
static uint32_t pulses[200000];

int main(void) {
    GROUP("round trip");
    const uint8_t hdr[19] = { 0x00, 0x03, 'S','A','V','E','D',' ',' ',' ',' ',' ',
                              0x05, 0x00, 0x00, 0x80, 0x00, 0x80, 0x00 };
    uint8_t dat[7] = { 0xFF, 1, 2, 3, 4, 5, 0 };
    for (int i = 0; i < 6; i++) dat[6] ^= dat[i];
    uint32_t n = 0;
    img[n++] = 19; img[n++] = 0; memcpy(img + n, hdr, 19); n += 19;
    img[n++] = 7;  img[n++] = 0; memcpy(img + n, dat, 7);  n += 7;

    pulse_open(img, n, PULSE_TAP);
    const uint32_t np = pulse_fill(pulses, 200000);
    CHECK(np > 10000, "the tape makes pulses");

    taperec_clear();
    uint64_t t = 1000;
    for (uint32_t i = 0; i < np; i++) {
        taperec_edge(t);                    // every pulse starts with an edge
        t += pulses[i] & PULSE_LEN_MASK;
    }
    taperec_poll(t + 1000000);

    EQ(taperec_blocks(), 2u, "two blocks heard");
    EQ(taperec_bytes(), n, "the same number of bytes");
    CHECK(memcmp(taperec_data(), img, n) == 0, "and the same bytes");

    GROUP("noise is not a block");
    taperec_clear();
    t = 0;
    for (int i = 0; i < 5000; i++) { taperec_edge(t); t += 300 + (uint32_t)(i * 37) % 900; }
    taperec_poll(t + 1000000);
    EQ(taperec_blocks(), 0u, "nothing from random edges");

    return tap_done("taperec");
}
