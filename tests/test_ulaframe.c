/*
 * The frame as the ULA saw it.
 *
 * A write lands in RAM at once; whether the picture shows it depends on
 * whether the beam had already fetched that byte. Each case here is a
 * write at a T-state on one side or the other of a fetch, and the frame
 * is checked against what a real ULA would have put on the screen.
 */
#include <stdio.h>
#include <string.h>

#include "spectrum.h"
#include "settings.h"
#include "ula_frame.h"
#include "z80_bus.h"
#include "tap.h"

#ifndef STUB_HAS_SPEC_RAM
uint8_t spec_ram[SPEC_RAM_BANKS][SPEC_BANK_SIZE];
#endif

/* A write as the bus engine makes it: the watch first, then the store. */
static void wr(const uint16_t a, const uint8_t v, const uint32_t t) {
    if ((z80_wwatch_mask >> (a >> 13)) & 1u) z80_wwatch_cb(a, v, t);
    z80_poke(a, v);
}

/* 48K: the first paper fetch is at 14336, 224 T-states a line. */
#define FETCH(y, c) (14336u + (y) * 224u + (c) * 4u)

int main(void) {
    memset(&settings, 0, sizeof settings);
    spec_screen_bank = 5;
    spec_border = 7;
    spec_machine = MACHINE_48K;
    z80_map_page(1, spec_ram[5], spec_ram[5]);       // 0x4000
    ula_set_enabled(true);

    GROUP("attributes");
    memset(spec_ram[5], 0, sizeof spec_ram[5]);
    spec_ram[5][0x1800] = 0x38;
    ula_frame_begin();
    CHECK(z80_wwatch_mask == 0x04, "the 8 KB window the screen is in is watched");
    wr(0x5800, 0x07, FETCH(3, 0) + 1);               // after line 3's fetch, before line 4's
    ula_frame_end();
    const ula_frame_t *f = ula_frame_latest();
    CHECK(f != NULL, "a frame is published");
    EQ(f->att[0][0], 0x38, "line 0 had already been drawn with the old attribute");
    EQ(f->att[3][0], 0x38, "...and so had line 3");
    EQ(f->att[4][0], 0x07, "line 4 was fetched after the write");
    EQ(f->att[7][0], 0x07, "...and line 7");

    GROUP("bitmap");
    ula_frame_begin();
    wr(0x4000, 0xAA, FETCH(0, 0) - 10);              // ahead of the beam
    wr(0x4000, 0x55, FETCH(0, 0) + 10);              // behind it
    wr(0x4001, 0x11, 100);                           // in the top border
    ula_frame_end();
    f = ula_frame_latest();
    EQ(f->bmp[0][0], 0xAA, "the byte the beam saw, not the one RAM ends with");
    EQ(f->bmp[0][1], 0x11, "a write before the paper is what is drawn");
    EQ(spec_ram[5][0], 0x55, "RAM has the last write regardless");

    GROUP("border");
    ula_frame_begin();
    ula_border(20000, 2);
    ula_border(20004, 5);
    ula_frame_end();
    f = ula_frame_latest();
    EQ(f->nev, 2u, "two changes");
    EQ(ULA_EV_T(f->ev[1]), 20004u, "at their T-states");
    EQ(ULA_EV_COL(f->ev[1]), 5u, "with their colours");
    EQ(f->border_at_start, 7u, "and what it was before them");

    GROUP("triple buffer");
    const ula_frame_t *a = ula_frame_latest();
    ula_frame_begin(); ula_frame_end();
    const ula_frame_t *b = ula_frame_latest();
    CHECK(a != b, "a new frame is a different buffer from the one on screen");

    return tap_done("ulaframe");
}
