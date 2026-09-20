/*
 * frank-dino — an RP2350B acting as the whole machine around a real Z80
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-dino
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "selftest.h"

#include <stdio.h>
#include <string.h>

#include "z80_bus.h"
#include "z80_progs.h"
#include "screen.h"

static selftest_result_t res;

static void record(const bool ok) {
    res.run++;
    if (ok) res.passed++; else res.failed++;
}

// ============================================================================
// Loading a program
// ============================================================================
//
// The whole 64 KB is cleared to 0xFF first, and 0xFF matters: it is
// RST 38h. A CPU that runs off the end of a test program lands somewhere
// definite and loudly instead of sliding through 64 KB of 0x00 looking
// like a healthy machine executing NOPs.
static void load(const uint16_t at, const uint8_t *code, const size_t len) {
    memcpy(&z80_mem[at], code, len);
}

static void wipe(void) {
    memset(z80_mem, 0xFF, GUEST_MEM_SIZE);
    z80_map_flat();    // the tests want the plain 64 KB, not a machine
}

// ============================================================================
// The I/O handlers the I/O test watches
// ============================================================================
static volatile uint16_t io_seen_port;
static volatile uint8_t  io_seen_value;
static volatile uint32_t io_seen_writes;
static volatile uint8_t  io_answer = 0x00;

static void test_io_out(const uint16_t port, const uint8_t value) {
    io_seen_port  = port;
    io_seen_value = value;
    io_seen_writes++;
}

static uint8_t test_io_in(const uint16_t port) {
    (void)port;
    return io_answer;
}

// ============================================================================
// 1. Is there a CPU
// ============================================================================
static bool test_presence(void) {
    const uint32_t m = z80_cpu_present();
    if (m == Z80_PRESENT_ALL) {
        scr_pass("CPU present", "MREQ RD WR M1 all driven");
        return true;
    }
    if (m == 0) {
        scr_fail("CPU present", "no control line is driven - empty socket or no 5V");
        return false;
    }
    scr_fail("CPU present", "only %s%s%s%s driven",
             (m & Z80_PRESENT_MREQ) ? "MREQ " : "",
             (m & Z80_PRESENT_RD)   ? "RD "   : "",
             (m & Z80_PRESENT_WR)   ? "WR "   : "",
             (m & Z80_PRESENT_M1)   ? "M1 "   : "");
    return false;
}

// ============================================================================
// 1b. The wiring, checked against itself
// ============================================================================
//
// With the CPU held in reset its address and data buses are
// high-impedance, so every one of the twenty-four lines should follow an
// internal pull, and driving any one of them should move that one alone.
//
// This runs before anything that asks the CPU a question, because a line
// that is shorted or open makes every later test lie: the address test
// reports "30/30 bad" and the data test "20/20 bad" and neither says
// which wire, because by then the answers are coming back through the
// fault.
static const char *bus_line_name(unsigned i) {
    static char buf[6];
    snprintf(buf, sizeof buf, "%c%u", i < 16 ? 'A' : 'D', i < 16 ? i : i - 16);
    return buf;
}

static void name_mask(char *out, size_t n, uint32_t mask) {
    out[0] = 0;
    size_t at = 0;
    for (unsigned i = 0; i < 24 && at + 5 < n; i++)
        if (mask & (1u << i))
            at += (size_t)snprintf(out + at, n - at, "%s ", bus_line_name(i));
}

static bool test_wiring(void) {
    z80_hold_reset(true);

    const uint32_t stuck        = z80_wiring_stuck();
    const uint32_t levels       = z80_wiring_levels();
    const uint32_t undriveable  = z80_wiring_undriveable();

    uint32_t shorts[24];
    z80_wiring_shorts(shorts);

    uint32_t shorted_lines = 0;
    for (unsigned i = 0; i < 24; i++)
        if (shorts[i]) shorted_lines |= (1u << i) | shorts[i];

    z80_hold_reset(false);

    if (!stuck && !shorted_lines) {
        scr_pass("Bus wiring (CPU in reset)", "all 24 lines free and independent");
        return true;
    }

    char names[64];
    if (stuck) {
        name_mask(names, sizeof names, stuck);
        scr_fail("Bus wiring (CPU in reset)", "will not follow a pull: %s", names);

        name_mask(names, sizeof names, stuck & levels);
        if (stuck & levels)  scr_log(ATTR(C_YELLOW, C_BLACK), "  held HIGH: %s", names);
        name_mask(names, sizeof names, stuck & ~levels);
        if (stuck & ~levels) scr_log(ATTR(C_YELLOW, C_BLACK), "  held LOW:  %s", names);

        /*
         * Which side of the 200R data resistors. This is the line that
         * decides whether to reach for a different CPU or a soldering
         * iron, so it says so rather than leaving it to be inferred.
         */
        if (undriveable & stuck) {
            name_mask(names, sizeof names, undriveable & stuck);
            scr_log(ATTR(C_LRED, C_BLACK), "  cannot even be driven high: %s", names);
            scr_log(ATTR(C_LRED, C_BLACK), "  -> fault is on the RP2350 side of R10-R17");
        } else {
            name_mask(names, sizeof names, stuck & ~undriveable);
            scr_log(ATTR(C_LGREEN, C_BLACK), "  but drive high fine: %s", names);
            scr_log(ATTR(C_YELLOW, C_BLACK), "  -> fault is past R10-R17: the CPU or its socket");
        }
    }
    if (shorted_lines) {
        // Four lines at most. A real short involves two lines; a result
        // where every line is shorted to every other is a fault in the
        // measurement, and printing all twenty-four of those scrolls the
        // rest of the report off the screen — which is how the first one
        // was nearly missed.
        unsigned shown = 0, total = 0;
        for (unsigned i = 0; i < 24; i++) {
            if (!shorts[i]) continue;
            total++;
            if (shown++ >= 2) continue;
            name_mask(names, sizeof names, shorts[i]);
            scr_log(ATTR(C_LRED, C_BLACK), "  %s shorted to: %s",
                    bus_line_name(i), names);
        }
        if (total > 2)
            scr_log(ATTR(C_YELLOW, C_BLACK), "  ...and %u more lines", total - 2);
        if (!stuck)
            scr_fail("Bus wiring (CPU in reset)", "%u lines shorted together", total);
    }
    return false;
}

// ============================================================================
// 2. Reset, and the first fetch
// ============================================================================
static bool test_reset(void) {
    wipe();
    z80_mem[0x0000] = 0x00;                 // NOP, so the fetch has somewhere to go

    z80_reset();
    const uint32_t a = z80_run_to_opfetch(64);

    if (a == Z80_NO_FETCH) {
        scr_fail("Reset and first fetch", "no opcode fetch in 64 T-states");
        return false;
    }
    if (a != 0x0000) {
        scr_fail("Reset and first fetch", "fetched from %04lX, expected 0000", (unsigned long)a);
        return false;
    }
    scr_pass("Reset and first fetch", "M1 at 0000 after reset");
    return true;
}

// ============================================================================
// 2b. The first cycles, shown rather than judged
// ============================================================================
//
// Not a pass/fail test. It fills memory with 00h — NOP — resets, and puts
// the first two dozen bus cycles on the screen exactly as the engine saw
// them.
//
// It exists because every failure below this point produces the same
// symptom from the outside ("the answer was wrong") and a completely
// different cause, and the address sequence separates them in one glance:
//
//   M1 0000, M1 0001, M1 0002 ...   the CPU is receiving 00h. The data
//                                   bus works in the write direction and
//                                   the fault is elsewhere.
//   M1 0000, MW FFFE, MW FFFD,
//   M1 0038 ...                     the CPU received FFh, not the 00h we
//                                   drove: RST 38h, pushing its PC. Our
//                                   drive is not reaching it.
//   anything else                   the address bus is not being read
//                                   correctly, and the numbers say which
//                                   bits.
//
// A bring-up firmware that can only say "failed" is a bring-up firmware
// that sends you back to the oscilloscope.
static const char *kind_name(bus_cycle_t k);

static void diag_pass(const uint8_t fill, const char *label) {
    memset(z80_mem, fill, GUEST_MEM_SIZE);
    z80_map_flat();    // the tests want the plain 64 KB, not a machine

    z80_trace_reset();
    z80_trace_on = true;
    z80_reset();
    z80_edge_watch_reset();     // after the reset: the bus floats during one
    z80_run(64);
    z80_trace_on = false;

    const uint32_t n = z80_trace_count();
    if (n == 0) {
        scr_log(ATTR(C_LRED, C_BLACK), "  %s: no bus cycles at all in 64 T-states", label);
        return;
    }

    /*
     * Which address lines never moved while the CPU was running.
     *
     * Sixty-four T-states of one-byte instructions walks the program
     * counter through roughly the first sixteen addresses, so A0-A3 must
     * all have been high at some point and anything below A4 that was
     * not is stuck — at the CPU's own pin, which the wiring test cannot
     * see because that test runs with the CPU in reset and its outputs
     * high-impedance. An output driver that fails only when it is driving
     * is exactly the fault that gets past both halves otherwise.
     */
    const uint32_t never_high = z80_edge_watch_stuck_low() & 0x000Fu;
    if (never_high) {
        char names[48];
        int at = 0;
        names[0] = 0;
        for (unsigned b = 0; b < 4; b++)
            if (never_high & (1u << b))
                at += snprintf(names + at, sizeof names - (size_t)at, "A%u ", b);
        scr_log(ATTR(C_LRED, C_BLACK),
                "  %sA0-A3 never went high: %s - stuck at the CPU pin", label, names);
    }

    char line[80];
    for (uint32_t i = 0; i < n && i < 12; i += 6) {
        int at = 0;
        line[0] = 0;
        for (uint32_t j = i; j < i + 6 && j < n; j++) {
            const bus_event_t e = z80_trace_get(j);
            at += snprintf(line + at, sizeof line - (size_t)at,
                           "%s%04X:%02X ", kind_name(e.kind), e.addr, e.data);
        }
        scr_log(ATTR(C_LCYAN, C_BLACK), "  %s%s", i == 0 ? label : "      ", line);
    }
}

/*
 * Not a pass/fail test: the first bus cycles after reset, twice, with
 * every byte of memory 00h and then FFh.
 *
 * Every failure below this point looks the same from outside ("the answer
 * was wrong") and has a completely different cause. Comparing two passes
 * separates them in one glance, because the two fills are instructions
 * that behave nothing alike:
 *
 *   00h is NOP. If the CPU is receiving it, the fetch addresses count
 *   0000, 0001, 0002 and the trace is a straight line.
 *
 *   FFh is RST 38h. If the CPU is receiving *that*, it pushes its program
 *   counter — MW FFFE, MW FFFD — and fetches from 0038, and keeps doing
 *   it down the stack.
 *
 * So two different-looking traces mean the data bus works in the
 * direction that matters and the fault is further on. Two identical
 * traces mean the CPU is not receiving what we drive at all, and is
 * running on whatever the bus floats to — which is the one failure the
 * pass/fail tests above cannot distinguish from a dead CPU.
 *
 * A bring-up firmware that can only say "failed" sends you back to the
 * oscilloscope.
 */
static void diag_first_cycles(void) {
    const uint32_t raw = z80_raw_bus();
    scr_log(ATTR(C_WHITE, C_BLACK),
            "Bus at rest: A=%04X D=%02X  MREQ=%u RD=%u WR=%u M1=%u",
            (unsigned)(raw & 0xFFFF), (unsigned)((raw >> 16) & 0xFF),
            (unsigned)((raw >> 24) & 1), (unsigned)((raw >> 25) & 1),
            (unsigned)((raw >> 26) & 1), (unsigned)((raw >> 27) & 1));

    scr_log(ATTR(C_WHITE, C_BLACK), "First cycles after reset, all memory:");
    diag_pass(0x00, "00 ");
    diag_pass(0xFF, "FF ");
}

// ============================================================================
// 3. The address bus, one line at a time
// ============================================================================
//
// Walking ones then walking zeros, thirty-two targets. Each is reached by
// a single JP from 0000h, so nothing carries over from the previous one —
// a chained walk would report every target after the first failure as
// broken too.
static bool test_address_bus(void) {
    uint16_t targets[32];
    int n = 0;
    for (int b = 0; b < 16; b++) targets[n++] = (uint16_t)(1u << b);
    for (int b = 0; b < 16; b++) targets[n++] = (uint16_t)~(1u << b);

    uint32_t bad_bits = 0;
    uint16_t first_want = 0, first_got = 0;
    int failures = 0;

    for (int i = 0; i < n; i++) {
        const uint16_t t = targets[i];

        // A JP whose operand lands on top of its own opcode cannot be
        // tested this way; 0000h and 0001h and 0002h are the jump itself.
        if (t <= 0x0002) continue;

        wipe();
        const uint8_t jp[] = PROG_JP((uint8_t)(t & 0xFF), (uint8_t)(t >> 8));
        load(0x0000, jp, sizeof jp);
        z80_mem[t] = 0x00;                  // a NOP to be fetched at the target

        z80_reset();
        const uint32_t first = z80_run_to_opfetch(64);
        const uint32_t got   = z80_run_to_opfetch(64);

        if (first != 0x0000 || got != t) {
            if (!failures) { first_want = t; first_got = (uint16_t)got; }
            failures++;
            if (got != Z80_NO_FETCH) bad_bits |= (uint32_t)(got ^ t);
        }
    }

    if (!failures) {
        scr_pass("Address bus A0-A15", "30 targets, walking ones and zeros");
        return true;
    }
    scr_fail("Address bus A0-A15", "%d/%d bad, first %04X read as %04X, bits %04lX",
             failures, n - 2, first_want, first_got, (unsigned long)(bad_bits & 0xFFFF));

    /*
     * Before believing that, check whether it is really the data bus.
     *
     * Each target is reached by a JP whose two operand bytes arrive over
     * D0-D7 — the low byte becomes A0-A7 and the high byte A8-A15. So a
     * single stuck data line b corrupts the address in exactly two
     * places, bit b and bit b+8, and the address test reports a fault it
     * does not have.
     *
     * That is not hypothetical: a Z80 with D6 stuck high failed this
     * test with "bits 4040" and sent a morning after A6 and A14.
     */
    const uint32_t lo = bad_bits & 0xFFu, hi = (bad_bits >> 8) & 0xFFu;
    if (lo && lo == hi)
        scr_log(ATTR(C_YELLOW, C_BLACK),
                "  bits %02lX repeat in both halves - that is a DATA line, not an address one",
                (unsigned long)lo);
    return false;
}

// ============================================================================
// 4. The data bus, one line at a time
// ============================================================================
//
// Each value makes the round trip: out to the CPU as the operand of
// LD A,n and back from it as the data of LD (nn),A. A line stuck or
// shorted fails in one direction or the other, and the XOR of what was
// asked for against what came back names it.
static bool test_data_bus(void) {
    uint8_t values[36];
    int n = 0;
    values[n++] = 0x00; values[n++] = 0xFF;
    values[n++] = 0x55; values[n++] = 0xAA;
    for (int b = 0; b < 8; b++) values[n++] = (uint8_t)(1u << b);
    for (int b = 0; b < 8; b++) values[n++] = (uint8_t)~(1u << b);

    uint32_t bad_bits = 0;
    uint8_t first_want = 0, first_got = 0;
    int failures = 0;

    for (int i = 0; i < n; i++) {
        wipe();
        const uint8_t prog[] = PROG_DATA_WALK(values[i]);
        load(0x0000, prog, sizeof prog);
        z80_mem[PROG_DATA_WALK_RESULT] = (uint8_t)~values[i];   // never the answer

        z80_reset();
        z80_run(PROG_DATA_WALK_TSTATES);

        const uint8_t got = z80_mem[PROG_DATA_WALK_RESULT];
        if (got != values[i]) {
            if (!failures) { first_want = values[i]; first_got = got; }
            failures++;
            bad_bits |= (uint32_t)(got ^ values[i]);
        }
    }

    if (!failures) {
        scr_pass("Data bus D0-D7", "%d values round-tripped", n);
        return true;
    }
    scr_fail("Data bus D0-D7", "%d/%d bad, first %02X read as %02X, bits %02lX",
             failures, n, first_want, first_got, (unsigned long)(bad_bits & 0xFF));
    return false;
}

// ============================================================================
// 5. I/O cycles, inferred without ~IORQ
// ============================================================================
static bool test_io(void) {
    const uint8_t value = 0x3C;
    const uint8_t port  = 0xFE;             // the ZX Spectrum's, as it happens
    const uint8_t answer = 0xC3;

    wipe();
    const uint8_t prog[] = PROG_IO(value, port);
    load(0x0000, prog, sizeof prog);
    z80_mem[PROG_IO_RESULT] = 0x00;

    io_seen_port = 0xFFFF; io_seen_value = 0; io_seen_writes = 0;
    io_answer = answer;

    const z80_io_in_fn  saved_in  = z80_io_in;
    const z80_io_out_fn saved_out = z80_io_out;
    z80_io_in  = test_io_in;
    z80_io_out = test_io_out;

    const uint32_t mem_writes_before = z80_stats.mem_write;
    z80_reset();
    z80_run(PROG_IO_TSTATES);
    const uint32_t mem_writes = z80_stats.mem_write - mem_writes_before;

    z80_io_in  = saved_in;
    z80_io_out = saved_out;

    if (io_seen_writes == 0) {
        // The most likely wrong answer, and worth naming: if the engine
        // read ~MREQ as active during an I/O cycle, the OUT would have
        // landed in memory at the port number instead.
        scr_fail("I/O without IORQ", "OUT never seen; %lu memory writes instead",
                 (unsigned long)mem_writes);
        return false;
    }
    if ((io_seen_port & 0xFF) != port || io_seen_value != value) {
        scr_fail("I/O without IORQ", "OUT was port %04X value %02X, expected %02X/%02X",
                 io_seen_port, io_seen_value, port, value);
        return false;
    }
    if (z80_mem[PROG_IO_RESULT] != answer) {
        scr_fail("I/O without IORQ", "IN returned %02X, expected %02X",
                 z80_mem[PROG_IO_RESULT], answer);
        return false;
    }

    // The high half of the address during OUT (n),A is A, which is what a
    // Spectrum's keyboard decode depends on. Report it rather than assert
    // it: getting it wrong is a real finding, not a pass/fail.
    scr_pass("I/O without IORQ", "OUT %02X=%02X, IN=%02X, A8-15=%02X",
             port, value, answer, io_seen_port >> 8);
    return true;
}

// ============================================================================
// 6. Does it actually compute
// ============================================================================
static bool test_execution(void) {
    wipe();
    load(0x0000, prog_checksum, sizeof prog_checksum);
    z80_mem[PROG_CHECKSUM_RESULT]     = 0;
    z80_mem[PROG_CHECKSUM_RESULT + 1] = 0;

    z80_reset();
    z80_edge_watch_reset();     // after the reset: the bus floats during one
    z80_run(PROG_CHECKSUM_TSTATES);

    const uint16_t got = (uint16_t)z80_mem[PROG_CHECKSUM_RESULT] |
                         ((uint16_t)z80_mem[PROG_CHECKSUM_RESULT + 1] << 8);

    if (got != PROG_CHECKSUM_EXPECTED) {
        scr_fail("Execution (sum 0..255)", "got %04X, expected %04X",
                 got, PROG_CHECKSUM_EXPECTED);
        return false;
    }

    scr_pass("Execution (sum 0..255)", "%04X in %lu fetches", got,
             (unsigned long)z80_stats.opfetch);

    /*
     * The loop runs out of 0000-0016h and stores to 4000h, and the
     * refresh address counts through A0-A6 on every M1 — so A0-A6 and
     * A14 are certain to have moved, and A7-A13 and A15 are certain to
     * have been held low the whole time. That is not a fault, so this is
     * reported rather than failed: the number to compare it against is
     * the one the address-bus test already proved, and the value here is
     * in catching a line that changed behaviour between the two.
     */
    const uint32_t stuck_hi = z80_edge_watch_stuck_high();
    const uint32_t stuck_lo = z80_edge_watch_stuck_low();
    if (stuck_hi)
        scr_log(ATTR(C_YELLOW, C_BLACK), "  note: address lines held high: %04lX",
                (unsigned long)stuck_hi);
    scr_log(ATTR(C_DGREY, C_BLACK), "  address lines exercised: %04lX",
            (unsigned long)(0xFFFFu & ~stuck_lo));
    return true;
}

// ============================================================================
// 7. ~NMI and ~INT
// ============================================================================
static bool test_nmi(void) {
    wipe();
    const uint8_t spin[] = PROG_NMI_SPIN;
    const uint8_t handler[] = PROG_NMI_HANDLER;
    load(0x0000, spin, sizeof spin);
    load(PROG_NMI_VECTOR, handler, sizeof handler);
    z80_mem[PROG_NMI_RESULT] = 0x00;

    z80_reset();
    z80_run(64);                    // let it reach the spin
    z80_pulse_nmi();
    z80_run(256);

    if (z80_mem[PROG_NMI_RESULT] != PROG_NMI_MARK) {
        scr_fail("NMI (GP39)", "handler at 0066 did not run; last M1 %04X",
                 z80_stats.last_opfetch_addr);
        return false;
    }
    scr_pass("NMI (GP39)", "vectored to 0066");
    return true;
}

static bool test_int(void) {
    wipe();
    const uint8_t spin[] = PROG_INT_SPIN;
    const uint8_t handler[] = PROG_INT_HANDLER;
    load(0x0000, spin, sizeof spin);
    load(PROG_INT_VECTOR, handler, sizeof handler);
    z80_mem[PROG_INT_RESULT] = 0x00;

    z80_int_vector = 0xFF;          // RST 38h, which IM 1 ignores anyway

    z80_reset();
    z80_run(128);                   // IM 1, EI, and into the spin

    z80_set_int(true);
    // Long enough for the current instruction to finish, the acknowledge
    // cycle to run and the handler to reach its store.
    z80_run(512);
    z80_set_int(false);
    z80_run(64);

    if (z80_stats.int_ack == 0) {
        scr_fail("INT in IM 1 (GP38)", "no acknowledge cycle seen");
        return false;
    }
    if (z80_mem[PROG_INT_RESULT] != PROG_INT_MARK) {
        scr_fail("INT in IM 1 (GP38)", "acknowledged but handler at 0038 did not run");
        return false;
    }
    scr_pass("INT in IM 1 (GP38)", "%lu acknowledge cycles, vectored to 0038",
             (unsigned long)z80_stats.int_ack);
    return true;
}

// ============================================================================
// 8. How fast will it go
// ============================================================================
//
// Not a benchmark. The question stage 1 needs answered is whether this
// bus engine can hold a ZX Spectrum's 3.5 MHz, and the only honest way to
// answer it is to run a program whose answer is known at each pace and
// find where the answer stops being right.
//
// Wrong answers here are not the CPU's fault. Below some settle time the
// engine samples the Z80's pins before they are valid and reads the
// previous cycle's address, which is a firmware limit rather than a
// silicon one — which is why the report gives the pace as well as the
// megahertz.
static void test_speed(void) {
    static const uint32_t paces[] = { 40, 32, 24, 18, 14, 10, 8, 6, 4, 3, 2, 1, 0 };
    const uint32_t saved = z80_get_pace();

    uint32_t best_hz = 0, best_pace = saved;

    // Thirteen entries of "4.40 " plus the label is more than eighty
    // characters, and once `at` passes the end `sizeof ramp - at`
    // underflows into a huge size_t and snprintf writes off the end of
    // the stack. Sized for the worst case, and clamped anyway.
    char ramp[160];
    int at = snprintf(ramp, sizeof ramp, "  MHz: ");

    for (unsigned i = 0; i < count_of(paces); i++) {
        z80_set_pace(paces[i]);

        wipe();
        load(0x0000, prog_checksum, sizeof prog_checksum);
        z80_mem[PROG_CHECKSUM_RESULT] = 0;
        z80_mem[PROG_CHECKSUM_RESULT + 1] = 0;

        // 20000 T-states of real work, which is comfortably more than the
        // ~11,500 the program needs — so the measurement is taken over the
        // program running and the answer is already stored when it ends.
        // Timing an idle spin would measure a different instruction mix
        // from the one stage 1 will actually run.
        z80_reset();
        const uint32_t hz = z80_measure_clock();

        const uint16_t got = (uint16_t)z80_mem[PROG_CHECKSUM_RESULT] |
                             ((uint16_t)z80_mem[PROG_CHECKSUM_RESULT + 1] << 8);
        const bool ok = (got == PROG_CHECKSUM_EXPECTED);

        /*
         * The ramp on one line, not thirteen.
         *
         * Thirteen lines of "pace N, M Hz, ok" pushed the header and the
         * first four verdicts off a thirty-row screen, which defeats the
         * point of having a screen: one photograph should answer whether
         * the board passed. The individual numbers still reach the
         * console, where there is no such limit.
         */
        if (at < 0 || (size_t)at >= sizeof ramp) at = (int)sizeof ramp - 1;
        at += snprintf(ramp + at, sizeof ramp - (size_t)at,
                       "%lu.%02lu%s ",
                       (unsigned long)(hz / 1000000), (unsigned long)((hz / 10000) % 100),
                       ok ? "" : "!");
        printf("  pace %2lu  %7lu Hz  %s\n",
               (unsigned long)paces[i], (unsigned long)hz, ok ? "ok" : "WRONG ANSWER");

        if (ok && hz > best_hz) { best_hz = hz; best_pace = paces[i]; }
        if (!ok) break;         // past the edge; going faster proves nothing
    }

    scr_log(ATTR(C_GREY, C_BLACK), "%s", ramp);

    res.z80_clock_hz = best_hz;
    res.best_pace    = best_pace;

    // Back to something with margin rather than to the edge. Half the
    // fastest pace that worked is a 2x guard band on a measurement taken
    // at one temperature, on one CPU, with the console quiet.
    z80_set_pace(best_pace + (best_pace >> 1) + 2);

    if (best_hz >= 3500000)
        scr_pass("Speed", "%lu.%02lu MHz clean - a Spectrum's 3.5 MHz fits",
                 (unsigned long)(best_hz / 1000000), (unsigned long)((best_hz / 10000) % 100));
    else
        scr_log(ATTR(C_YELLOW, C_BLACK), "Speed                        %lu.%02lu MHz clean - under 3.5 MHz",
                (unsigned long)(best_hz / 1000000), (unsigned long)((best_hz / 10000) % 100));

    res.run++;
    res.passed++;
}

// ============================================================================
// The trace
// ============================================================================
static const char *kind_name(const bus_cycle_t k) {
    switch (k) {
        case BUS_OPFETCH:   return "M1  ";
        case BUS_MEM_READ:  return "MR  ";
        case BUS_MEM_WRITE: return "MW  ";
        case BUS_IO_READ:   return "IOR ";
        case BUS_IO_WRITE:  return "IOW ";
        case BUS_INT_ACK:   return "INTA";
        default:            return "??  ";
    }
}

void selftest_dump_trace(const uint32_t max_lines) {
    const uint32_t n = z80_trace_count();
    const uint32_t start = n > max_lines ? n - max_lines : 0;
    printf("--- bus trace, last %lu of %lu cycles ---\n",
           (unsigned long)(n - start), (unsigned long)n);
    for (uint32_t i = start; i < n; i++) {
        const bus_event_t e = z80_trace_get(i);
        printf("  %s %04X %02X\n", kind_name(e.kind), e.addr, e.data);
    }
}

bool selftest_quick_check(void) {
    if (!test_presence()) return false;

    /*
     * The wiring test runs, but it does not get a vote.
     *
     * It is a diagnostic, not a gate: it measures the bus with the CPU
     * held in reset and its outputs high-impedance, which is a state the
     * machine is never in while it is working. It has twice reported a
     * data line held high on a board whose data bus then round-tripped
     * all twenty test values without a fault — so on this hardware it
     * produces false positives, and refusing to boot a Spectrum on the
     * strength of one would be believing the weaker measurement.
     *
     * What it is good for is naming a line when something really is
     * wrong, which is worth printing either way. The tests that decide
     * whether to go on are the ones that ask the CPU to do something.
     */
    test_wiring();

    if (!test_reset()) return false;
    return true;
}

// ============================================================================
// The run
// ============================================================================
selftest_result_t selftest_run_all(void) {
    memset(&res, 0, sizeof res);

    scr_log(ATTR(C_WHITE, C_BLACK), "Stage 0 - proving the Z80 and the bus");
    scr_blank();

    // Presence is the one test worth stopping on. Everything below it
    // reads a floating bus and reports nonsense with great confidence.
    const bool present = test_presence();
    record(present);
    if (!present) {
        scr_blank();
        scr_log(ATTR(C_YELLOW, C_BLACK), "Check: CPU in the socket the right way round, 5V on U3 pin 11.");
        return res;
    }

    record(test_wiring());
    record(test_reset());

    scr_blank();
    diag_first_cycles();
    scr_blank();

    record(test_address_bus());
    record(test_data_bus());
    record(test_io());
    record(test_execution());
    record(test_nmi());
    record(test_int());

    scr_blank();
    test_speed();

    return res;
}
