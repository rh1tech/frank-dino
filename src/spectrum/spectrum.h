/*
 * frank-dino — an RP2350B acting as the whole machine around a real Z80
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-dino
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * A ZX Spectrum 128K, for a Z80 that is not emulated.
 *
 * What is here is only the half of a Spectrum that is not the CPU: the
 * memory map and its paging port, the ULA's two jobs (the screen and
 * port 0xFE), the 50 Hz interrupt, and the keyboard matrix. The other
 * half is a Z84C00 in a socket, and it needs no help.
 *
 * The rules — which bits of which port do what, how the screen is laid
 * out, how many T-states a frame is — are the ones pico-speccy and
 * ESPectrum established; see docs/spectrum.md. The code is not theirs,
 * because theirs is built around ticking an emulated Z80 through twenty
 * different machines and this is built around answering a real one.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

// ============================================================================
// Which machine
// ============================================================================
//
// Three, and the differences are small but none of them are cosmetic.
//
// A 48K has one ROM, no paging port and a frame of 69888 T-states; its
// AY does not exist, so writes to 0xFFFD must do nothing rather than
// quietly work. A 128K has two ROMs, the 0x7FFD latch, 70908 T-states
// and the AY. A +2 is a 128K with Amstrad's ROM 0 — same hardware,
// different menu, and enough software checks for it that it is worth
// being able to select.
//
// A Pentagon 128 is the Russian clone, and it is not a cosmetic
// difference: 71680 T-states to the frame, no ULA contention at all, and
// a Beta Disk interface as standard rather than as an add-on. Half the
// demo scene targets its timing specifically, and a good deal of what is
// on a .trd expects to find TR-DOS on it.
typedef enum : uint8_t {
    MACHINE_48K = 0,
    MACHINE_128K,
    MACHINE_PLUS2,
    MACHINE_PENTAGON,
    MACHINE_COUNT,
} machine_t;

extern machine_t spec_machine;

const char *spec_machine_name(void);

/* Select a machine and cold-start it. Pauses nothing; caller owns that. */
void spec_set_machine(machine_t m);
void spec_next_machine(void);

/* T-states per frame for the current machine. */
uint32_t spec_frame_tstates(void);

// ============================================================================
// Timing
// ============================================================================
//
// A 128K frame is 70908 T-states, not the 48K's 69888 — the 128's ULA
// runs 311 lines to the 48's 312 and the line is a T-state longer. Getting
// this wrong does not break anything visibly; it makes the machine run at
// the wrong speed and every piece of music play flat.
#define SPEC_TSTATES_PER_FRAME   70908
#define SPEC_TSTATES_PER_FRAME_48 69888
#define SPEC_TSTATES_PER_FRAME_PENT 71680

// How long the ULA holds ~INT low. 36 T-states on a 128K; 32 on a 48K.
// Long enough that the CPU cannot miss it between two instructions.
#define SPEC_INT_TSTATES 36

// 70908 T-states at 50.01 Hz.
#define SPEC_CPU_HZ 3546900

// ============================================================================
// Memory
// ============================================================================
//
// Eight 16 KB RAM banks and two 16 KB ROMs. The window layout never
// changes; only which bank is in the top one, and which ROM is in the
// bottom one:
//
//   0000-3FFF   ROM 0 (editor) or ROM 1 (48K BASIC)
//   4000-7FFF   RAM bank 5, always — and the screen normally lives here
//   8000-BFFF   RAM bank 2, always
//   C000-FFFF   RAM bank 0-7, whichever port 0x7FFD names
#define SPEC_RAM_BANKS 8
#define SPEC_BANK_SIZE 0x4000

extern uint8_t spec_ram[SPEC_RAM_BANKS][SPEC_BANK_SIZE];

// ============================================================================
// State the video driver reads
// ============================================================================

/* The bank the ULA displays: 5 normally, 7 when 0x7FFD bit 3 is set. */
extern volatile uint8_t spec_screen_bank;

/* Border colour, 0-7. Written by OUT (0xFE),A while a frame is drawing. */
extern volatile uint8_t spec_border;

/* Flips every 16 frames; inverts any cell whose attribute has bit 7. */
extern volatile bool spec_flash;

/* Frames since reset, and the paging port's last value, for the report. */
extern volatile uint32_t spec_frames;
extern volatile uint8_t  spec_port_7ffd;

// ============================================================================
// Sound
// ============================================================================
//
// A 128K has an AY-3-8912 on ports 0xFFFD and 0xBFFD. TurboSound is two
// of them behind the same pair: a write of 0xFF or 0xFE to the select
// port chooses which chip the following register accesses reach, instead
// of naming a register.
//
// Whether the second chip exists is a runtime thing rather than a build
// one, because a program that does not know about TurboSound writes 0xFF
// to the select port as a register number now and then, and on a machine
// with only one chip that must stay harmless. It lives in settings.h
// with everything else that is a preference.
extern volatile uint32_t spec_ay_writes;

/*
 * Put a known 441 Hz sine straight into the mixer.
 *
 * For measuring the sound path instead of listening to it. Not played
 * through the AY, deliberately: the 128K ROM rewrites the chip's
 * registers fifty times a second, so a tone programmed into it from
 * outside lasts one frame. And a sine rather than a square, because a
 * square wave's harmonics fold back from past Nyquist and make every
 * spectrum dense -- with a sine, 44100 divides by 441 exactly, so there
 * are a hundred samples to the cycle and every bin but one is evidence.
 */
void spec_test_tone(bool on);
bool spec_test_tone_is_on(void);

/* True while the beeper bit is set. The mixer samples it; nothing drives
 * the pin directly any more — see src/drivers/audio/audio.h. */
extern volatile bool spec_beeper;

// ============================================================================
// Running it
// ============================================================================

/*
 * Load the ROMs, clear RAM, map the windows and reset the CPU.
 *
 * Does not return the machine running — call spec_run_frame() for that.
 */
void spec_init(void);

/*
 * Clock the Z80 for one frame and deliver the 50 Hz interrupt.
 *
 * Runs on core 1, and is the whole of the Spectrum's main loop: there is
 * nothing to emulate between interrupts because the CPU does it.
 */
void spec_run_frame(void);

/*
 * Trim the Z80's clock until a frame really does take 20 ms.
 *
 * z80_calibrate() measures the bus engine on its own, which is the right
 * answer to a different question. The Spectrum's loop also renders two AY
 * chips and pushes a sample 882 times a frame, and that work is not free:
 * calibrated at 3.563 MHz, the machine actually ran at 3.109 — twelve per
 * cent slow, the audio starved, and every note was a tone and a half flat.
 *
 * So this measures the loop that actually runs, with the machine running
 * it, and adjusts the pace until the frame rate is 50 Hz. Returns the
 * achieved rate in millihertz-per-frame... in frames per second, x1000.
 */
uint32_t spec_tune_clock(void);

/*
 * How many samples a frame is currently cut into.
 *
 * Not the constant 882: that is only right on a machine running at
 * exactly 3.5469 MHz, and this one runs at whatever the tuner could
 * reach. The count follows the measured frame rate so the DAC is always
 * handed a full frame's worth. See the definition.
 */
uint32_t spec_samples_per_frame(void);

// ============================================================================
// Stopping it
// ============================================================================
//
// Core 0 owns the card, the screen and the menu; core 1 owns the Z80. The
// two never touch the same thing at the same time because core 1 parks
// between frames whenever core 0 asks — which is also the only moment
// FatFs may be entered, and the only moment guest RAM may be rewritten
// underneath the CPU.
//
// Parking between frames rather than mid-frame matters: the machine
// experiences the pause as one very long gap between interrupts, which
// is something a Spectrum's ROM copes with, rather than as a bus cycle
// that never completed, which is not.
void spec_pause(void);
void spec_resume(void);
bool spec_is_paused(void);

// ============================================================================
// Loading something into the machine
// ============================================================================

/* Set the paging latch from this side, as though the CPU had written it. */
void spec_set_paging(uint8_t value);

/*
 * Map `page` over the ROM window and arm the swap back.
 *
 * The real ROM returns on the first opcode fetch after the one at
 * jp_addr, which is the instruction that leaves the page. See
 * src/spectrum/launch.c for why it has to be that exact moment.
 */
void spec_begin_launch(uint8_t *page, uint16_t jp_addr);

/* True while a launch is still waiting for the CPU to leave the page. */
bool spec_launch_pending(void);

// ============================================================================
// Tape
// ============================================================================
//
// The 48K BASIC ROM is where LD-BYTES lives, and it is a different image
// on a 48K (its only ROM) than on a 128K or +2 (ROM 1). src/spectrum/tape.c
// takes a copy of whichever it is, patches a stub into it and hands it
// back here; NULL puts the real one back.
//
// The substitution happens inside the paging function rather than by
// mapping the page directly, so that it survives the CPU paging the ROM
// out and in again -- which a 128K game does every time it loads.
const uint8_t *spec_rom_48k_image(void);
void spec_set_tape_rom(const uint8_t *page);

// ============================================================================
// Keyboard
// ============================================================================
//
// Eight half-rows of five keys, read through port 0xFE with the row
// selected by whichever of A8-A15 the CPU pulls low. A pressed key reads
// as 0, which is why the matrix is held as "not pressed" bits and handed
// out inverted.
//
// There is no keyboard socket on this board yet, so the console is the
// keyboard: spec_key_ascii() takes a character off the UART or USB and
// holds the matching keys down for a few frames. That is enough to reach
// the 128K menu and type a BASIC line, and it is testable from a script,
// which a PS/2 socket would not be.
void spec_key_ascii(char c);

/* Press or release a matrix position directly. row 0-7, bit 0-4. */
void spec_key_set(uint8_t row, uint8_t bit, bool pressed);

/*
 * The joystick, whichever one the settings say it is.
 *
 * One set of five bits; how they reach the guest is the interface's
 * business. `bit` is a JOY_BIT_* from settings.h.
 */
void    spec_joy_set(uint8_t bit, bool pressed);
uint8_t spec_joy_state(void);

/*
 * Type a string, one character every few frames.
 *
 * `s` must outlive the typing -- a string literal or a static buffer --
 * because nothing here copies it. `delay_frames` is how long to wait
 * before the first character, which is what lets a caller cold-start the
 * machine and then type into the prompt the ROM has not put up yet.
 */
void spec_type(const char *s, unsigned delay_frames);
void spec_type_cancel(void);

/* Called once per frame to time out keys pressed by spec_key_ascii(). */
void spec_key_tick(void);
