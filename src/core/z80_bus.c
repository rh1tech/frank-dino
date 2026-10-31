/*
 * frank-dino — an RP2350B acting as the whole machine around a real Z80
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-dino
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "z80_bus.h"

/*
 * Everything on the clock path is pinned in SRAM with
 * __not_in_flash_func(), the way pico-speccy pins its own hot functions.
 *
 * The rest of this firmware runs from flash through the XIP cache, which
 * is the only reason it fits. This file cannot: it generates the Z80's
 * clock edge by edge, and a cache miss between two edges stretches a
 * clock phase by about a microsecond. The CPU is static so nothing
 * breaks, but the measured clock moves and the pace floor moves with it
 * -- which is the check that this file is still where it should be.
 *
 * The helpers are all __force_inline and land inside their caller, so
 * marking the entry points is enough.
 */

#include <string.h>
#include <hardware/gpio.h>
#include <pico/time.h>

// ============================================================================
// State
// ============================================================================
z80_stats_t z80_stats;

uint8_t *z80_mem;       /* the app's, see z80_bus.h */

uint8_t *z80_rpage[Z80_PAGES];
uint8_t *z80_wpage[Z80_PAGES];

/*
 * Where writes to a read-only window go.
 *
 * Thrown away so the write path can be a single indexed store with no
 * test for ROM. The alternative costs a compare and a branch on a path
 * the CPU takes several million times a second. One window's worth --
 * 8 KB since the windows halved -- because every read-only window's
 * write pointer aims at the same bucket.
 */
static uint8_t write_bitbucket[8192] __attribute__((aligned(4)));

void z80_map_page8(const unsigned slot, uint8_t *read_from, uint8_t *write_to) {
    z80_rpage[slot] = read_from;
    z80_wpage[slot] = write_to ? write_to : write_bitbucket;
}

void z80_map_page(const unsigned slot, uint8_t *read_from, uint8_t *write_to) {
    /* The second half starts 8 KB in, and a NULL write pointer stays NULL
     * so both halves land in the bucket rather than one of them landing
     * 8 KB into it. */
    z80_map_page8(slot * 2, read_from, write_to);
    z80_map_page8(slot * 2 + 1, read_from ? read_from + 8192 : NULL,
                  write_to ? write_to + 8192 : NULL);
}

void z80_map_flat(void) {
    if (!z80_mem) return;       /* nothing to map: the app has not set it */

    // Wraps when z80_mem is smaller than the address space, which in a
    // Spectrum build it is: every window then shows the same 16 KB, which
    // is all calibration needs and all it ever looks at.
    for (unsigned i = 0; i < 4; i++) {
        uint8_t *p = z80_mem + ((i * 16384) % GUEST_MEM_SIZE);
        z80_map_page(i, p, p);
    }
}

uint8_t  z80_int_vector = 0xFF;

volatile bool     z80_trace_on = false;
bus_event_t       z80_trace[BUS_TRACE_DEPTH];
volatile uint32_t z80_trace_pos = 0;

static uint8_t io_in_default(uint16_t port, uint32_t t);
static void    io_out_default(uint16_t port, uint8_t val, uint32_t t) { (void)port; (void)val; (void)t; }

z80_io_in_fn  z80_io_in  = io_in_default;
z80_io_out_fn z80_io_out = io_out_default;

z80_opfetch_fn z80_opfetch_cb;

volatile uint8_t z80_wwatch_mask;
z80_wwatch_fn    z80_wwatch_cb;

uint32_t z80_t;
volatile uint32_t z80_fetch0;
/* Set by a debugger: the trace stops at the next fetch from #0000, so the
 * ring holds the way into a reset (tools/dino.py). */
volatile bool     z80_trace_stop0;

z80_contend_t z80_contend;

/*
 * How long the ULA holds the CPU for an access at frame T-state t.
 *
 * Zero outside the 192 lines of paper and outside the 128 T-states of
 * each line the ULA spends fetching; inside, the pattern for the
 * position within the ULA's eight-T-state fetch cycle. The modulo is
 * the one division here, and the M33 does it in a few cycles.
 */
__force_inline static uint32_t contend_at(const uint32_t t) {
    const uint32_t d = t - z80_contend.t0;
    if (d >= z80_contend.span) return 0;
    const uint32_t x = d % z80_contend.line;
    return x < 128u ? z80_contend.pat[x & 7u] : 0u;
}

/*
 * An I/O cycle's contention, the four cases Fuse documents: the high
 * byte decides whether the address is contended, A0 whether the ULA is
 * being addressed, and the two together the shape. Only on the machines
 * whose ULA contends I/O at all -- not the +2A/+3.
 */
static uint32_t __not_in_flash_func(contend_io)(const uint16_t port, uint32_t t) {
    const uint32_t t_in = t;
    const bool hi = ((z80_contend.mask >> (port >> 14)) & 1u) != 0;
    if (!z80_contend.io) return 0;
    if (hi) {
        if (!(port & 1u)) { t += contend_at(t) + 1u; t += contend_at(t) + 3u; }
        else for (int i = 0; i < 4; i++) t += contend_at(t) + 1u;
    } else {
        if (!(port & 1u)) { t += 1u; t += contend_at(t) + 3u; }
        else t += 4u;
    }
    return t - t_in - 4u;
}

/*
 * An unterminated Z80 data bus does not read as zero.
 *
 * The first thing tried here was returning 0x00 for an unmapped read,
 * which is NOP and looks harmless. It is not: a machine whose data bus
 * has come loose then runs quietly through all 64 KB of nothing and looks
 * exactly like a healthy one executing NOPs. 0xFF is RST 38h — it goes
 * somewhere, and it goes there loudly.
 */
#define BUS_FLOAT 0xFF

static uint8_t __not_in_flash_func(io_in_default)(uint16_t port, uint32_t t) {
    (void)port; (void)t;
    return BUS_FLOAT;
}

// How many consecutive quiet samples with ~M1 low and ~MREQ high mean an
// interrupt acknowledge rather than the first half-T-state of an ordinary
// opcode fetch. Sampling is twice per T-state, and on a fetch ~MREQ falls
// at the end of T1 — so two samples in it is normal and three is not.
#define INTA_SETTLE 3

static uint32_t pace_settle = 24;

/*
 * The second half-cycle's pace, which does not have to equal the first.
 *
 * A T-state is two settles, so splitting the count between them halves
 * the granularity for nothing: pace 7 can be 3+4 as well as 3+3 or 4+4.
 * That matters because one unit of this loop is about three processor
 * cycles, which at 3.5 MHz is five per cent of a half-cycle — so whole
 * steps put the nearest achievable clock 3.3 % below a Spectrum's
 * 3.5469 MHz, and 3.3 % is a third of a semitone on everything the
 * machine ever plays.
 *
 * Dithering with a fractional accumulator was tried first and made it
 * worse, not better: the add and compare it needs cost more time than
 * the granularity it was buying back, and the top speed fell below the
 * target altogether. This costs nothing at all.
 */
static uint32_t pace_settle_b = 24;

// How many consecutive quiet samples have matched the interrupt-
// acknowledge shape. File scope rather than a static inside the service
// routine, which is force-inlined at more than one call site and would
// otherwise have had one counter per site.
static uint32_t inta_run;

// ============================================================================
// Pins
// ============================================================================
//
// Everything the hot loop touches is in GP0-GP31, so every one of these is
// a single store to a SIO register with no read-modify-write and no
// function call. See dino.h for why that is not an accident.

__force_inline static void dbus_drive(const uint8_t v) {
    sio_hw->gpio_clr   = Z80_DATA_MASK;
    sio_hw->gpio_set   = (uint32_t)v << Z80_DATA_BASE;
    sio_hw->gpio_oe_set = Z80_DATA_MASK;
}

__force_inline static void dbus_release(void) {
    sio_hw->gpio_oe_clr = Z80_DATA_MASK;
}

/*
 * The delay, with its length passed in.
 *
 * pace_settle is a plain uint32_t and sio_hw points at volatile
 * uint32_t, so the compiler has to assume a store through SIO might
 * alias it -- which meant reloading the pace from memory after every
 * clock edge, twice per T-state, on the hottest loop there is. The same
 * is true of prev_ctl. Both are now held in registers by the caller and
 * written back when it is done, and that alone bought about a tenth of
 * the machine's speed.
 */
__force_inline static void settle(uint32_t n) {
    // A do-while, not a while: the zero test costs a compare and a
    // branch on every clock phase for a case that cannot happen --
    // z80_set_pace_raw() will not hand out a half-count below one.
    do { __asm volatile("nop" ::: "memory"); } while (--n);
}

// ============================================================================
// Memory
// ============================================================================

__force_inline static uint8_t mem_read(const uint16_t a) {
    return z80_rpage[a >> 13][a & 0x1FFF];
}

__force_inline static void mem_write(const uint16_t a, const uint8_t v) {
    z80_wpage[a >> 13][a & 0x1FFF] = v;
}

// ============================================================================
// The trace ring
// ============================================================================

__force_inline static void trace_put(const uint16_t addr, const uint8_t data,
                                     const bus_cycle_t kind) {
    if (likely(!z80_trace_on)) return;
    const uint32_t p = z80_trace_pos;
    z80_trace[p & (BUS_TRACE_DEPTH - 1)] = (bus_event_t){ addr, data, kind };
    z80_trace_pos = p + 1;
}

void z80_trace_reset(void) { z80_trace_pos = 0; }

uint32_t z80_trace_count(void) {
    const uint32_t n = z80_trace_pos;
    return n < BUS_TRACE_DEPTH ? n : BUS_TRACE_DEPTH;
}

bus_event_t z80_trace_get(const uint32_t i) {
    const uint32_t n = z80_trace_pos;
    const uint32_t base = n < BUS_TRACE_DEPTH ? 0 : n - BUS_TRACE_DEPTH;
    return z80_trace[(base + i) & (BUS_TRACE_DEPTH - 1)];
}

// ============================================================================
// One bus cycle's worth of work
// ============================================================================
/*
 * Called after every clock edge. Edge-triggered on the strobes, because
 * a cycle has to be serviced once and only once.
 *
 * The first version of this was level-driven — "if ~RD is low, answer" —
 * which is correct for a read, where answering twice with the same byte
 * is answering once. It is wrong for everything else. ~WR is low for two
 * samples and ~RD for three, so every OUT dispatched to the I/O handler
 * four times and every fetch counted three, and a ZX Spectrum's beeper
 * would have been kicked four times per OUT. Reads looked fine, which is
 * exactly why it survived being written.
 *
 * Sampling twice per T-state cannot miss an edge here, and that is a
 * property of the design rather than of the numbers: the strobes are
 * generated by the Z80 from the very clock edges this loop produces, so
 * the sampling points sit at fixed positions inside every cycle no matter
 * how long a phase runs. ~RD is low across four samples and ~WR across
 * two; a stretched phase stretches both.
 *
 * The bus is released on the first sample that sees ~RD high. The Z80's
 * data hold time is specified against ~RD rising and is zero, which is
 * how the part avoids making every designer work out which of its two
 * kinds of read cycle latches on which clock edge — an opcode fetch
 * latches at the rising edge of T3 and an ordinary read at the falling
 * edge, and neither needs to be known here.
 */
static uint32_t prev_ctl = 0xFFFFFFFFu;

/*
 * `diag` is a compile-time constant at every call site, so this is two
 * routines from one source: the diagnostic one counts every cycle into
 * z80_stats, and the production one counts nothing but the last fetch
 * address. The counters were five loads and stores per fetch on the loop
 * that is the machine's speed, read by nothing but stage 0.
 */
__force_inline static uint32_t bus_service(const uint32_t prev, uint32_t *const tp,
                                           const bool diag) {
    const uint32_t g    = sio_hw->gpio_in;
    const uint32_t fell = prev & ~g;

    if (fell & Z80_RD) {                    // a read cycle begins
        const uint16_t a = (uint16_t)(g & Z80_ADDR_MASK);
        uint8_t v;
        bus_cycle_t kind;

        /*
         * The ULA's contention, as time the frame moves on by with no
         * clock edge at all -- which is what the ULA does to a real Z80:
         * it stops its clock. The CPU is static and cannot tell.
         */
        if (!diag && unlikely(z80_contend.on)) {
            if (!(g & Z80_MREQ)) {
                if ((z80_contend.mask >> (a >> 14)) & 1u) *tp += contend_at(*tp);
            } else {
                *tp += contend_io(a, *tp);
            }
        }
        const uint32_t t = *tp;

        if (!(g & Z80_MREQ)) {
            if (!(g & Z80_M1)) {
                kind = BUS_OPFETCH;
                if (diag) z80_stats.opfetch++;
                // A fetch from #0000 is a reset or an RST 0: worth knowing about.
                if (unlikely(!a)) { z80_fetch0++; if (z80_trace_stop0) z80_trace_on = false; }
                z80_stats.last_opfetch_addr = a;
                // Before the read, not after: the callback exists to
                // change what this fetch will see. See z80_bus.h.
                if (unlikely(z80_opfetch_cb)) z80_opfetch_cb(a, t);
            } else {
                kind = BUS_MEM_READ;
                if (diag) z80_stats.mem_read++;
            }
            v = mem_read(a);
        } else {
            // ~MREQ idle with ~RD active is an I/O read and nothing else.
            v = z80_io_in(a, t);
            kind = BUS_IO_READ;
            if (diag) z80_stats.io_read++;
        }

        dbus_drive(v);
        if (diag) {
            z80_stats.last_addr = a;
            z80_stats.last_data = v;
        }
        trace_put(a, v, kind);
        inta_run = 0;
        return g;
    }

    if (!(g & Z80_RD)) return g;              // still reading; the byte is up

    if (fell & Z80_WR) {                    // a write cycle begins
        // The data lines are inputs: they were released on the sample
        // that ended the last read, and a write cannot follow a read
        // without one in between.
        const uint16_t a = (uint16_t)(g & Z80_ADDR_MASK);
        const uint8_t  v = (uint8_t)((g & Z80_DATA_MASK) >> Z80_DATA_BASE);

        if (!diag && unlikely(z80_contend.on)) {
            if (!(g & Z80_MREQ)) {
                if ((z80_contend.mask >> (a >> 14)) & 1u) *tp += contend_at(*tp);
            } else {
                *tp += contend_io(a, *tp);
            }
        }
        const uint32_t t = *tp;

        if (!(g & Z80_MREQ)) {
            // Before the store: a watcher wants the byte it replaces.
            if (unlikely((z80_wwatch_mask >> (a >> 13)) & 1u)) z80_wwatch_cb(a, v, t);
            mem_write(a, v);
            if (diag) z80_stats.mem_write++;
            trace_put(a, v, BUS_MEM_WRITE);
        } else {
            z80_io_out(a, v, t);
            if (diag) z80_stats.io_write++;
            trace_put(a, v, BUS_IO_WRITE);
        }
        if (diag) {
            z80_stats.last_addr = a;
            z80_stats.last_data = v;
        }
        inta_run = 0;
        return g;
    }

    if (!(g & Z80_WR)) return g;              // still writing

    /*
     * No strobe at all. Three things look like this and only one needs
     * anything done about it.
     *
     * A refresh cycle drives ~MREQ with the refresh address and neither
     * strobe, and wants to be left alone. The first half-T-state of an
     * opcode fetch has ~M1 low before ~MREQ has followed it, and also
     * wants to be left alone. An interrupt acknowledge is ~M1 low with
     * ~MREQ that never follows — which is why it takes a run of samples
     * to tell apart from the second case, and why INTA_SETTLE is three
     * rather than two.
     *
     * The CPU reads the vector off the data bus on the falling edge of
     * the second automatic wait state, so putting it up as soon as the
     * cycle is recognised leaves two clear T-states of setup.
     */
    if (!(g & Z80_M1) && (g & Z80_MREQ)) {
        if (++inta_run == INTA_SETTLE) {
            if (diag) z80_stats.int_ack++;
            trace_put((uint16_t)(g & Z80_ADDR_MASK), z80_int_vector, BUS_INT_ACK);
            dbus_drive(z80_int_vector);
        }
        return g;                           // the vector stays up until ~M1 rises
    }

    /*
     * The one place the bus is let go.
     *
     * It has to be here rather than immediately after ~RD rises, because
     * an interrupt acknowledge is a cycle with no strobe at all and the
     * vector has to survive it. Putting the release above the check
     * released it one sample after it went up, which is a fault that
     * only shows on a machine that takes interrupts — so, not until a
     * Spectrum was running on it.
     */
    inta_run = 0;
    dbus_release();
    return g;
}

/*
 * Which address lines have ever been seen high, and ever been seen low.
 *
 * Two words and two ORs per clock phase, which is cheap enough to leave
 * on always. It answers the question a failing test leaves behind — not
 * "did it work" but "which wire" — and it answers it for every line at
 * once rather than one probe at a time.
 *
 * Address lines only, deliberately. The data bus is *ours* for the whole
 * of every read cycle, so a data line stuck at the Z80's pin would still
 * be seen toggling — by us, driving it. That would be a stuck-line
 * detector that cannot detect a stuck line, which is worse than not
 * having one. Data lines are covered properly by the round trip in the
 * data-bus test instead.
 *
 * The address bus is high-impedance while ~RESET is low, so the watch has
 * to be reset after a reset rather than before one; floating pins read as
 * exercised.
 */
static uint32_t seen_high, seen_low;

/*
 * ...and it is off unless a test is watching.
 *
 * It cost an extra read of GPIO_IN and two read-modify-writes on globals
 * twice per T-state, on a loop where the whole T-state is about thirty
 * instructions. That is not a rounding error: switching it off bought
 * back most of a 13 % shortfall against 50 Hz. Nothing but the stage-0
 * wiring tests has ever read it.
 */
static bool watching;

__force_inline static void edge_watch(void) {
    if (likely(!watching)) return;
    const uint32_t g = sio_hw->gpio_in;
    seen_high |= g;
    seen_low  |= ~g;
}

/*
 * One T-state, with everything it reads more than once held in registers.
 *
 * `prev` is the previous sample, in and out; the paces come from the
 * caller. See settle() for why none of them is read from memory here.
 */
__force_inline static uint32_t t_state_r(uint32_t prev, const uint32_t pa, const uint32_t pb,
                                         const uint32_t t) {
    uint32_t tt = t;
    sio_hw->gpio_set = Z80_CLK;
    settle(pa);
    prev = bus_service(prev, &tt, true);
    edge_watch();

    sio_hw->gpio_clr = Z80_CLK;
    settle(pb);
    prev = bus_service(prev, &tt, true);
    edge_watch();

    return prev;
}

/* The same, for the callers that run a handful of cycles and go away. */
__force_inline static void t_state(void) {
    prev_ctl = t_state_r(prev_ctl, pace_settle, pace_settle_b, z80_t);
    z80_t++;
    z80_stats.t_states++;
}

uint32_t z80_raw_bus(void) { return sio_hw->gpio_in; }

// ============================================================================
// Wiring
// ============================================================================
//
// All twenty-four bus lines, checked against themselves rather than
// against the CPU. Everything here runs with the Z80 in reset and its
// buses high-impedance, so anything that holds a line is a fault.

#define BUS_LINES 24        // A0-A15 then D0-D7, contiguous from GP0

void z80_hold_reset(const bool held) {
    if (!held) {
        sio_hw->gpio_set = Z80_RST;
        busy_wait_us(50);
        return;
    }

    sio_hw->gpio_clr = Z80_RST;

    /*
     * And then clock it, which is the whole point.
     *
     * A Z80's reset is synchronous. Pulling ~RESET low and waiting does
     * not put the CPU into the reset state — it asks for it, and the
     * request is only honoured across rising clock edges. With no clock
     * the CPU keeps whatever it was doing, buses included.
     *
     * The first version of the wiring test did exactly that, and the
     * result was spectacular: every bus line reported as shorted to every
     * other bus line, because the CPU was still sitting there driving
     * them. Eight edges, then let go of the data bus on our own side, and
     * the bus is genuinely floating and worth measuring.
     */
    for (int i = 0; i < 8; i++) t_state();
    dbus_release();
    busy_wait_us(50);
}

// 50k against a few tens of pF of track and socket. Ten microseconds is
// comfortably past it; the pad itself settles in nanoseconds and is not
// what is being waited for.
#define PULL_SETTLE_US 10

// Bring-up breadcrumb: which line the pull test is on, readable over SWD
// while the thing is wedged. Cheaper than a marker on the screen and it
// survives a core that has stopped answering the debugger.
volatile uint32_t z80_wiring_pin = 0xFFFFFFFFu;

uint32_t z80_wiring_stuck(void) {
    uint32_t stuck = 0;

    for (unsigned i = 0; i < BUS_LINES; i++) {
        z80_wiring_pin = i;
        gpio_set_pulls(i, true, false);         // up
        busy_wait_us(PULL_SETTLE_US);
        const bool high = gpio_get(i);

        gpio_set_pulls(i, false, true);         // down
        busy_wait_us(PULL_SETTLE_US);
        const bool low = gpio_get(i);

        gpio_set_pulls(i, false, false);
        if (high == low) stuck |= 1u << i;      // would not move
    }

    busy_wait_us(PULL_SETTLE_US);
    return stuck;
}

uint32_t z80_wiring_levels(void) {
    for (unsigned i = 0; i < BUS_LINES; i++) {
        gpio_set_dir(i, GPIO_IN);
        gpio_set_pulls(i, false, false);
    }
    busy_wait_us(PULL_SETTLE_US);
    return sio_hw->gpio_in & ((1u << BUS_LINES) - 1u);
}

uint32_t z80_wiring_undriveable(void) {
    uint32_t bad = 0;

    for (unsigned i = 0; i < BUS_LINES; i++) {
        gpio_set_pulls(i, false, false);
        gpio_put(i, 1);
        gpio_set_dir(i, GPIO_OUT);
        busy_wait_us(PULL_SETTLE_US);

        // Reading a pin while driving it reads the pad, not the latch —
        // so this is the voltage actually on the track, and a line that
        // something else is holding down reads back low.
        if (!gpio_get(i)) bad |= 1u << i;

        gpio_set_dir(i, GPIO_IN);
        busy_wait_us(PULL_SETTLE_US);
    }

    for (unsigned i = 0; i < BUS_LINES; i++) gpio_set_pulls(i, false, false);
    busy_wait_us(PULL_SETTLE_US);
    return bad;
}

void z80_wiring_shorts(uint32_t out[BUS_LINES]) {
    // Every line an input, pulled down, so a line that comes up has been
    // brought up by something rather than left floating.
    for (unsigned i = 0; i < BUS_LINES; i++) {
        gpio_set_dir(i, GPIO_IN);
        gpio_set_pulls(i, false, true);
    }
    busy_wait_us(PULL_SETTLE_US);

    for (unsigned i = 0; i < BUS_LINES; i++) {
        gpio_set_pulls(i, false, false);
        gpio_put(i, 1);
        gpio_set_dir(i, GPIO_OUT);
        busy_wait_us(PULL_SETTLE_US);

        const uint32_t seen = sio_hw->gpio_in;
        out[i] = seen & ~(1u << i) & ((1u << BUS_LINES) - 1u);

        gpio_set_dir(i, GPIO_IN);
        gpio_set_pulls(i, false, true);
        busy_wait_us(PULL_SETTLE_US);
    }

    // Back to plain inputs: the bus engine expects to find them that way.
    for (unsigned i = 0; i < BUS_LINES; i++) {
        gpio_set_pulls(i, false, false);
        gpio_set_dir(i, GPIO_IN);
    }
    busy_wait_us(PULL_SETTLE_US);
}

void z80_edge_watch_reset(void) { seen_high = 0; seen_low = 0; watching = true; }
void z80_edge_watch_stop(void)  { watching = false; }

uint32_t z80_edge_watch_stuck_high(void) { return (~seen_low)  & Z80_ADDR_MASK; }
uint32_t z80_edge_watch_stuck_low(void)  { return (~seen_high) & Z80_ADDR_MASK; }

/*
 * The loop the whole machine's speed is.
 *
 * Everything it touches repeatedly lives in a register for the duration
 * and is written back at the end: the pace, the previous bus sample and
 * the T-state count. That is not micro-optimisation for its own sake --
 * the pace is how long the engine waits for the Z80's pins to settle,
 * and every instruction saved here is an instruction the tuner can spend
 * on waiting instead. Cheaper loop, longer settle, more margin.
 */
void __not_in_flash_func(z80_run)(const uint32_t t_states) {
    const uint32_t pa = pace_settle, pb = pace_settle_b;
    uint32_t prev = prev_ctl;
    uint32_t t = z80_t;

    for (uint32_t i = t_states; i--; t++) prev = t_state_r(prev, pa, pb, t);

    prev_ctl = prev;
    z80_t = t;

    // Counted once rather than once per T-state: the loop always runs
    // the whole count, and a register held for the duration is a
    // register the settle loop could have had.
    z80_stats.t_states += t_states;
}

/*
 * The machine's loop: z80_run() without the counting.
 *
 * No stuck-line watch -- two loads and a branch per T-state for a test
 * that only stage 0 runs -- and no counters but the last fetch address.
 * The frame's T-state is the loop variable rather than a down-counter, so
 * every handler is told when it was called at no cost at all.
 */
void __not_in_flash_func(z80_run_until)(const uint32_t t_stop) {
    const uint32_t pa = pace_settle, pb = pace_settle_b;
    uint32_t prev = prev_ctl;
    uint32_t t = z80_t;

    while ((int32_t)(t - t_stop) < 0) {
        sio_hw->gpio_set = Z80_CLK;
        settle(pa);
        prev = bus_service(prev, &t, false);

        sio_hw->gpio_clr = Z80_CLK;
        settle(pb);
        prev = bus_service(prev, &t, false);
        t++;
    }

    prev_ctl = prev;
    z80_t = t;
}

/*
 * Stop the clock HIGH.
 *
 * The engine ends every T-state with the clock low, which is fine for
 * the millisecond between two frames and not for longer: a 128K paused
 * in a menu for a second or two came back with an interrupt's return
 * address gone -- on most pauses, not all -- and walked off through
 * empty RAM into a cold start. Zilog specifies the Z84C00 as static with
 * the clock stopped high; held low for seconds, this one is not.
 *
 * So any stop that can be long is made in the high half of a T-state:
 * the rising edge and its sample now. Nothing undoes it -- the run loop
 * starts every T-state by setting the clock high, which from here is no
 * edge at all, then takes the falling edge as usual, so the T-state
 * parked in is simply a long one.
 */
void __not_in_flash_func(z80_park)(void) {
    if (sio_hw->gpio_out & Z80_CLK) return;     // already high
    uint32_t t = z80_t;
    sio_hw->gpio_set = Z80_CLK;
    settle(pace_settle);
    prev_ctl = bus_service(prev_ctl, &t, false);
}

uint32_t __not_in_flash_func(z80_run_to_opfetch)(const uint32_t max_t_states) {
    const uint32_t before = z80_stats.opfetch;
    for (uint32_t i = 0; i < max_t_states; i++) {
        t_state();
        if (z80_stats.opfetch != before) return z80_stats.last_opfetch_addr;
    }
    return Z80_NO_FETCH;
}

// ============================================================================
// Reset, and the two interrupt lines
// ============================================================================

void z80_reset(void) {
    dbus_release();
    prev_ctl = 0xFFFFFFFFu;
    inta_run = 0;
    sio_hw->gpio_clr = Z80_RST;          // ~RESET low: the CPU is held

    // The Z80 wants three rising edges with ~RESET low. Eight is not
    // caution for its own sake — the internal reset is synchronous, and a
    // clock this firmware generates can have a phase stretched by an
    // interrupt, so counting edges rather than microseconds is the only
    // measure that means anything.
    for (int i = 0; i < 8; i++) t_state();

    sio_hw->gpio_set = Z80_RST;          // released

    /*
     * And no clocks after that, deliberately. The counters are zeroed
     * here so a test reads its own machine rather than the sum of every
     * test before it — which means any T-state run between releasing
     * ~RESET and zeroing them is a bus cycle thrown away. The Z80 needs
     * two clocks after ~RESET rises before it starts, and an earlier
     * version spent them here: if the first opcode fetch had ever landed
     * inside those two, z80_run_to_opfetch() would have gone looking for
     * a fetch that had already happened and reported a CPU that was not
     * running. The caller's own clocks serve the same purpose and are
     * counted.
     */
    const uint32_t t = z80_stats.t_states;
    memset(&z80_stats, 0, sizeof z80_stats);
    z80_stats.t_states = t;
}

/*
 * A Z80 held in reset still drives its control outputs. That is the whole
 * trick: the address and data buses go high-impedance and tell us
 * nothing, but ~MREQ, ~RD, ~WR and ~M1 are pushed to their inactive high
 * and will not be moved by an internal pull.
 *
 * The delay after switching the pull is not for the pad, which settles in
 * nanoseconds — it is for the capacitance of the trace and the socket,
 * which a 50k pull drives slowly. Ten microseconds is about four time
 * constants on a track this length.
 */
uint32_t z80_cpu_present(void) {
    static const uint8_t pins[4] = { Z80_MREQ_PIN, Z80_RD_PIN, Z80_WR_PIN, Z80_M1_PIN };
    uint32_t driven = 0;

    for (unsigned i = 0; i < 4; i++) {
        gpio_set_pulls(pins[i], false, true);       // pull down
        busy_wait_us(10);
        const bool low_with_pulldown = !gpio_get(pins[i]);

        gpio_set_pulls(pins[i], true, false);       // pull up
        busy_wait_us(10);
        const bool high_with_pullup = gpio_get(pins[i]);

        gpio_set_pulls(pins[i], false, false);      // back to floating

        // Followed both pulls: nothing on the other end. Stayed high
        // against the pull-down: something is driving it, and at rest
        // that something can only be a CPU.
        if (!(low_with_pulldown && high_with_pullup)) driven |= 1u << i;
    }

    busy_wait_us(10);
    return driven;
}

void z80_set_int(const bool asserted) {
    gpio_put(Z80_INT_PIN, !asserted);    // active low
}

void z80_pulse_nmi(void) {
    // Edge-triggered, and the edge is what matters: the Z80 latches the
    // falling edge and services it after the current instruction. Two
    // T-states of width is comfortably over the 80 ns the part asks for
    // at any clock this firmware can generate.
    gpio_put(Z80_NMI_PIN, 0);
    z80_run(2);
    gpio_put(Z80_NMI_PIN, 1);
}

// ============================================================================
// Pace and measurement
// ============================================================================

/*
 * The pace is a total, split across the T-state's two half-cycles.
 * Odd totals lean the extra nop onto the second half, which is the one
 * the Z80 has least to say during.
 */
void z80_set_pace_raw(const uint32_t total) {
    // Two is the floor of the mechanism rather than of correctness: the
    // settle loop runs at least once per half-cycle, so that the test
    // for zero can come out of it. See settle().
    const uint32_t t = total < 2 ? 2 : total;
    pace_settle   = t / 2;
    pace_settle_b = t - pace_settle;
}

/*
 * The floor, which starts at a conservative guess and is replaced by a
 * measurement at boot. See selftest_probe_pace_floor().
 */
static uint32_t pace_floor = Z80_PACE_MIN;

void z80_set_pace_floor(const uint32_t floor) {
    pace_floor = floor < Z80_PACE_MIN ? Z80_PACE_MIN : floor;
    if (z80_get_pace() < pace_floor) z80_set_pace_raw(pace_floor);
}

uint32_t z80_get_pace_floor(void) { return pace_floor; }

void z80_set_pace(const uint32_t total) {
    z80_set_pace_raw(total < pace_floor ? pace_floor : total);
}

uint32_t z80_get_pace(void) { return pace_settle + pace_settle_b; }

uint32_t z80_measure_clock(void) {
    constexpr uint32_t sample = 20000;

    const bool traced = z80_trace_on;
    z80_trace_on = false;                // tracing is not free; do not time it

    const uint64_t t0 = time_us_64();
    z80_run(sample);
    const uint64_t t1 = time_us_64();

    z80_trace_on = traced;

    const uint64_t us = t1 - t0;
    return us ? (uint32_t)((uint64_t)sample * 1000000u / us) : 0;
}

uint32_t z80_calibrate(const uint32_t target_hz) {
    uint32_t best_total = 0, best_hz = 0, best_err = UINT32_MAX;

    // Pace only ever makes the CPU slower, so walk up from flat out and
    // stop one step past the target rather than sweeping the whole range.
    // The step is half what it used to be, because a total of N is split
    // across the two half-cycles.
    for (uint32_t t = 0; t <= 128; t++) {
        z80_set_pace(t);
        const uint32_t hz = z80_measure_clock();

        const uint32_t err = hz > target_hz ? hz - target_hz : target_hz - hz;
        if (err < best_err) { best_err = err; best_total = t; best_hz = hz; }

        if (hz < target_hz) break;
    }

    z80_set_pace(best_total);
    return best_hz;
}

// ============================================================================
// Bring-up
// ============================================================================

void z80_bus_init(void) {
    // Address, data and control lines: plain SIO inputs. The data lines
    // become outputs only for as long as the Z80 is reading, and that is
    // done through OE rather than by reconfiguring the pad.
    for (int i = 0; i < Z80_ADDR_BITS; i++) {
        gpio_init(Z80_ADDR_BASE + i);
        gpio_set_dir(Z80_ADDR_BASE + i, GPIO_IN);
    }
    for (int i = 0; i < Z80_DATA_BITS; i++) {
        gpio_init(Z80_DATA_BASE + i);
        gpio_set_dir(Z80_DATA_BASE + i, GPIO_IN);
    }
    static const uint8_t ctrl_pins[] = { Z80_MREQ_PIN, Z80_RD_PIN, Z80_WR_PIN, Z80_M1_PIN };
    for (unsigned i = 0; i < count_of(ctrl_pins); i++) {
        gpio_init(ctrl_pins[i]);
        gpio_set_dir(ctrl_pins[i], GPIO_IN);
    }

    // Clock out, low. ~RESET out, low — which is where R8's 10K pull-down
    // was already holding it, so the CPU has been in reset since power-on
    // and stays there until something asks for it not to be.
    gpio_init(Z80_CLK_PIN);
    gpio_put(Z80_CLK_PIN, 0);
    gpio_set_dir(Z80_CLK_PIN, GPIO_OUT);

    gpio_init(Z80_RESET_PIN);
    gpio_put(Z80_RESET_PIN, 0);
    gpio_set_dir(Z80_RESET_PIN, GPIO_OUT);

    // ~INT and ~NMI idle high. Both have 10K pull-ups on the board, so
    // driving them high is about drive strength rather than level.
    gpio_init(Z80_INT_PIN);
    gpio_put(Z80_INT_PIN, 1);
    gpio_set_dir(Z80_INT_PIN, GPIO_OUT);

    gpio_init(Z80_NMI_PIN);
    gpio_put(Z80_NMI_PIN, 1);
    gpio_set_dir(Z80_NMI_PIN, GPIO_OUT);

    z80_map_flat();
    memset(&z80_stats, 0, sizeof z80_stats);
}
