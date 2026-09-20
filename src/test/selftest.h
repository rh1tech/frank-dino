/*
 * frank-dino — an RP2350B acting as the whole machine around a real Z80
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-dino
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Stage 0: does the Z80 work, and does this board's idea of a bus match
 * the Z80's?
 *
 * Every test here runs on the physical CPU. Nothing is emulated and
 * nothing is simulated; the pass conditions are bytes a real Z84C00 wrote
 * back across real wires. They are ordered so that the first one to fail
 * is the most informative: presence, then reset, then the address bus,
 * then the data bus, then I/O, then execution, then the interrupt pins,
 * and only then speed.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint32_t run;
    uint32_t passed;
    uint32_t failed;
    uint32_t z80_clock_hz;      // best clock that still computed correctly
    uint32_t best_pace;
} selftest_result_t;

selftest_result_t selftest_run_all(void);

/* Dump the bus trace ring to the console. Called after a failure. */
void selftest_dump_trace(uint32_t max_lines);
