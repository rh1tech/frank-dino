/*
 * The ZX Evolution's scancode log: USB reports in, set-2 bytes out, with
 * the AVR's own edges -- 0 when empty, 0xFF once after an overflow.
 */
#include "tap.h"

#include <string.h>

#include "evo_ps2.h"

static int drain(uint8_t *out, int max) {
    int n = 0;
    for (uint8_t b; n < max && (b = evo_ps2_pop()) != 0; ) out[n++] = b;
    return n;
}

int main(void) {
    uint8_t got[32];
    int n;

    GROUP("the log");
    EQ(evo_ps2_pop(), 0, "an empty log reads 0");

    const uint8_t none[8] = { 0 };
    const uint8_t a[8]    = { 0, 0, 0x04 };                 // A
    const uint8_t sa[8]   = { 0x02, 0, 0x04 };              // LShift + A
    evo_ps2_report(a, none);
    evo_ps2_report(none, a);
    n = drain(got, 32);
    CHECK(n == 3 && got[0] == 0x1C && got[1] == 0xF0 && got[2] == 0x1C, "A: make 1C, break F0 1C");

    evo_ps2_report(sa, none);
    EQ(evo_ps2_mods0(), 0x10, "LShift held: #0D D4");
    evo_ps2_report(none, sa);
    n = drain(got, 32);
    CHECK(n == 6 && got[0] == 0x12 && got[1] == 0x1C && got[2] == 0xF0 && got[3] == 0x12
          && got[4] == 0xF0 && got[5] == 0x1C, "Shift+A: 12 1C, then F0 12 F0 1C");
    EQ(evo_ps2_mods0(), 0, "and let go");

    GROUP("extended keys");
    evo_ps2_hid(0x52, true);                                // up arrow
    evo_ps2_hid(0x52, false);
    n = drain(got, 32);
    CHECK(n == 5 && got[0] == 0xE0 && got[1] == 0x75 && got[2] == 0xE0 && got[3] == 0xF0 && got[4] == 0x75,
          "Up: E0 75, E0 F0 75");
    evo_ps2_hid(0x40, true);                                // F7, the plain code above #7F
    n = drain(got, 32);
    CHECK(n == 1 && got[0] == 0x83, "F7 is a plain 83");
    evo_ps2_hid(0x40, false);
    drain(got, 32);

    GROUP("the console");
    evo_ps2_ascii('Q');
    n = drain(got, 32);
    CHECK(n == 6 && got[0] == 0x12 && got[1] == 0x15, "'Q' is Shift and Q");
    evo_ps2_ascii('\r');
    n = drain(got, 32);
    CHECK(n == 3 && got[0] == 0x5A, "Enter is 5A");
    evo_ps2_ascii('?');
    n = drain(got, 32);
    CHECK(n == 6 && got[1] == 0x4A, "'?' is Shift and /");

    GROUP("overflow");
    for (int i = 0; i < 10; i++) evo_ps2_ascii('x');        // 30 bytes into 16
    EQ(evo_ps2_pop(), 0xFF, "a full log reads 0xFF");
    EQ(evo_ps2_pop(), 0, "and is empty after it");
    evo_ps2_ascii('x');
    evo_ps2_clear();
    EQ(evo_ps2_pop(), 0, "#0C D0 empties it");

    return tap_done("evops2");
}
