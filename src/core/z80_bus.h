/*
 * frank-dino — an RP2350B acting as the whole machine around a real Z80
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-dino
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * The bus engine: the RP2350 is the Z80's clock, its memory and its I/O.
 *
 * Every Z80 bus cycle is answered by software running in step with a
 * clock that same software generates. The CPU is not emulated — it
 * fetches, it drives ~MREQ and ~RD and ~WR, and this file is everything
 * on the other side of those pins.
 */
#pragma once

#include "dino.h"

// ============================================================================
// What the engine saw
// ============================================================================
typedef enum : uint8_t {
    BUS_NONE = 0,
    BUS_OPFETCH,        // ~M1 + ~MREQ + ~RD — an opcode fetch
    BUS_MEM_READ,       // ~MREQ + ~RD
    BUS_MEM_WRITE,      // ~MREQ + ~WR
    BUS_IO_READ,        // ~RD with ~MREQ idle — inferred, see dino.h
    BUS_IO_WRITE,       // ~WR with ~MREQ idle
    BUS_INT_ACK,        // ~M1 with ~MREQ never following
} bus_cycle_t;

typedef struct {
    uint16_t    addr;
    uint8_t     data;
    bus_cycle_t kind;
} bus_event_t;

// ============================================================================
// Counters
// ============================================================================
//
// Every one of these is a question that has been asked at the bench with
// a scope and answered wrongly. "Is it fetching?" and "is it fetching
// from where I think?" are different questions, and a CPU that has lost
// its mind usually still fetches.
typedef struct {
    uint64_t t_states;
    uint32_t opfetch;
    uint32_t mem_read;
    uint32_t mem_write;
    uint32_t io_read;
    uint32_t io_write;
    uint32_t int_ack;
    uint16_t last_opfetch_addr;
    uint16_t last_addr;
    uint8_t  last_data;
} z80_stats_t;

extern z80_stats_t z80_stats;

// ============================================================================
// Guest memory
// ============================================================================
//
// Four 16 KB windows, which is the shape every Z80 machine worth emulating
// already has. The Z80's address space divides into four naturally, a
// Spectrum 128 pages exactly on those boundaries, and the read path becomes
// one shift and two loads with no branch:
//
//     z80_rpage[a >> 14][a & 0x3FFF]
//
// Writes go through a separate table, so ROM is not a comparison against a
// limit on every cycle — it is a window whose write pointer aims at a bit
// bucket. That is also closer to the truth: a real machine's ROM is not
// write-protected, it simply has nothing connected to ~WR.
extern uint8_t *z80_rpage[4];
extern uint8_t *z80_wpage[4];

/*
 * The flat 64 KB the stage-0 tests use.
 *
 * Kept as its own array rather than carved out of a machine's RAM: the
 * self-test fills all of it with patterns, and a test that scribbles over
 * the Spectrum's ROM image to prove the data bus works is a test that can
 * only be run once.
 */
extern uint8_t z80_mem[GUEST_MEM_SIZE];

/* Point all four windows at z80_mem, readable and writable. */
void z80_map_flat(void);

/* Aim one 16 KB window. A null write pointer makes the window read-only. */
void z80_map_page(unsigned slot, uint8_t *read_from, uint8_t *write_to);

// ============================================================================
// I/O
// ============================================================================
//
// Function pointers rather than a switch, because in stage 0 the thing on
// the other end of an OUT is a test, and each test wants a different one.
// The cost is an indirect call on a path the Z80 takes rarely; the memory
// path, which it takes on every single cycle, stays inlined.
//
// The port argument is the full 16-bit address the Z80 put out: A0-A7 is
// the port for `OUT (n),A`, and A8-A15 carries B for `OUT (C),r`. A ZX
// Spectrum decodes on A0 alone and on the high half for the keyboard, so
// handing over the whole word rather than the low byte is not generality
// for its own sake.
typedef uint8_t (*z80_io_in_fn)(uint16_t port);
typedef void    (*z80_io_out_fn)(uint16_t port, uint8_t value);

extern z80_io_in_fn  z80_io_in;
extern z80_io_out_fn z80_io_out;

// ============================================================================
// The trace ring
// ============================================================================
//
// The last BUS_TRACE_DEPTH cycles, newest last. Off by default: recording
// costs a store and a mask on every cycle, which is most of the reason the
// Z80's clock is what it is. Turn it on for one test, read it, turn it off.
#define BUS_TRACE_DEPTH 256

extern volatile bool     z80_trace_on;
extern bus_event_t       z80_trace[BUS_TRACE_DEPTH];
extern volatile uint32_t z80_trace_pos;     // total events, not an index

void        z80_trace_reset(void);
uint32_t    z80_trace_count(void);          // how many entries are valid
bus_event_t z80_trace_get(uint32_t i);      // i = 0 is the oldest kept

// ============================================================================
// Driving the CPU
// ============================================================================

/* Take the pins, hold the Z80 in reset, leave the clock low. */
void z80_bus_init(void);

/*
 * Is there a CPU in the socket, and is it powered?
 *
 * Asked of the control lines, not the address lines, because a Z80 held
 * in reset tri-states its address and data buses but keeps driving ~MREQ,
 * ~RD, ~WR and ~M1 to their inactive high. So: pull each one down, look,
 * pull it up, look. A pin that follows the pull is connected to nothing
 * that cares — an empty socket, a dead 5V rail, or a cold joint. A pin
 * that stays high against a 50k pull-down has a CPU behind it.
 *
 * Returns a mask of the control pins that appear to be driven, in the
 * order MREQ, RD, WR, M1 — bit 0 to bit 3. All four means a CPU; none
 * means nothing is there; some means the interesting kind of broken, and
 * naming which ones is the whole reason this returns a mask rather than a
 * bool.
 */
#define Z80_PRESENT_MREQ 0x1
#define Z80_PRESENT_RD   0x2
#define Z80_PRESENT_WR   0x4
#define Z80_PRESENT_M1   0x8
#define Z80_PRESENT_ALL  0xF
uint32_t z80_cpu_present(void);

/*
 * Which of A0-A15 never moved.
 *
 * Reset it *after* a z80_reset(), not before: the Z80 tri-states its
 * address bus while ~RESET is low, and a floating pin reads as exercised.
 * Then run something that should have driven every line, and anything
 * still in the mask is stuck — a short to a rail, a cold joint, or a bent
 * pin under the socket. Bit 0 is A0.
 *
 * Address lines only. See z80_bus.c for why a data line cannot be tested
 * this way.
 */
/*
 * The bus pins, right now, as one raw word — GP0-GP27 exactly as the
 * engine sees them. For reporting what a wedged machine is sitting on
 * when no test has an opinion about it yet.
 */
uint32_t z80_raw_bus(void);

/*
 * Hold the CPU in reset, or let it go, without clocking it.
 *
 * A Z80 in reset tri-states its address and data buses. That is what
 * makes the wiring tests below mean anything: with the CPU not driving,
 * the only thing that can hold one of those lines is a fault.
 */
void z80_hold_reset(bool held);

/*
 * Which of A0-A15 and D0-D7 will not follow an internal pull.
 *
 * Run with the CPU in reset. Each line is pulled up, read, pulled down,
 * read. A line that reports the same level both times is shorted to a
 * rail or driven by something that should not be driving it. Returns a
 * mask over GP0-GP23 — bit 0 is A0, bit 16 is D0.
 *
 * This is the test that separates "the RP2350's pin is broken" from
 * "the wire to the CPU is broken", and it is worth having as firmware
 * rather than as a meter probe because it checks all twenty-four at once
 * and because a short between two bus lines is invisible to a meter that
 * is only looking at one of them.
 */
uint32_t z80_wiring_stuck(void);

/*
 * Shorts between bus lines.
 *
 * Run with the CPU in reset. Drives each of the twenty-four lines high in
 * turn with every other line an input pulled down, and records which
 * others came up with it. out[] must have 24 entries; out[i] is a mask of
 * the lines that followed line i, and should be zero for every i.
 *
 * A solder bridge on the RP2350, on the socket, or across two of the
 * 200R data resistors all look like this and like nothing else.
 */
void z80_wiring_shorts(uint32_t out[24]);

/*
 * Which lines cannot be pulled high even by the pad driving them.
 *
 * This is the test that says *which side* of R10-R17 a stuck data line
 * is on, and it works because those 200R resistors are in the way.
 *
 * A pad driving high is a few tens of ohms. Against a short to ground on
 * the far side of a 200R resistor it still wins — the node sits around
 * 2.6 V, comfortably a logic high — so the line reads back high. Against
 * a short on *this* side of the resistor it does not, and the line reads
 * back low.
 *
 * So, combined with z80_wiring_stuck():
 *
 *   won't follow a 50k pull, but drives high   the fault is past the
 *                                              resistor: the CPU, its
 *                                              socket, or the resistor's
 *                                              far pad
 *   won't drive high either                    the fault is this side:
 *                                              the RP2350's pin, its
 *                                              joint, or the track
 *
 * Returns a mask over GP0-GP23. Run with the CPU in reset.
 */
uint32_t z80_wiring_undriveable(void);

/* Level each line sits at with no pull and nothing driving it. */
uint32_t z80_wiring_levels(void);

void     z80_edge_watch_reset(void);
uint32_t z80_edge_watch_stuck_high(void);
uint32_t z80_edge_watch_stuck_low(void);

/*
 * Reset the CPU and leave it fetching from 0x0000.
 *
 * The Z80 wants ~RESET low across at least three rising clock edges; this
 * gives it eight and then clocks two more with ~RESET released, which is
 * where the internal reset sequence finishes. It does not run any
 * instruction — the next thing z80_run() sees is the fetch of the byte at
 * 0x0000.
 */
void z80_reset(void);

/* Clock the CPU for n T-states, answering every cycle it starts. */
void z80_run(uint32_t t_states);

/*
 * Clock until the next opcode fetch, or until the budget runs out.
 *
 * Returns the address fetched from, or Z80_NO_FETCH if the budget was
 * exhausted first — which is the answer that matters, because a CPU that
 * is not fetching is the failure this whole firmware exists to catch.
 */
#define Z80_NO_FETCH 0xFFFFFFFFu
uint32_t z80_run_to_opfetch(uint32_t max_t_states);

/* ~INT and ~NMI, active low on the pin and active true here. */
void z80_set_int(bool asserted);
void z80_pulse_nmi(void);

/*
 * The byte put on the bus during an interrupt acknowledge.
 *
 * 0xFF is RST 38h, which is what an unterminated Spectrum bus reads as
 * and what IM 1 does anyway. It only matters in IM 0 and IM 2.
 */
extern uint8_t z80_int_vector;

// ============================================================================
// Pace
// ============================================================================
//
// How long the engine waits after each clock edge before it samples the
// Z80's pins, in loop iterations. This is the one knob that sets how fast
// the CPU runs, and it is a delay rather than a frequency because that is
// what it physically is: the Z80's outputs are valid some nanoseconds
// after an edge and sampling earlier reads the previous cycle.
//
// There is no lower bound at which the machine becomes *incorrect* in the
// way a free-running CPU would — a Z84C00 is fully static, so a phase
// that runs long is simply a long phase. Too short is what breaks, and it
// breaks at the Z80's minimum clock width and its output delay, not at
// ours. z80_measure_clock() reports what a given pace actually achieves.
void     z80_set_pace(uint32_t settle_iterations);
uint32_t z80_get_pace(void);

/* Achieved Z80 clock in Hz, measured over a real run rather than derived. */
uint32_t z80_measure_clock(void);

/*
 * Find the pace that runs the CPU closest to target_hz, and set it.
 * Returns the clock actually achieved.
 *
 * Measured rather than computed, because the loop's speed depends on the
 * system clock, on what the compiler did with the service routine, and on
 * how often an interrupt lands in the middle of a clock phase. A Spectrum
 * wants 3.5 MHz and every note it plays is flat or sharp by whatever this
 * misses by, so it is worth spending a few milliseconds at boot on.
 */
uint32_t z80_calibrate(uint32_t target_hz);
