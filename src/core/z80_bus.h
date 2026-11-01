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
    /*
     * Thirty-two bits, not sixty-four.
     *
     * This is incremented once per T-state on the loop that generates
     * the clock, and a 64-bit increment there is two loads, two adds and
     * two stores where a 32-bit one is one of each. It wraps after about
     * twenty minutes of running, which costs nothing: every use is a
     * difference between two readings, and unsigned subtraction gives
     * the right answer across a wrap.
     */
    uint32_t t_states;
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
// Eight 8 KB windows, and the read path is one shift and two loads with no
// branch:
//
//     z80_rpage[a >> 13][a & 0x1FFF]
//
// It was four 16 KB windows, which is the shape a Spectrum 128 pages on and
// the shape almost every machine here wants. The Timex is the exception: its
// SCLD banks in 8 KB chunks, eight of them, each independently from the home
// memory, the cartridge dock or the EX-ROM — so a 16 KB table cannot express
// its map at all. Halving the window costs nothing on the hot path (the same
// shift and mask against different constants) and four more pointers, so the
// exception decides it.
//
// Most callers still think in 16 KB and should use z80_map_page(), which
// fills the two halves; z80_map_page8() is for the machines that need the
// finer grain.
//
// Writes go through a separate table, so ROM is not a comparison against a
// limit on every cycle — it is a window whose write pointer aims at a bit
// bucket. That is also closer to the truth: a real machine's ROM is not
// write-protected, it simply has nothing connected to ~WR.
#define Z80_PAGES 8
extern uint8_t *z80_rpage[Z80_PAGES];
extern uint8_t *z80_wpage[Z80_PAGES];

/*
 * The flat memory the stage-0 tests use.
 *
 * Not an array of its own. The self-test fills all of it with patterns,
 * so it cannot be the ROM image -- a test that scribbles over the
 * Spectrum's ROM to prove the data bus works is a test that can only be
 * run once -- but it CAN be a page of the guest's RAM, because stage 0
 * finishes before spec_init() puts a machine in it.
 *
 * So the app provides it, and the app points it at RAM bank 0. Sixteen
 * kilobytes of .bss that was only ever alive for the first second after
 * reset is sixteen kilobytes a machine can have instead.
 *
 * Set before core 1 starts; NULL until then, which is why z80_map_flat()
 * checks.
 */
extern uint8_t *z80_mem;

/* Point all four windows at z80_mem, readable and writable. */
void z80_map_flat(void);

/* Aim one 16 KB window. A null write pointer makes the window read-only. */
/*
 * Read and write guest memory the way the bus engine does.
 *
 * Use these rather than indexing the tables by hand. The window size is
 * not a constant anyone should be repeating: it went from 16 KB to 8 KB
 * when the Timex arrived, and four places that had open-coded
 * `z80_rpage[a >> 14][a & 0x3FFF]` silently started reading and writing
 * the wrong addresses -- the tape loader put every block a page out,
 * and the pace probe measured a floor that did not exist.
 */
static inline uint8_t z80_peek(const uint16_t a) {
    return z80_rpage[a >> 13][a & 0x1FFF];
}

static inline void z80_poke(const uint16_t a, const uint8_t v) {
    z80_wpage[a >> 13][a & 0x1FFF] = v;
}

/* One of the four 16 KB windows: fills both of its 8 KB halves. */
void z80_map_page(unsigned slot, uint8_t *read_from, uint8_t *write_to);

/* One of the eight 8 KB windows, for the machines that page that finely. */
void z80_map_page8(unsigned slot, uint8_t *read_from, uint8_t *write_to);

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
//
// Every handler is also told *when*: t is the T-state within the frame at
// which the strobe was seen (see z80_t below). An I/O cycle is seen in its
// T2, so it began at t-1; a memory read or fetch is seen in T1; a write in
// T2. Whatever needs the ULA's view of the cycle -- the border, the EAR
// bit, the floating bus -- applies its own offset to that.
typedef uint8_t (*z80_io_in_fn)(uint16_t port, uint32_t t);
typedef void    (*z80_io_out_fn)(uint16_t port, uint8_t value, uint32_t t);

extern z80_io_in_fn  z80_io_in;
extern z80_io_out_fn z80_io_out;

/*
 * Called on every opcode fetch, *before* the byte is supplied.
 *
 * Null almost always, and the check is one load and a
 * predicted-not-taken branch on the hot path. It exists for the one
 * thing that cannot be done any other way: swapping what is mapped at
 * 0x0000 between the instruction that jumps out of it and the fetch at
 * the destination. A snapshot's restore code lives in the ROM window and
 * has to stop existing at exactly that moment — one fetch earlier and
 * the jump reads its operands from the wrong page, one later and the
 * destination's first byte comes from the trampoline.
 */
typedef void (*z80_opfetch_fn)(uint16_t addr, uint32_t t);
extern z80_opfetch_fn z80_opfetch_cb;

/*
 * Called before a memory write lands, for the windows whose bit is set
 * in z80_wwatch_mask (bit n = window n, the 8 KB one at n * 0x2000).
 *
 * The mask is the whole cost on the hot path -- one load and a test per
 * write cycle -- so a machine that watches nothing pays almost nothing.
 * What watches: the frame builder, for the windows the screen is in, and
 * the bank cache, for the first write to a page it would otherwise not
 * need to write back.
 */
typedef void (*z80_wwatch_fn)(uint16_t addr, uint8_t value, uint32_t t);
extern volatile uint8_t z80_wwatch_mask;
extern z80_wwatch_fn    z80_wwatch_cb;

/*
 * Called AFTER a memory read -- an opcode fetch or a data read -- from
 * the sixteen bytes at z80_rtap_base, once the byte has gone out.
 *
 * For hardware that switches on being read: ProfROM's plane select is a
 * tap on #0100-#010F that fires on data reads (its trampolines are
 * LD L,(HL)) and takes effect for the cycle after. NULL when nothing is
 * armed, so every other machine pays one load and a predicted branch.
 */
typedef void (*z80_rtap_fn)(uint16_t addr, uint32_t t);
extern z80_rtap_fn z80_rtap_cb;
extern uint16_t    z80_rtap_base;

/*
 * Where in the frame the machine is, in T-states.
 *
 * Kept by both loops and handed to every handler, so anything that has
 * to know *when* -- the tape's EAR level, a border change, a sound chip's
 * register write -- knows to the T-state rather than to whichever slice
 * of the frame it happened in. The frame loop sets it to zero at the
 * start of each frame, which is where ~INT goes down.
 */
extern uint32_t z80_t;

/* Opcode fetches from #0000 since boot: resets and RST 0s. A diagnostic. */
extern volatile uint32_t z80_fetch0;

/*
 * The ULA's contention, for the production loop.
 *
 * A contended access does not slow the clock here -- there is no ~WAIT
 * to slow it with -- it moves z80_t on by the delay, with no edges. To
 * the frame that is exactly a CPU held by the ULA; to the CPU, which is
 * static, it is nothing at all. See contend_at() in z80_bus.c.
 */
typedef struct {
    bool           on;          // this machine contends, and it is not in turbo
    bool           io;          // ...and contends I/O too (not the +2A/+3)
    uint8_t        mask;        // which 16 KB windows are contended
    uint32_t       t0;          // the first contended T-state
    uint32_t       line;        // T-states a line
    uint32_t       span;        // 192 lines of them
    const uint8_t *pat;         // the delay at each of the eight positions
} z80_contend_t;
extern z80_contend_t z80_contend;

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

/*
 * Start and stop the stuck-line watch.
 *
 * Recording is not free -- it is a read of GPIO_IN and two globals
 * updated twice per T-state, on the loop that generates the clock -- so
 * it is off until a test asks for it, and has to be turned off again.
 */
void     z80_edge_watch_reset(void);
void     z80_edge_watch_stop(void);
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

/*
 * Clock the CPU for n T-states, answering every cycle it starts.
 *
 * The diagnostic loop: it counts every cycle into z80_stats and feeds the
 * stuck-line watch, which is what stage 0 and the launch checks read.
 * The machine itself runs on z80_run_until(), which does neither.
 */
void z80_run(uint32_t t_states);

/*
 * Clock the CPU until z80_t reaches t_stop.
 *
 * The production loop. The frame's T-state is its loop variable, held in
 * a register and handed to every handler, and nothing is counted that
 * nothing reads -- only the last fetch address, which the settings page
 * shows. Returns with z80_t == t_stop.
 */
void z80_run_until(uint32_t t_stop);

/*
 * Stop the clock in its high half. For any stop that can be long: the
 * CPU keeps its state with the clock held high, and does not reliably
 * with it held low. Whoever is clocking the CPU calls it.
 */
void z80_park(void);

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
// There is no *upper* bound at which the machine becomes incorrect — a
// Z84C00 is fully static, so a phase that runs long is simply a long
// phase. Too short is what breaks, and it breaks at the Z80's minimum
// clock width and its output delay. z80_measure_clock() reports what a
// given pace actually achieves.
/*
 * The fastest the engine may be driven and still be right.
 *
 * Below some settle time bus_service() samples the Z80's pins before
 * they are valid and reads the previous cycle's address. The stage-0
 * speed ramp is what measures it: on this board the CPU answers
 * correctly at a pace of 6 and gives the wrong answer at 4.
 *
 * Eight, because the boundary is not a constant. It moves with
 * temperature, with the code's layout and with whatever else the frame
 * loop is doing -- and anything that tunes for a frame *rate* will walk
 * straight through it, because that arithmetic cannot tell "the machine
 * is slow" from "the machine is wrong". Which is a failure like no
 * other: the Spectrum boots, the ROM runs, the self-tests pass, the
 * frame rate reads 50.5 Hz, and the screen is noise. Two steps of margin
 * is cheap.
 */
#define Z80_PACE_MIN 8

/*
 * Set the padding, clamped to Z80_PACE_MIN.
 *
 * Everything that tunes for speed goes through this. The stage-0 speed
 * ramp, whose whole job is to find where the engine stops being right,
 * uses z80_set_pace_raw() instead.
 */
void     z80_set_pace(uint32_t settle_iterations);
void     z80_set_pace_raw(uint32_t settle_iterations);

/*
 * Replace the floor with one that has been measured on this board.
 *
 * Z80_PACE_MIN is a guess made once; the real boundary moves with the
 * system clock, with the size of the service routine and with how warm
 * the board is. selftest_probe_pace_floor() finds it at boot and hands
 * it to this.
 */
void     z80_set_pace_floor(uint32_t floor);
uint32_t z80_get_pace_floor(void);
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
