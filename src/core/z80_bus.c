/*
 * frank-dino — an RP2350B acting as the whole machine around a real Z80
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-dino
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "z80_bus.h"

#include <string.h>
#include <hardware/gpio.h>
#include <pico/time.h>

// ============================================================================
// State
// ============================================================================
z80_stats_t z80_stats;

uint8_t  z80_mem[GUEST_MEM_SIZE] __attribute__((aligned(4)));
uint16_t z80_rom_limit = 0;

uint8_t  z80_int_vector = 0xFF;

volatile bool     z80_trace_on = false;
bus_event_t       z80_trace[BUS_TRACE_DEPTH];
volatile uint32_t z80_trace_pos = 0;

static uint8_t io_in_default(uint16_t port);
static void    io_out_default(uint16_t port, uint8_t val) { (void)port; (void)val; }

z80_io_in_fn  z80_io_in  = io_in_default;
z80_io_out_fn z80_io_out = io_out_default;

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

static uint8_t io_in_default(uint16_t port)               { (void)port; return BUS_FLOAT; }

// How many consecutive quiet samples with ~M1 low and ~MREQ high mean an
// interrupt acknowledge rather than the first half-T-state of an ordinary
// opcode fetch. Sampling is twice per T-state, and on a fetch ~MREQ falls
// at the end of T1 — so two samples in it is normal and three is not.
#define INTA_SETTLE 3

static uint32_t pace_settle = 24;

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

__force_inline static void settle(void) {
    uint32_t n = pace_settle;
    while (n--) __asm volatile("nop" ::: "memory");
}

// ============================================================================
// Memory
// ============================================================================

__force_inline static uint8_t mem_read(const uint16_t a) {
    return z80_mem[a];
}

__force_inline static void mem_write(const uint16_t a, const uint8_t v) {
    if (likely(a >= z80_rom_limit)) z80_mem[a] = v;
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

__force_inline static void bus_service(void) {
    const uint32_t g    = sio_hw->gpio_in;
    const uint32_t fell = prev_ctl & ~g;
    prev_ctl = g;

    if (fell & Z80_RD) {                    // a read cycle begins
        const uint16_t a = (uint16_t)(g & Z80_ADDR_MASK);
        uint8_t v;
        bus_cycle_t kind;

        if (!(g & Z80_MREQ)) {
            v = mem_read(a);
            if (!(g & Z80_M1)) {
                kind = BUS_OPFETCH;
                z80_stats.opfetch++;
                z80_stats.last_opfetch_addr = a;
            } else {
                kind = BUS_MEM_READ;
                z80_stats.mem_read++;
            }
        } else {
            // ~MREQ idle with ~RD active is an I/O read and nothing else.
            v = z80_io_in(a);
            kind = BUS_IO_READ;
            z80_stats.io_read++;
        }

        dbus_drive(v);
        z80_stats.last_addr = a;
        z80_stats.last_data = v;
        trace_put(a, v, kind);
        inta_run = 0;
        return;
    }

    if (!(g & Z80_RD)) return;              // still reading; the byte is up

    if (fell & Z80_WR) {                    // a write cycle begins
        // The data lines are inputs: they were released on the sample
        // that ended the last read, and a write cannot follow a read
        // without one in between.
        const uint16_t a = (uint16_t)(g & Z80_ADDR_MASK);
        const uint8_t  v = (uint8_t)((g & Z80_DATA_MASK) >> Z80_DATA_BASE);

        if (!(g & Z80_MREQ)) {
            mem_write(a, v);
            z80_stats.mem_write++;
            trace_put(a, v, BUS_MEM_WRITE);
        } else {
            z80_io_out(a, v);
            z80_stats.io_write++;
            trace_put(a, v, BUS_IO_WRITE);
        }
        z80_stats.last_addr = a;
        z80_stats.last_data = v;
        inta_run = 0;
        return;
    }

    if (!(g & Z80_WR)) return;              // still writing

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
            z80_stats.int_ack++;
            trace_put((uint16_t)(g & Z80_ADDR_MASK), z80_int_vector, BUS_INT_ACK);
            dbus_drive(z80_int_vector);
        }
        return;                             // the vector stays up until ~M1 rises
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

__force_inline static void t_state(void) {
    sio_hw->gpio_set = Z80_CLK;
    settle();
    bus_service();
    uint32_t g = sio_hw->gpio_in;
    seen_high |= g;
    seen_low  |= ~g;

    sio_hw->gpio_clr = Z80_CLK;
    settle();
    bus_service();
    g = sio_hw->gpio_in;
    seen_high |= g;
    seen_low  |= ~g;

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

void z80_edge_watch_reset(void) { seen_high = 0; seen_low = 0; }

uint32_t z80_edge_watch_stuck_high(void) { return (~seen_low)  & Z80_ADDR_MASK; }
uint32_t z80_edge_watch_stuck_low(void)  { return (~seen_high) & Z80_ADDR_MASK; }

void z80_run(uint32_t t_states) {
    while (t_states--) t_state();
}

uint32_t z80_run_to_opfetch(const uint32_t max_t_states) {
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
    const uint64_t t = z80_stats.t_states;
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

void     z80_set_pace(const uint32_t settle_iterations) { pace_settle = settle_iterations; }
uint32_t z80_get_pace(void)                             { return pace_settle; }

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

    memset(&z80_stats, 0, sizeof z80_stats);
}
