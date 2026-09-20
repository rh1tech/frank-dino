/*
 * frank-dino — an RP2350B acting as the whole machine around a real Z80
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-dino
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * The Z80 programs the stage-0 tests run, hand-assembled.
 *
 * Hand-assembled rather than built with an assembler, and that is a
 * choice rather than laziness: these are the bytes that prove the data
 * bus works, and a build step between the listing and the binary is one
 * more thing that can be the reason a test fails. Each array is short
 * enough to check against the listing beside it by eye, which is the
 * property that matters when the question is whether the *hardware* is
 * wrong.
 *
 * Every program ends in a two-byte spin (JP to itself) rather than HALT.
 * ~HALT is not connected on this board, so a halted CPU and a wedged one
 * look identical from here; a spin keeps fetching from an address the
 * trace can show.
 */
#pragma once

#include <stdint.h>

// ---------------------------------------------------------------------------
// JP nn — the address-bus probe.
//
//   0000  C3 lo hi   JP target
//
// One jump and nothing else. The fetch that follows is at `target`, so
// each of the sixteen address lines can be driven high and low on its own
// by choosing the target, with no chained state to go wrong in between.
// ---------------------------------------------------------------------------
#define PROG_JP(lo, hi) { 0xC3, (lo), (hi) }

// ---------------------------------------------------------------------------
// The data-bus probe.
//
//   0000  3E nn      LD A,nn
//   0002  32 00 40   LD (4000h),A
//   0005  C3 05 00   JP 0005h
//
// nn goes out over D0-D7 as an operand and comes back over D0-D7 as
// written data, so a line stuck either way fails in the comparison rather
// than silently. 4000h is chosen so the write also toggles A14 on its own.
// ---------------------------------------------------------------------------
#define PROG_DATA_WALK(nn) \
    { 0x3E, (nn), 0x32, 0x00, 0x40, 0xC3, 0x05, 0x00 }
#define PROG_DATA_WALK_RESULT 0x4000
#define PROG_DATA_WALK_TSTATES 64

// ---------------------------------------------------------------------------
// The I/O probe — the one that proves ~IORQ is not needed.
//
//   0000  3E nn      LD A,nn
//   0002  D3 pp      OUT (pp),A
//   0004  DB pp      IN A,(pp)
//   0006  32 00 40   LD (4000h),A
//   0009  C3 09 00   JP 0009h
//
// OUT and IN hold ~MREQ high for the whole cycle and assert an ~IORQ this
// board does not connect. If the bus engine's inference is right, the OUT
// arrives at the I/O handler and the IN's answer lands at 4000h. If it is
// wrong, the OUT looks like a write to memory at pp — which is a distinct
// enough failure to name in the report.
//
// Note the address the Z80 actually puts out: for OUT (n),A and IN A,(n)
// the low half is n and the *high* half is A. That is not a quirk to work
// around; a ZX Spectrum's keyboard is read exactly that way.
// ---------------------------------------------------------------------------
#define PROG_IO(nn, pp) \
    { 0x3E, (nn), 0xD3, (pp), 0xDB, (pp), 0x32, 0x00, 0x40, 0xC3, 0x09, 0x00 }
#define PROG_IO_RESULT 0x4000
#define PROG_IO_TSTATES 128

// ---------------------------------------------------------------------------
// Execution: does it compute?
//
//   0000  21 00 00   LD HL,0000h
//   0003  06 00      LD B,00h        ; DJNZ with B=0 runs 256 times
//   0005  0E 00      LD C,00h
//   0007  79         LD A,C          ; loop:
//   0008  85         ADD A,L
//   0009  6F         LD L,A
//   000A  3E 00      LD A,00h
//   000C  8C         ADC A,H         ; carry out of the low byte
//   000D  67         LD H,A
//   000E  0C         INC C
//   000F  10 F6      DJNZ loop       ; 0007h - 0011h = -10
//   0011  22 00 40   LD (4000h),HL
//   0014  C3 14 00   JP 0014h
//
// Sums 0..255 into HL, which is 32640 = 7F80h, and stores it little-endian.
// The point is not the arithmetic: it is that the answer depends on the
// carry flag surviving four instructions, on a conditional branch taken
// 255 times and not taken once, and on a 16-bit store. A CPU with one bad
// data line gets a wrong number; a CPU with a bad flag gets a wrong number
// too, and 7F80h is specific enough that neither can pass by accident.
// ---------------------------------------------------------------------------
static const uint8_t prog_checksum[] = {
    0x21, 0x00, 0x00,
    0x06, 0x00,
    0x0E, 0x00,
    0x79,
    0x85,
    0x6F,
    0x3E, 0x00,
    0x8C,
    0x67,
    0x0C,
    0x10, 0xF6,
    0x22, 0x00, 0x40,
    0xC3, 0x14, 0x00,
};
#define PROG_CHECKSUM_RESULT   0x4000
#define PROG_CHECKSUM_EXPECTED 0x7F80
#define PROG_CHECKSUM_TSTATES  16000

// ---------------------------------------------------------------------------
// ~NMI. The handler is at 0066h and is not reachable any other way, so
// finding 5Ah at 4002h means the pin moved the CPU and nothing else did.
//
//   0000  C3 00 00   JP 0000h        ; spin
//   0066  3E 5A      LD A,5Ah
//   0068  32 02 40   LD (4002h),A
//   006B  C3 6B 00   JP 006Bh
// ---------------------------------------------------------------------------
#define PROG_NMI_SPIN   { 0xC3, 0x00, 0x00 }
#define PROG_NMI_HANDLER { 0x3E, 0x5A, 0x32, 0x02, 0x40, 0xC3, 0x6B, 0x00 }
#define PROG_NMI_VECTOR 0x0066
#define PROG_NMI_RESULT 0x4002
#define PROG_NMI_MARK   0x5A

// ---------------------------------------------------------------------------
// ~INT in interrupt mode 1, which is the mode a ZX Spectrum uses.
//
//   0000  ED 56      IM 1
//   0002  FB         EI
//   0003  C3 03 00   JP 0003h        ; spin with interrupts enabled
//   0038  3E A5      LD A,A5h
//   003A  32 03 40   LD (4003h),A
//   003D  F3         DI
//   003E  C3 3E 00   JP 003Eh
//
// EI does not take effect until after the instruction following it, which
// is why the spin is a separate instruction rather than a jump back to
// the EI. The handler disables interrupts before spinning so a line still
// held low cannot make it fire twice and hide a stuck pin.
// ---------------------------------------------------------------------------
#define PROG_INT_SPIN    { 0xED, 0x56, 0xFB, 0xC3, 0x03, 0x00 }
#define PROG_INT_HANDLER { 0x3E, 0xA5, 0x32, 0x03, 0x40, 0xF3, 0xC3, 0x3E, 0x00 }
#define PROG_INT_VECTOR  0x0038
#define PROG_INT_RESULT  0x4003
#define PROG_INT_MARK    0xA5
