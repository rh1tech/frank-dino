/*
 * frank-dino — an RP2350B acting as the whole machine around a real Z80
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-dino
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Board facts: what is wired where, and what is not wired at all. Nothing
 * here does anything — it is the one file to open when the question is
 * "which pin is that".
 *
 * The numbers are read off dino_z80.kicad_pcb, not inherited from
 * frank-xt8086. boards/frank_dino.h is the SDK-facing half of the same
 * map and docs/pinout.md is the prose one; if the three ever disagree,
 * the PCB wins.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <hardware/clocks.h>
#include <hardware/structs/sio.h>

#define likely(x)   __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)

// ============================================================================
// Clocks
// ============================================================================
//
// CPU_SPEED_MHZ comes from CMake. Unlike xt8086, nothing here *needs* a
// particular system clock to be correct: this firmware generates the Z80's
// own clock edge by edge, so a slower RP2350 makes a slower Z80 rather
// than a broken one. It still sets the ceiling on how fast the Z80 can be
// driven, which is what the speed ramp in src/test measures.
#ifndef CPU_SPEED_MHZ
#define CPU_SPEED_MHZ 378
#endif
#define PICO_CLOCK_SPEED (CPU_SPEED_MHZ * MHZ)

// ============================================================================
// GPIO pin map
// ============================================================================
//
// Everything the Z80 touches is inside GP0-GP31. That is not a
// coincidence worth ignoring: it means one 32-bit read of SIO's GPIO_IN
// captures the entire bus — sixteen address lines, eight data lines and
// all four control lines — in a single instruction, and the data bus can
// be driven and released through the 32-bit SET/CLR/OE registers. The bus
// engine in z80_bus.c is built on exactly that.

#define Z80_ADDR_BASE   0       // A0-A15  -> GP0-GP15,  Z80 pins 30-40, 1-5
#define Z80_ADDR_BITS   16
#define Z80_ADDR_MASK   0x0000FFFFu

#define Z80_DATA_BASE   16      // D0-D7   -> GP16-GP23, Z80 pins 14,15,12,8,7,9,10,13
#define Z80_DATA_BITS   8
#define Z80_DATA_MASK   0x00FF0000u

#define Z80_MREQ_PIN    24      // ~MREQ, Z80 pin 19, in
#define Z80_RD_PIN      25      // ~RD,   Z80 pin 21, in
#define Z80_WR_PIN      26      // ~WR,   Z80 pin 22, in
#define Z80_M1_PIN      27      // ~M1,   Z80 pin 27, in, through R9 (1K)
#define Z80_CLK_PIN     28      // CLK,   Z80 pin 6,  out, through U1 74HCT125
#define Z80_RESET_PIN   29      // ~RESET, Z80 pin 26, out, R8 10K pull-DOWN
#define Z80_INT_PIN     38      // ~INT,  Z80 pin 16, out, R6 10K pull-up
#define Z80_NMI_PIN     39      // ~NMI,  Z80 pin 17, out, R7 10K pull-up

#define Z80_MREQ (1u << Z80_MREQ_PIN)
#define Z80_RD   (1u << Z80_RD_PIN)
#define Z80_WR   (1u << Z80_WR_PIN)
#define Z80_M1   (1u << Z80_M1_PIN)
#define Z80_CLK  (1u << Z80_CLK_PIN)
#define Z80_RST  (1u << Z80_RESET_PIN)

/*
 * What the Z80 has that this board does not connect.
 *
 * ~IORQ (pin 20), ~RFSH (28), ~HALT (18) and ~BUSACK (23) go nowhere;
 * ~WAIT (24) and ~BUSRQ (25) are strapped to VCC. Two of those shape the
 * whole firmware and are worth stating rather than discovering:
 *
 * ~IORQ absent means an I/O cycle cannot be seen directly. It does not
 * have to be: the Z80 holds ~MREQ high for the whole of one, so "~RD or
 * ~WR active while ~MREQ is idle" is an I/O cycle and nothing else is.
 * Interrupt acknowledge is the same trick one step further — ~M1 active
 * with ~MREQ never following it. See z80_bus.c, which is where the
 * inference actually lives.
 *
 * ~WAIT strapped high means the CPU cannot be stalled, so every bus cycle
 * has to be answered inside the clock we give it. That would be a hard
 * timing constraint on a CPU with its own oscillator. Here the clock is
 * ours, edge by edge, and a Z84C00 is fully static — so the constraint
 * turns into "do not clock it faster than the answer takes", which is a
 * throughput question rather than a correctness one.
 */
#define Z80_HAS_IORQ    0
#define Z80_HAS_WAIT    0
#define Z80_HAS_RFSH    0
#define Z80_HAS_HALT    0

// --- Video: a six-bit R2R ladder plus separate sync, GP30-GP37 ---
//
// The byte shifted out is VHRRGGBB — blue in the low pair, which is what
// R48/R49 (800R/400R into the blue node) make it. Separate H and V, so
// there is nothing to fold onto green. The ladder drives U13, an MS9288A
// VGA-to-HDMI converter, which drives J10; there is no VGA connector on
// this board, so HDMI is the only output and the capture stick sees it.
#define VGA_BASE_PIN     30

// --- microSD (J3) on SPI1 — GP40-43 have no SPI0 mapping at all ---
#define SDCARD_MISO_PIN  40
#define SDCARD_CS_PIN    41
#define SDCARD_SCK_PIN   42
#define SDCARD_MOSI_PIN  43

// --- Console UART (J1: pin 1 TX, pin 2 GND, pin 3 RX) ---
#define UART_TX_PIN      44
#define UART_RX_PIN      45

/*
 * The tape input (J9) and the console's RX are the same net.
 *
 * The schematic calls it TAPE_UART_RX: the comparator on the tape
 * sheet drives the pin the UART receives on, so GP45 is one or the
 * other and never both. Loading from a real tape costs the console's
 * receive side for as long as it is switched on.
 */
#define TAPEIN_PIN       UART_RX_PIN

#define SPEAKER_PIN      46     // through S2, to the jack / buzzer / HDMI audio
#define PSRAM_CS_PIN     47     // ESP-PSRAM64H (U7) on XIP CS1, 10k pull-up

// --- Debug (J5: pin 1 SWDIO, pin 2 GND, pin 3 SWCLK) ---
// Not a GPIO; the RP2350's dedicated SWD pads. Noted here because J1 and
// J5 are both three-pin headers a finger's width apart, and a probe on
// the wrong one is silent in exactly the way a dead board is.

// ============================================================================
// Guest memory
// ============================================================================
//
// The Z80 addresses 64 KB and that is the whole of it, so the guest's
// memory is one flat array in on-chip SRAM. The RP2350B has 520 KB; this
// takes an eighth of it and buys a read path with no cache, no bank
// register and no PSRAM latency on the critical path. The PSRAM (U7, 8 MB)
// is untouched in stage 0 and is there for when something needs more than
// the Z80 can address — snapshots, disk images, a second machine.
/*
 * Only the stage-0 tests need a real flat 64 KB.
 *
 * A machine brings its own memory and aims the windows at it, so in a
 * Spectrum build this array exists solely for the clock calibration,
 * which runs the CPU through NOPs and never cares what it reads twice.
 * Sixteen kilobytes shared by all four windows does that just as well and
 * gives back 48 KB — which matters, because 128 KB of guest RAM, four
 * ROMs, a trampoline page and the video buffers had RAM at 88 %.
 */
#if SELFTEST
#define GUEST_MEM_SIZE   (64 * 1024)
#else
#define GUEST_MEM_SIZE   (16 * 1024)
#endif
