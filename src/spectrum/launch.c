/*
 * frank-dino — an RP2350B acting as the whole machine around a real Z80
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-dino
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "launch.h"

#include <stdio.h>
#include <string.h>

#include "dino.h"
#include "spectrum.h"
#include "z80_bus.h"

uint32_t spec_launch_opfetches;

/*
 * The page the trampoline is built in.
 *
 * Sixteen kilobytes to hold about sixty bytes of generated code, because
 * it stands in for the ROM window and the window is that size. The waste
 * is the price of not putting a single byte of it in guest RAM.
 */
static uint8_t tramp[SPEC_BANK_SIZE] __attribute__((aligned(4)));

uint8_t *spec_launch_page(void) { return tramp; }

// Where the two flag/accumulator pairs are parked for POP AF to reach.
// Well clear of the code, and inside the window so the CPU can read them.
#define TRAMP_AF  0x3F00
#define TRAMP_AF_ 0x3F02

// ---------------------------------------------------------------------------
// Emitting
// ---------------------------------------------------------------------------

static uint16_t emit;

static void b1(const uint8_t v)  { tramp[emit++] = v; }
static void b2(const uint16_t v) { b1((uint8_t)v); b1((uint8_t)(v >> 8)); }

static void ld_rr(const uint8_t opcode, const uint16_t v) { b1(opcode); b2(v); }

bool spec_launch(const z80_state_t *st) {
    memset(tramp, 0xFF, sizeof tramp);
    emit = 0;

    tramp[TRAMP_AF]      = (uint8_t)st->af;         // F
    tramp[TRAMP_AF + 1]  = (uint8_t)(st->af >> 8);  // A
    tramp[TRAMP_AF_]     = (uint8_t)st->af_;
    tramp[TRAMP_AF_ + 1] = (uint8_t)(st->af_ >> 8);

    /*
     * Order matters in two places.
     *
     * The shadow set has to be loaded and swapped away before the main
     * set, because there is only one set of instructions to load either.
     * And SP is set last of the sixteen-bit registers, because it is used
     * as a pointer to reach AF on the way there.
     */
    b1(0xF3);                                   // DI

    b1(0x3E); b1(st->i);                        // LD A,i
    b1(0xED); b1(0x47);                         // LD I,A

    /*
     * R is a best effort and cannot be otherwise.
     *
     * The refresh counter increments on every M1 cycle, including the
     * ones this trampoline is made of, so whatever is loaded here is
     * already wrong by the time the jump happens. It is loaded anyway
     * because the low seven bits are what a handful of programs use as a
     * cheap random number, and being approximately right beats being
     * zero. Anything that depends on R exactly is not going to work on
     * this machine regardless.
     */
    b1(0x3E); b1(st->r);                        // LD A,r
    b1(0xED); b1(0x4F);                         // LD R,A

    ld_rr(0x31, TRAMP_AF_);                     // LD SP,af'
    b1(0xF1);                                   // POP AF
    b1(0x08);                                   // EX AF,AF'
    ld_rr(0x01, st->bc_);                       // LD BC,bc'
    ld_rr(0x11, st->de_);                       // LD DE,de'
    ld_rr(0x21, st->hl_);                       // LD HL,hl'
    b1(0xD9);                                   // EXX

    ld_rr(0x31, TRAMP_AF);                      // LD SP,af
    b1(0xF1);                                   // POP AF
    ld_rr(0x01, st->bc);                        // LD BC,bc
    ld_rr(0x11, st->de);                        // LD DE,de
    ld_rr(0x21, st->hl);                        // LD HL,hl
    b1(0xDD); ld_rr(0x21, st->ix);              // LD IX,ix
    b1(0xFD); ld_rr(0x21, st->iy);              // LD IY,iy

    b1(0xED);                                   // IM n
    b1(st->im == 0 ? 0x46 : (st->im == 1 ? 0x56 : 0x5E));

    ld_rr(0x31, st->sp);                        // LD SP,sp

    // EI must be the last thing before the jump: on a Z80 it takes effect
    // after the *following* instruction, which makes that instruction the
    // jump, which is exactly right — an interrupt cannot then arrive
    // between enabling them and arriving at the snapshot's code.
    if (st->iff) b1(0xFB);                      // EI

    const uint16_t jp_at = emit;
    b1(0xC3); b2(st->pc);                       // JP pc

    /*
     * The border and the paging latch are set from this side, not by the
     * trampoline.
     *
     * An OUT to 0x7FFD from inside the trampoline would page the ROM
     * window — and the trampoline is the ROM window, so it would unmap
     * itself half way through. Doing it here avoids the problem entirely
     * and costs the snapshot nothing: no real machine can tell the
     * difference between the latch being written a microsecond earlier
     * and it always having been that way.
     */
    spec_port_1ffd = st->port1ffd;
    spec_port_dffd = st->portdffd;
    spec_set_paging(st->port7ffd);
    spec_border = st->border & 7;

    // Now put the trampoline where the ROM was, and hold the swap-back
    // until the CPU has finished reading the jump.
    spec_begin_launch(tramp, jp_at);

    z80_reset();

    /*
     * Enough T-states for sixty bytes of straight-line code, and then
     * some. Nothing here loops, so if the CPU has not reached the jump in
     * this long it is not going to.
     */
    const uint32_t before = z80_stats.opfetch;
    z80_run(2000);
    spec_launch_opfetches = z80_stats.opfetch - before;
    z80_park();                     // a menu may stay open a while yet

    return !spec_launch_pending();
}

// ---------------------------------------------------------------------------
// Proving it
// ---------------------------------------------------------------------------

/*
 * Stores seven registers and spins.
 *
 * At 0x8000, which is bank 2 and therefore always mapped whatever the
 * paging latch says; the results land at 0x9000, in the same bank.
 *
 *   8000  32 00 90        LD (9000h),A
 *   8003  ED 43 01 90     LD (9001h),BC
 *   8007  ED 53 03 90     LD (9003h),DE
 *   800B  22 05 90        LD (9005h),HL
 *   800E  DD 22 07 90     LD (9007h),IX
 *   8012  FD 22 09 90     LD (9009h),IY
 *   8016  ED 73 0B 90     LD (900Bh),SP
 *   801A  D9              EXX
 *   801B  ED 43 0D 90     LD (900Dh),BC     ; the shadow set
 *   801F  18 FE           JR 801Fh
 */
static const uint8_t probe[] = {
    0x32, 0x00, 0x90,
    0xED, 0x43, 0x01, 0x90,
    0xED, 0x53, 0x03, 0x90,
    0x22, 0x05, 0x90,
    0xDD, 0x22, 0x07, 0x90,
    0xFD, 0x22, 0x09, 0x90,
    0xED, 0x73, 0x0B, 0x90,
    0xD9,
    0xED, 0x43, 0x0D, 0x90,
    0x18, 0xFE,
};

#define PROBE_ADDR   0x8000
#define PROBE_RESULT 0x9000
#define BANK2_OFF(a) ((a) - 0x8000)

bool spec_launch_selftest(char *detail, const unsigned detail_len) {
    // Values with no repeated bytes between them, so a register that
    // arrives in the wrong place is obvious rather than plausible.
    const z80_state_t st = {
        .af = 0x5A00, .bc = 0x1234, .de = 0x5678, .hl = 0x9ABC,
        .af_ = 0x0000, .bc_ = 0xCAFE, .de_ = 0x0000, .hl_ = 0x0000,
        .ix = 0x1122, .iy = 0x3344, .sp = 0xFF00, .pc = PROBE_ADDR,
        .i = 0x3F, .r = 0x00, .im = 1, .iff = false,
        .border = 0, .port7ffd = 0,
    };

    memset(spec_ram[2], 0, SPEC_BANK_SIZE);
    memcpy(&spec_ram[2][BANK2_OFF(PROBE_ADDR)], probe, sizeof probe);

    if (!spec_launch(&st)) {
        snprintf(detail, detail_len, "CPU never left the trampoline (%lu fetches)",
                 (unsigned long)spec_launch_opfetches);
        return false;
    }

    const uint8_t *r = &spec_ram[2][BANK2_OFF(PROBE_RESULT)];
    const uint16_t bc  = (uint16_t)(r[1] | (r[2] << 8));
    const uint16_t de  = (uint16_t)(r[3] | (r[4] << 8));
    const uint16_t hl  = (uint16_t)(r[5] | (r[6] << 8));
    const uint16_t ix  = (uint16_t)(r[7] | (r[8] << 8));
    const uint16_t iy  = (uint16_t)(r[9] | (r[10] << 8));
    const uint16_t sp  = (uint16_t)(r[11] | (r[12] << 8));
    const uint16_t bc_ = (uint16_t)(r[13] | (r[14] << 8));

    struct { const char *name; uint16_t got, want; } check[] = {
        { "A",   r[0],            (uint8_t)(st.af >> 8) },
        { "BC",  bc,              st.bc  },
        { "DE",  de,              st.de  },
        { "HL",  hl,              st.hl  },
        { "IX",  ix,              st.ix  },
        { "IY",  iy,              st.iy  },
        { "SP",  sp,              st.sp  },
        { "BC'", bc_,             st.bc_ },
    };

    for (unsigned i = 0; i < count_of(check); i++)
        if (check[i].got != check[i].want) {
            snprintf(detail, detail_len, "%s came back %04X, wanted %04X",
                     check[i].name, check[i].got, check[i].want);
            return false;
        }

    snprintf(detail, detail_len, "8 registers restored through the trampoline");
    return true;
}
