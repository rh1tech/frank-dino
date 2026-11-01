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

/*
 * A binary file, as a symbol in .rodata. Overridable so the host tests can
 * supply a Mach-O form of the same thing and link the real ROM images.
 */
#ifndef IMPORT_BIN
#define IMPORT_BIN(file, sym) asm (\
    ".section .rodata." #sym "\n" \
    ".balign 4\n"                 \
    ".global " #sym "\n"          \
    #sym ":\n"                    \
    ".incbin \"" file "\"\n"      \
    ".balign 4\n"                 \
    ".section \".text\"\n")
#endif

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
    MACHINE_PLUS3,
    MACHINE_PENTAGON,
    MACHINE_SCORPION,       /* ZS-256: 256 KB, a service ROM and TR-DOS */
    MACHINE_PROFI,          /* Profi 1024: 1 MB through #DFFD */
    MACHINE_TIMEX,          /* TC2068: the SCLD, a dock and an EX-ROM */
    /*
     * The rest, after the original eight so that a settings file written
     * before they existed still names the machine it meant. Ported from
     * pico-speccy's machine set; see roms/README.md for the images.
     */
    MACHINE_16K,            /* the first Spectrum: 16 KB, nothing above #8000 */
    MACHINE_PLUS2A,         /* a +3 without the drive */
    MACHINE_TC2048,         /* Timex's 48K: the SCLD's video, no dock */
    MACHINE_GAMA,           /* Didaktik Gama 89, a Czechoslovak 48K */
    MACHINE_LENINGRAD1,     /* the Leningrad clones: 48K, own ~INT, no contention */
    MACHINE_LENINGRAD2,
    MACHINE_BYTE,           /* Byte, from Bryansk: a 48K with a timer for sound */
    MACHINE_PENT512,        /* Pentagon 512: #7FFD bits 6 and 7 page 512 KB */
    MACHINE_PENT1024,       /* Pentagon 1024SL: bit 5 too, #EFF7, a cache page, 16 colours */
    MACHINE_ZS1024,         /* Scorpion ZS-1024 with ProfROM 4.44: 1 MB, four ROM planes */
    MACHINE_SCORPEVO,       /* the same on the Pentagon's raster, as the Evo runs it */
    MACHINE_KAY256,         /* Nemo KAY-256: a Scorpion with its own #1FFD */
    MACHINE_KAY1024,        /* Nemo KAY-1024 */
    MACHINE_KAY1024B,       /* KAY-1024 with the 2010 ROM */
    MACHINE_PHOENIX,        /* ZXM-Phoenix: KAY paging, 2 MB of decode, 1 MB fitted here */
    MACHINE_ATM1,           /* MicroART ATM-Turbo 1, BIOS 1.04 */
    MACHINE_ATM2,           /* ATM-Turbo 2, BIOS 1.06.02 */
    MACHINE_ATM2P,          /* ATM-Turbo 2+, BIOS 1.07.13 */
    MACHINE_ATM2X,          /* ATM-Turbo 2+, xBIOS 1.37 */
    MACHINE_ATM3,           /* NedoPC ATM-Turbo 3: 4 MB, test ROM + xBIOS */
    MACHINE_ALF,            /* ALF TV Game: a 128K with banked cartridges */
    MACHINE_COUNT,
} machine_t;

/*
 * The families, for the checks that mean "a machine like that" rather
 * than one machine. A 48K-class machine has one ROM, 48 KB and no #7FFD.
 */
static inline bool spec_is_48k_class(const machine_t m) {
    return m == MACHINE_48K || m == MACHINE_16K || m == MACHINE_TC2048 || m == MACHINE_GAMA
        || m == MACHINE_LENINGRAD1 || m == MACHINE_LENINGRAD2 || m == MACHINE_BYTE;
}

/* The +2A/+3 gate array: four ROMs, #1FFD, the special all-RAM modes. */
static inline bool spec_is_plus3_class(const machine_t m) {
    return m == MACHINE_PLUS3 || m == MACHINE_PLUS2A;
}

/* The Pentagons: Pentagon timing, no contention, TR-DOS at home. */
static inline bool spec_is_pentagon_class(const machine_t m) {
    return m == MACHINE_PENTAGON || m == MACHINE_PENT512 || m == MACHINE_PENT1024;
}

/*
 * The Scorpion's descendants: #1FFD, four ROM roles (BASIC 128, BASIC 48,
 * service, TR-DOS), and more RAM at #C000 the newer the board.
 */
static inline bool spec_is_scorpion_class(const machine_t m) {
    return m == MACHINE_SCORPION || m == MACHINE_ZS1024 || m == MACHINE_SCORPEVO
        || m == MACHINE_KAY256 || m == MACHINE_KAY1024 || m == MACHINE_KAY1024B
        || m == MACHINE_PHOENIX;
}

/* KAY's level, as pico-speccy numbers it: 2 the 256, 3 the 1024s, 4 Phoenix. */
static inline unsigned spec_kay_level(const machine_t m) {
    return m == MACHINE_KAY256 ? 2u : (m == MACHINE_KAY1024 || m == MACHINE_KAY1024B) ? 3u
         : m == MACHINE_PHOENIX ? 4u : 0u;
}

/* The SCLD's video modes are built in, not a card. */
static inline bool spec_has_scld(const machine_t m) {
    return m == MACHINE_TIMEX || m == MACHINE_TC2048;
}

extern machine_t spec_machine;

/*
 * The +3's second paging latch, port #1FFD.
 *
 * Bit 0 picks between the ordinary 128K map and the "special" all-RAM
 * one. In the ordinary map bit 2 is the high bit of a two-bit ROM
 * number, whose low bit is #7FFD bit 4 -- so the +3 pages one of FOUR
 * ROMs where a 128K pages one of two. In the special map bits 1-2 pick
 * one of four fixed bank arrangements and no ROM is mapped at all,
 * which is how +3DOS gets 64K of contiguous RAM.
 *
 * Bits 3-4 are the disk motor and drive select, which the FDC reads.
 */
extern volatile uint8_t spec_port_1ffd;

/*
 * The Profi's extra paging latch, port #DFFD, decoded on all sixteen
 * bits as their Ports.cpp decodes it.
 *
 *   bits 0-2  the upper RAM page GROUP: with #7FFD bits 0-2 this makes
 *             eight groups of eight, which is the 1 MB
 *   bit 4     RAM page 0 at #0000 instead of ROM; setting it also
 *             clears the paging lock, unconditionally
 *   bit 5     the CP/M extended-periphery mode
 *   bit 6     RAM page 6 at #8000 instead of page 2
 *   bit 7     the DS80 512x240 video mode, which this board does not
 *             have a renderer for and ignores
 */
extern volatile uint8_t spec_port_dffd;

/*
 * The Pentagon 1024SL's #EFF7: D0 the 16-colour mode, D2 "no more than
 * 128K" (#7FFD bit 5 is a lock again), D3 the cache page at #0000, D4
 * turbo (latched, ignored: the clock is the clock). And whether the
 * 512/1024's hidden cache page is in, by IN #FB / IN #7B.
 */
extern volatile uint8_t spec_port_eff7;
extern volatile bool    spec_pent16c;

/*
 * The Timex's horizontal select register, port #F4, and the DOCK/EX-ROM
 * select that rides in bit 7 of the SCLD's own port #FF.
 *
 * One bit of the HSR per 8 KB chunk: set means that chunk comes from
 * the cartridge dock or the EX-ROM instead of the ordinary machine.
 * This is the reason the bus engine pages in eight windows and not four.
 */
extern volatile uint8_t spec_timex_hsr;
extern volatile bool    spec_timex_exrom;

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

/*
 * Machines with more than 128 KB, on a part with 512 KB.
 *
 * The bus engine reads guest RAM on every memory cycle, so what is
 * MAPPED has to be in SRAM -- a PSRAM miss there stretches a clock
 * phase by about a microsecond. But a Scorpion's 256 KB and a Profi's
 * 1 MB are mostly not mapped: the first eight banks are the 128K
 * machine every one of them also is, and the rest only ever appear in
 * the window at #C000, one at a time.
 *
 * So banks 0-7 stay in SRAM and everything above lives in the PSRAM,
 * with one 16 KB staging page in SRAM holding whichever high bank is
 * currently mapped. Switching costs a write-back and a read -- two
 * 16 KB transfers on a paging port write, which is a few hundred
 * microseconds on a path that happens hundreds of times a second and
 * not hundreds of thousands.
 *
 * This is pico-speccy's idea, from the tiers their MemESP::ram[] pages
 * between; the shape here is simpler because only one window can hold a
 * high bank.
 */
#define SPEC_MAX_BANKS 64
#define SPEC_MAX_BANKS_BIG 256   /* the ATM-Turbo 3: 4 MB, the upper 3 in the PSRAM arena */
unsigned spec_ram_banks(void);                   /* 1 MB, which is the Profi */
uint8_t *spec_bank_ptr(unsigned bank);

/*
 * A RAM page for a machine that can put any page in any window (the ATM):
 * the first eight from SRAM, the rest straight through the PSRAM's XIP
 * cache. Never staged, so never out of step with itself.
 */
uint8_t *spec_ram_page(unsigned page);

/*
 * A 16 KB ROM page from flash, in one of the four SRAM ROM slots: the
 * least recently used is refilled on a miss. For the machines whose ROM
 * is bigger than four pages and pages any of it anywhere.
 */
uint8_t *spec_rom_page(const uint8_t *src);

/*
 * The same bank for reading from the host, without staging it: a
 * snapshot writer walking every bank must not page the machine's own
 * window about while it does.
 */
const uint8_t *spec_bank_host(unsigned bank);

/* Push the staged bank back before anything reads the PSRAM copy. */
void spec_bank_flush(void);

/*
 * Re-apply the memory map.
 *
 * Exposed for the interfaces that page themselves -- the Beta Disk and
 * DivMMC both decide from the outside what the bottom of the address
 * space holds, and both have to be able to say so.
 */
void apply_paging_hook(void);

// ============================================================================
// State the video driver reads
// ============================================================================

/* The bank the ULA displays: 5 normally, 7 when 0x7FFD bit 3 is set. */
extern volatile uint8_t spec_screen_bank;

/* Border colour, 0-7. Written by OUT (0xFE),A while a frame is drawing. */
extern volatile uint8_t spec_border;

/* Flips every 16 frames; inverts any cell whose attribute has bit 7. */
extern volatile bool spec_flash;

/*
 * ULA+, from pico-speccy's Ports.cpp and Video.cpp.
 *
 * Two odd ports -- 0xBF3B selects a register, 0xFF3B reads or writes it
 * -- and sixty-four palette entries in G3R3B2. With the palette enabled
 * the attribute byte stops meaning FLASH and BRIGHT and starts meaning
 * "which of four groups of sixteen": bits 7-6 pick the group, 5-3 the
 * paper within it, 2-0 the ink. FLASH goes away with them, which is why
 * turning it on clears the flash counter.
 *
 * Sixty-four arbitrary colours out of a two-bit DAC is only possible
 * because every Spectrum pixel is already doubled into a 2x2 block of
 * output pixels -- which is exactly one Bayer cell. See vga.c.
 */
extern volatile bool    spec_ulaplus_on;    // the palette is enabled
extern volatile uint8_t spec_ulaplus_reg;
extern uint8_t          spec_ulaplus_pal[64];
/* Raised when an entry changed, so the video driver rebuilds its tables. */
extern volatile bool    spec_ulaplus_dirty;

/*
 * The Timex SCLD's video modes, from port #FF.
 *
 * Bits 0-2 pick the mode and bits 3-5 the hi-res colour:
 *
 *   0  the ordinary screen
 *   1  the second screen, the one at #6000
 *   2  hi-colour -- the same bitmap, but one attribute per PIXEL row
 *      instead of per character row, read from #6000 at the bitmap's own
 *      scrambled offset
 *   6  hi-res -- 512x192 monochrome, built by taking byte-columns
 *      alternately from the two areas: #4000 supplies the first eight
 *      pixels of each pair and #6000 the next eight
 *
 * Both areas are in bank 5, which is 16 KB at #4000, so the second is
 * simply +0x2000 into the same bank.
 */
extern volatile uint8_t spec_timex_mode;    // 0, 1, 2 or 6
extern volatile uint8_t spec_timex_ink;     // hi-res ink, 0-7

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

/* Does this machine have an AY at all? A 48K only with settings.ay48. */
bool spec_have_ay(void);

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

/* The Kempston mouse: see spectrum.c. Written by the USB driver. */
extern volatile uint8_t spec_mouse_x, spec_mouse_y, spec_mouse_buttons, spec_mouse_wheel;
/* The mouse's movement as USB reports it, not yet taken: the Profi's
 * serial mouse sends deltas rather than positions. */
extern volatile int16_t spec_mouse_dx, spec_mouse_dy;
extern volatile bool    spec_mouse_seen;

/* Gigascreen "Auto" has seen a program flipping screens every frame. */
extern volatile bool spec_giga_auto;

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

/* An Interface 2 ROM cartridge: takes the 48K's ROM's place from its next reset. */
void spec_if2_insert(const uint8_t *img, uint32_t len);

/* An ALF cartridge, up to 1 MB in 16 KB banks. */
bool spec_alf_insert(const uint8_t *img, uint32_t len);
void spec_if2_eject(void);
bool spec_if2_present(void);

/* The machine's own clock in Hz, turbo included. */
uint32_t spec_cpu_hz(void);

/*
 * How the frames are keeping time, for the settings page.
 *
 * idle_us is how long the last frame waited for its deadline -- the
 * headroom, measured. A machine with none is late, and late counts the
 * frames that were. capacity is what the engine managed unpaced the last
 * time spec_tune_clock() looked; target is the machine's own rate.
 */
typedef struct {
    uint32_t run_us;            // the last frame, start to deadline check
    uint32_t idle_us;           // ...and how long it then waited
    uint32_t idle_min_us;       // the least it has waited since the last tune
    uint32_t late;              // frames that missed their deadline
    uint32_t capacity_mfps;     // fps x1000, unpaced
    uint32_t target_mfps;       // fps x1000, the machine's own
} spec_timing_t;
extern spec_timing_t spec_timing;

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
 * Read the CPU's registers, by NMI. The machine must be paused; core 1
 * does the work from its pause loop and this waits for it. See the NMI
 * dump in spectrum.c for what it costs the guest and what it cannot see.
 */
struct z80_state_s;
bool spec_capture_state(struct z80_state_s *st);

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
// on a 48K (its only ROM) than on a 128K or +2 (ROM 1), or a +3 (ROM 3).
// src/spectrum/tape.c patches a stub over LD-BYTES in whichever window
// slot holds it.
//
// The substitution is in the window and not a copy of it, so it survives
// the CPU paging the ROM out and in again -- which a 128K game does every
// time it loads -- and it cannot outlive the ROM it was made for, because
// a reset refills the window from flash and puts the stub back afterwards.

/*
 * The window slot holding the 48K BASIC ROM on the current machine.
 *
 * The tape trap patches its LD-BYTES in place. It is a window slot and
 * not a copy, so a reset restores the pristine image for free -- and
 * there is no second 16 KB of the same ROM in the SRAM.
 */
uint8_t *spec_basic_slot(void);


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
// keyboard: spec_key_ascii() takes a character off the USB console and
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
