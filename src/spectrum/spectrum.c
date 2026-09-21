/*
 * frank-dino — an RP2350B acting as the whole machine around a real Z80
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-dino
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "spectrum.h"

#include <string.h>
#include <pico/time.h>
#include <hardware/gpio.h>

#include "audio.h"
#include "ay.h"
#include "beta.h"
#include "dino.h"
#include "settings.h"
#include "tape.h"
#include "z80_bus.h"

// ============================================================================
// The ROMs
// ============================================================================
//
// Welded into the binary rather than read off the card: the board has a
// microSD slot that nothing reads yet, and a machine that cannot start
// without a file on a card is a machine that cannot be tested by a
// script. See roms/README.md for where these two came from.
#define IMPORT_BIN(file, sym) asm (\
    ".section .rodata." #sym "\n" \
    ".balign 4\n"                 \
    ".global " #sym "\n"          \
    #sym ":\n"                    \
    ".incbin \"" file "\"\n"      \
    ".balign 4\n"                 \
    ".section \".text\"\n")

IMPORT_BIN("roms/128-0.rom", spec_rom0);
IMPORT_BIN("roms/128-1.rom", spec_rom1);
IMPORT_BIN("roms/48.rom",    spec_rom48);
IMPORT_BIN("roms/plus2.rom", spec_romp2);
IMPORT_BIN("roms/pentagon-0.rom", spec_rompent);
IMPORT_BIN("roms/trdos.rom", spec_romtrd);
extern const uint8_t spec_rom0[SPEC_BANK_SIZE];
extern const uint8_t spec_rom1[SPEC_BANK_SIZE];
extern const uint8_t spec_rom48[SPEC_BANK_SIZE];
extern const uint8_t spec_romp2[SPEC_BANK_SIZE];
extern const uint8_t spec_rompent[SPEC_BANK_SIZE];
extern const uint8_t spec_romtrd[SPEC_BANK_SIZE];

machine_t spec_machine = MACHINE_128K;

const char *spec_machine_name(void) {
    switch (spec_machine) {
        case MACHINE_48K:      return "ZX Spectrum 48K";
        case MACHINE_PLUS2:    return "ZX Spectrum +2";
        case MACHINE_PENTAGON: return "Pentagon 128";
        default:               return "ZX Spectrum 128K";
    }
}

/*
 * Overclocking a real Z80, in halves.
 *
 * The clock is generated here, edge by edge, and the part in the socket
 * is a Z84C00-10 -- so a faster Z80 is not a trick, it is the CPU
 * actually running faster. turbo_num() is the multiplier over two: 2 is
 * 3.5 MHz, 3 is 5.3, 4 is 7.
 *
 * What changes is how much work fits between two interrupts. The frame
 * is still a fiftieth of a second, so a faster CPU means more T-states
 * in it -- which is why the frame length is a function rather than a
 * constant, and why the AY has to be told about it: the chip has its own
 * 1.7734 MHz and must keep it, or every tune plays sharp.
 */
static inline uint32_t turbo_num(void) { return 2u + settings.turbo; }

uint32_t spec_frame_tstates(void) {
    uint32_t base;
    switch (spec_machine) {
        case MACHINE_48K:      base = SPEC_TSTATES_PER_FRAME_48;   break;
        case MACHINE_PENTAGON: base = SPEC_TSTATES_PER_FRAME_PENT; break;
        default:               base = SPEC_TSTATES_PER_FRAME;      break;
    }
    return base * turbo_num() / 2;
}

// ============================================================================
// State
// ============================================================================
uint8_t spec_ram[SPEC_RAM_BANKS][SPEC_BANK_SIZE] __attribute__((aligned(4)));

volatile uint8_t  spec_screen_bank = 5;
volatile uint8_t  spec_border      = 7;
volatile bool     spec_flash       = false;
volatile uint32_t spec_frames      = 0;
volatile uint8_t  spec_port_7ffd   = 0;

// Once 0x7FFD bit 5 is set the machine cannot page again until it is
// reset. That is how a 128K pretends to be a 48K, and how the 48K BASIC
// mode in the menu works — so it has to be honoured, not ignored.
static bool paging_locked = false;

// Keyboard: bit set = key up. Five keys per half-row, bits 0-4.
static volatile uint8_t kbd[8];

/*
 * The matrix, as the ULA sees it.
 *
 * Row order is address-line order — row 0 is A8 — and within a row bit 0
 * is the key nearest the outside of the keyboard. Both halves run inward
 * from the edges, which is why 1-5 and 0-6 look reversed with respect to
 * each other and are not a typo.
 */
enum { R_CSZXCV = 0, R_ASDFG, R_QWERT, R_12345, R_09876, R_POIUY, R_ENTER, R_SPACE };

// ---------------------------------------------------------------------------
// Sound
// ---------------------------------------------------------------------------
volatile bool     spec_beeper;
volatile uint32_t spec_ay_writes;

/*
 * Does this machine have an AY at all?
 *
 * A 128K and a +2 always do. A 48K never did from Sinclair, but half the
 * clones and a good many add-on boards put one on the same two ports,
 * and a fair amount of 48K music expects to find it -- so it is an
 * option rather than a fact about the machine.
 */
static inline bool have_ay(void) {
    return spec_machine != MACHINE_48K || settings.ay48;
}

static ay_t    ay[2];
static uint8_t ay_sel;          // which chip the ports are talking to

// The last byte written to the ULA's port, which the EAR bit reads back.
static volatile uint8_t last_fe;

/*
 * Two ways of bolting a real DAC onto a machine that has none.
 *
 * Covox is one 8-bit latch on port #FB, and SounDrive (the "Covox quad")
 * is four of them, two per side. Both are just a byte that goes straight
 * to the speaker, so there is nothing to model: the mixer adds the
 * latched value in and the guest does the rest. Off by default, because
 * a machine that never had one must not have writes to those ports turn
 * into noise.
 */
static volatile uint8_t covox_level;
static volatile uint8_t sd_level[4];

// ============================================================================
// Paging
// ============================================================================

/*
 * A stand-in for the 48K BASIC ROM, or NULL.
 *
 * src/spectrum/tape.c installs a patched copy of that ROM here so a call
 * to LD-BYTES lands in its own code instead. It goes through the paging
 * function rather than being mapped directly because the CPU can page the
 * ROM out and back at any moment, and the patch has to survive that: a
 * 128K game loading from tape sits in ROM 1 while it loads and returns to
 * ROM 0 afterwards.
 */
static const uint8_t *tape_rom;

static void apply_paging(void);

const uint8_t *spec_rom_48k_image(void) {
    return spec_machine == MACHINE_48K ? spec_rom48 : spec_rom1;
}

void spec_set_tape_rom(const uint8_t *page) {
    tape_rom = page;
    apply_paging();
}

/* The 48K BASIC ROM, or the patched copy standing in for it. */
static const uint8_t *basic_rom(void) {
    return tape_rom ? tape_rom : spec_rom_48k_image();
}

static void apply_paging(void) {
    const uint8_t v = spec_port_7ffd;

    /*
     * The Beta Disk's ROM sits on top of everything.
     *
     * When the interface has decided its ROM is in -- see spec_opfetch()
     * for the rule, which is about where the CPU is fetching from and
     * nothing else -- it replaces whatever the bottom window would
     * otherwise hold, on every machine. So this runs first and the rest
     * of the function only decides what is underneath it.
     */
    if (unlikely(beta_rom_paged())) {
        z80_map_page(0, (uint8_t *)spec_romtrd, NULL);
        if (spec_machine == MACHINE_48K) {
            z80_map_page(1, spec_ram[5], spec_ram[5]);
            z80_map_page(2, spec_ram[2], spec_ram[2]);
            z80_map_page(3, spec_ram[0], spec_ram[0]);
            spec_screen_bank = 5;
        } else {
            z80_map_page(1, spec_ram[5], spec_ram[5]);
            z80_map_page(2, spec_ram[2], spec_ram[2]);
            z80_map_page(3, spec_ram[v & 7], spec_ram[v & 7]);
            spec_screen_bank = (v & 0x08) ? 7 : 5;
        }
        return;
    }

    /*
     * A 48K has one ROM and no paging at all: whatever is in the latch,
     * the map is fixed. Modelling that as "the latch exists but is
     * ignored" rather than as a separate code path keeps one function
     * responsible for the memory map.
     */
    if (spec_machine == MACHINE_48K) {
        z80_map_page(0, (uint8_t *)basic_rom(), NULL);
        z80_map_page(1, spec_ram[5], spec_ram[5]);
        z80_map_page(2, spec_ram[2], spec_ram[2]);
        z80_map_page(3, spec_ram[0], spec_ram[0]);
        spec_screen_bank = 5;
        return;
    }

    // Bottom window: ROM 1 (48K BASIC) when bit 4 is set, ROM 0 otherwise.
    // Read-only, which here means "its write pointer goes to the bucket".
    const uint8_t *rom0 = spec_machine == MACHINE_PLUS2    ? spec_romp2
                        : spec_machine == MACHINE_PENTAGON ? spec_rompent
                                                           : spec_rom0;
    z80_map_page(0, (uint8_t *)((v & 0x10) ? basic_rom() : rom0), NULL);

    // The two fixed windows never move. Mapped every time anyway, because
    // the cost is two stores on a path taken a few hundred times a second
    // and the alternative is a special case that can be got wrong.
    z80_map_page(1, spec_ram[5], spec_ram[5]);
    z80_map_page(2, spec_ram[2], spec_ram[2]);

    const uint8_t bank = v & 0x07;
    z80_map_page(3, spec_ram[bank], spec_ram[bank]);

    spec_screen_bank = (v & 0x08) ? 7 : 5;
}

// ---------------------------------------------------------------------------
// Launching
// ---------------------------------------------------------------------------
static uint16_t tramp_jp;
static bool     tramp_seen_jp;
static bool     tramp_active;

void spec_set_paging(const uint8_t value) {
    spec_port_7ffd = value;
    apply_paging();
}

static void update_opfetch_hook(void);

/*
 * Everything that has to watch where the CPU is fetching from.
 *
 * Two things do, and they are unrelated except in wanting the same
 * event. The trampoline needs the instruction after its jump, so it can
 * put the real ROM back at the exact moment nothing of it is needed any
 * more. The Beta Disk needs every fetch, because its ROM appearing and
 * disappearing is a function of the address being fetched and of nothing
 * else.
 *
 * This is on the hottest path there is -- once per opcode, a few million
 * times a second -- so it is not installed at all unless one of the two
 * actually wants it. That is what update_opfetch_hook() is for.
 */
static void spec_opfetch(const uint16_t addr) {
    if (unlikely(tramp_active)) {
        /*
         * The first opcode fetch after the trampoline's jump is the
         * destination, and this runs before that fetch is answered — so
         * putting the real ROM back here means the destination's first
         * byte comes from the ROM even when the destination is itself
         * inside the bottom 16K, which a few snapshots' program counters
         * are.
         */
        if (!tramp_seen_jp) {
            if (addr == tramp_jp) tramp_seen_jp = true;
            return;
        }
        apply_paging();             // the real ROM, per the latch
        tramp_active = false;
        update_opfetch_hook();
        return;
    }

    if (!settings.betadisk) return;

    /*
     * The Beta Disk's paging rule, which is the whole of its address
     * decoding: the TR-DOS ROM appears when an opcode is fetched from
     * 0x3Dxx with the 48K BASIC ROM selected, and goes away on the first
     * opcode fetch at 0x4000 or above.
     *
     * That is why entering TR-DOS is spelled RANDOMIZE USR 15616. 15616
     * is 0x3D00, and calling it is the entire mechanism -- there is no
     * port and no latch, just a comparator on the address bus.
     */
    if (addr >= 0x4000) {
        if (unlikely(beta_rom_paged())) {
            beta_set_rom_paged(false);
            apply_paging();
        }
        return;
    }

    if (beta_rom_paged()) return;

    // On a 128K the rule only applies while ROM 1 is selected; the
    // editor ROM has its own use for that address.
    const bool basic48 = spec_machine == MACHINE_48K || (spec_port_7ffd & 0x10);
    if ((addr & 0xFF00) == 0x3D00 && basic48) {
        beta_set_rom_paged(true);
        apply_paging();
    }
}

static void update_opfetch_hook(void) {
    z80_opfetch_cb = (tramp_active || settings.betadisk) ? spec_opfetch : NULL;
}

void spec_begin_launch(uint8_t *page, const uint16_t jp_addr) {
    tramp_jp       = jp_addr;
    tramp_seen_jp  = false;
    tramp_active   = true;
    z80_map_page(0, page, NULL);    // read-only, like the ROM it replaces
    update_opfetch_hook();
}

bool spec_launch_pending(void) { return tramp_active; }

// ============================================================================
// Ports
// ============================================================================
/*
 * Two ports, and the decode is the interesting part of both.
 *
 * Nothing on a Spectrum decodes an address fully. The ULA answers every
 * port with A0 low — 0xFE is merely the one the ROM happens to use — and
 * the 128K's paging latch answers every port with A15 and A1 both low.
 * Programs rely on this: half the loaders in existence write the border
 * through 0xFF or 0x7E rather than 0xFE.
 *
 * The port argument is the whole 16-bit address, which matters twice
 * over. The keyboard is read by pulling one of A8-A15 low and seeing
 * which of the five bits comes back clear, and the paging latch is
 * selected by A15/A1. Neither is visible in the low byte alone.
 */
static void spec_io_out(const uint16_t port, const uint8_t value) {
    /*
     * The tape trap first, and it only ever answers when a patched ROM
     * is mapped -- nothing else on the machine can reach the port it
     * uses, because the only code that knows about it is code this
     * firmware wrote into the ROM window itself.
     */
    if (tape_port_out(port, value)) return;
    if (beta_port_out(port, value)) return;

    if ((port & 0x0001) == 0) {                 // the ULA
        spec_border = value & 0x07;

        /*
         * The beeper. Bit 4 of this port is the speaker on a real
         * Spectrum, driven from the ULA's latch.
         *
         * This used to be a gpio_put() straight to GP46 — the CPU's own
         * edges reaching the transducer with nothing in between, which
         * is exactly what 1-bit audio is. The AY ended that: the pin is
         * a PWM DAC now and cannot also be a logic output, so the beeper
         * becomes a level that the mixer samples 44100 times a second
         * along with everything else.
         *
         * The loss is real but small. Core 1 runs the frame in
         * sample-sized slices and reads this between them, so an edge
         * still lands in the sample it happened in; what is gone is
         * sub-sample detail, which for a beeper mostly means the very
         * top of its range.
         */
        spec_beeper = (value & 0x10) != 0;
        last_fe     = value;
    }

    /*
     * Covox on #FB and SounDrive on #0F/#1F/#4F/#5F.
     *
     * Decoded on the low byte alone, which is how the cards themselves
     * did it. #1F is also where a Kempston joystick answers -- but that
     * is a read and this is a write, so the two never meet.
     */
    if (settings.covox && (port & 0xFF) == 0xFB) covox_level = value;
    if (settings.soundrive) {
        switch (port & 0xFF) {
            case 0x0F: sd_level[0] = value; break;
            case 0x1F: sd_level[1] = value; break;
            case 0x4F: sd_level[2] = value; break;
            case 0x5F: sd_level[3] = value; break;
            default: break;
        }
    }

    /*
     * The AY, on two ports that are both "A15 high".
     *
     * Register select is A15 and A14 both high (0xFFFD); data is A15 high
     * with A14 low (0xBFFD). As everywhere else on this machine the
     * decode is partial, and as everywhere else programs rely on it.
     */
    if (have_ay() && (port & 0xC002) == 0xC000) {   // select
        /*
         * TurboSound steals the top of the register range.
         *
         * 0xFF and 0xFE are not register numbers there, they pick which
         * of the two chips the next data writes reach. A single-chip
         * machine must treat them as ordinary selects of a register that
         * does not exist, which is why this only acts when the second
         * chip has been turned on.
         */
        if (settings.turbosound && (value & 0xFE) == 0xFE) {
            ay_sel = (uint8_t)(value & 1 ? 0 : 1);   // 0xFF = first, 0xFE = second
        } else {
            ay_select(&ay[ay_sel], value);
        }
        return;
    }
    if (have_ay() && (port & 0xC002) == 0x8000) {   // data
        ay_write(&ay[ay_sel], value);
        spec_ay_writes++;
        return;
    }

    if (spec_machine != MACHINE_48K && (port & 0x8002) == 0 && !paging_locked) {
        spec_port_7ffd = value;
        if (value & 0x20) paging_locked = true;
        apply_paging();
    }
}

// ---------------------------------------------------------------------------
// Joysticks
// ---------------------------------------------------------------------------
/*
 * One set of five bits, read five different ways.
 *
 * Nothing about a Spectrum joystick is standard, because the machine has
 * no joystick port: every interface is somebody's add-on and they agree
 * about nothing. Kempston put it on a port of its own, active high.
 * Sinclair wired two of them straight onto keyboard rows -- so a
 * Sinclair joystick IS keys 6-0 and 1-5, and a game reading it is
 * reading the keyboard. The Cursor interface picked a third set of keys.
 * Fuller used yet another port, with the bits in a different order and
 * inverted.
 *
 * So the state is held once, in joy_state, and each interface is a way
 * of presenting it. The Sinclairs and the Cursor are folded into the
 * matrix at read time rather than pressed into it, which keeps a real
 * key and a joystick direction from cancelling each other out when they
 * happen to be the same key.
 */
static volatile uint8_t joy_state;      // bits per JOY_BIT_*

void spec_joy_set(const uint8_t bit, const bool pressed) {
    if (bit > JOY_BIT_FIRE) return;
    if (pressed) joy_state |=  (uint8_t)(1u << bit);
    else         joy_state &= (uint8_t)~(1u << bit);
}

uint8_t spec_joy_state(void) { return joy_state; }

#define JOY(b) (joy_state & (1u << (b)))

/* Which bits of half-row `row` a keyboard-wired joystick is holding down. */
static uint8_t joy_row_mask(const unsigned row) {
    uint8_t m = 0;
    switch (settings.joystick) {
        case JOY_SINCLAIR1:             // keys 6 7 8 9 0, on half-row 4
            if (row != R_09876) break;
            if (JOY(JOY_BIT_LEFT))  m |= 1u << 4;   // 6
            if (JOY(JOY_BIT_RIGHT)) m |= 1u << 3;   // 7
            if (JOY(JOY_BIT_DOWN))  m |= 1u << 2;   // 8
            if (JOY(JOY_BIT_UP))    m |= 1u << 1;   // 9
            if (JOY(JOY_BIT_FIRE))  m |= 1u << 0;   // 0
            break;

        case JOY_SINCLAIR2:             // keys 1 2 3 4 5, on half-row 3
            if (row != R_12345) break;
            if (JOY(JOY_BIT_LEFT))  m |= 1u << 0;   // 1
            if (JOY(JOY_BIT_RIGHT)) m |= 1u << 1;   // 2
            if (JOY(JOY_BIT_DOWN))  m |= 1u << 2;   // 3
            if (JOY(JOY_BIT_UP))    m |= 1u << 3;   // 4
            if (JOY(JOY_BIT_FIRE))  m |= 1u << 4;   // 5
            break;

        case JOY_CURSOR:                // 5 6 7 8 and 0 -- the ROM's own arrows
            if (row == R_12345 && JOY(JOY_BIT_LEFT))  m |= 1u << 4;   // 5
            if (row == R_09876) {
                if (JOY(JOY_BIT_DOWN))  m |= 1u << 4;   // 6
                if (JOY(JOY_BIT_UP))    m |= 1u << 3;   // 7
                if (JOY(JOY_BIT_RIGHT)) m |= 1u << 2;   // 8
                if (JOY(JOY_BIT_FIRE))  m |= 1u << 0;   // 0
            }
            break;

        default: break;                 // Kempston and Fuller have their own ports
    }
    return m;
}

/*
 * Kempston: its own port, active high, and only five bits of it.
 *
 * The decode is A5 low and nothing else, which is why #1F is the usual
 * name for it and why it collides with a good deal else. Some interfaces
 * sat on #DF instead; a few games only find one or the other.
 */
static uint8_t kempston_byte(void) {
    uint8_t v = 0;
    if (JOY(JOY_BIT_RIGHT)) v |= 0x01;
    if (JOY(JOY_BIT_LEFT))  v |= 0x02;
    if (JOY(JOY_BIT_DOWN))  v |= 0x04;
    if (JOY(JOY_BIT_UP))    v |= 0x08;
    if (JOY(JOY_BIT_FIRE))  v |= 0x10;
    return v;
}

/* Fuller: port #7F, inverted, and in a different order again. */
static uint8_t fuller_byte(void) {
    uint8_t v = 0;
    if (JOY(JOY_BIT_UP))    v |= 0x01;
    if (JOY(JOY_BIT_DOWN))  v |= 0x02;
    if (JOY(JOY_BIT_LEFT))  v |= 0x04;
    if (JOY(JOY_BIT_RIGHT)) v |= 0x08;
    if (JOY(JOY_BIT_FIRE))  v |= 0x80;
    return (uint8_t)~v;
}

// ---------------------------------------------------------------------------

static uint8_t spec_io_in(const uint16_t port) {
    uint8_t v;
    if (tape_port_in(port, &v)) return v;
    if (beta_port_in(port, &v)) return v;

    if (settings.joystick == JOY_KEMPSTON &&
        (port & 0xFF) == (settings.kempston_port ? 0xDF : 0x1F))
        return kempston_byte();

    if (settings.joystick == JOY_FULLER && (port & 0xFF) == 0x7F)
        return fuller_byte();

    if ((port & 0x0001) == 0) {                 // the ULA: keyboard
        uint8_t result = 0x1F;                  // five keys, all up

        // A half-row is selected by its address line being LOW, and more
        // than one can be low at once — which is not a corner case, it is
        // how the ROM scans the whole keyboard with a single IN from
        // 0x00FE. So this ANDs every selected row rather than picking one.
        for (unsigned row = 0; row < 8; row++)
            if (!(port & (0x0100u << row))) result &= (uint8_t)(kbd[row] & ~joy_row_mask(row));

        /*
         * Bit 6 is the EAR input, and what it reads with no tape
         * attached is the difference between an issue 2 board and an
         * issue 3 one.
         *
         * Both feed the last value written to port 0xFE back into it. An
         * issue 3 looks at bit 4 alone -- the speaker; an issue 2 looks
         * at bits 3 and 4 together, so setting the MIC bit also lifts
         * EAR. A handful of 48K games test this to tell the two boards
         * apart, and one written for an issue 2 reads the key rows as
         * permanently pressed on an issue 3.
         */
        const uint8_t ear = settings.issue2 ? ((last_fe & 0x18) ? 0x40 : 0)
                                            : ((last_fe & 0x10) ? 0x40 : 0);
        return (uint8_t)(result | 0xA0 | ear);
    }

    // The AY's read-back port is the select port, not the data one.
    if (have_ay() && (port & 0xC002) == 0xC000)
        return ay_read(&ay[ay_sel]);

    /*
     * Everything else is the floating bus, and 0xFF is the honest answer
     * for it here. On real hardware an unclaimed IN returns whatever the
     * ULA happened to be fetching, which a handful of effects depend on;
     * reproducing that needs the video and the CPU to share a clock, and
     * they do not on this board.
     */
    return 0xFF;
}

// ============================================================================
// Keyboard
// ============================================================================

void spec_key_set(const uint8_t row, const uint8_t bit, const bool pressed) {
    if (row > 7 || bit > 4) return;
    if (pressed) kbd[row] &= ~(1u << bit);
    else         kbd[row] |=  (1u << bit);
}

typedef struct { uint8_t row, bit; } key_t;

static const key_t key_letters[26] = {
    /* A */ {R_ASDFG, 0}, /* B */ {R_SPACE, 4}, /* C */ {R_CSZXCV, 3},
    /* D */ {R_ASDFG, 2}, /* E */ {R_QWERT, 2}, /* F */ {R_ASDFG, 3},
    /* G */ {R_ASDFG, 4}, /* H */ {R_ENTER, 4}, /* I */ {R_POIUY, 2},
    /* J */ {R_ENTER, 3}, /* K */ {R_ENTER, 2}, /* L */ {R_ENTER, 1},
    /* M */ {R_SPACE, 2}, /* N */ {R_SPACE, 3}, /* O */ {R_POIUY, 1},
    /* P */ {R_POIUY, 0}, /* Q */ {R_QWERT, 0}, /* R */ {R_QWERT, 3},
    /* S */ {R_ASDFG, 1}, /* T */ {R_QWERT, 4}, /* U */ {R_POIUY, 3},
    /* V */ {R_CSZXCV, 4}, /* W */ {R_QWERT, 1}, /* X */ {R_CSZXCV, 2},
    /* Y */ {R_POIUY, 4}, /* Z */ {R_CSZXCV, 1},
};

static const key_t key_digits[10] = {
    /* 0 */ {R_09876, 0}, /* 1 */ {R_12345, 0}, /* 2 */ {R_12345, 1},
    /* 3 */ {R_12345, 2}, /* 4 */ {R_12345, 3}, /* 5 */ {R_12345, 4},
    /* 6 */ {R_09876, 4}, /* 7 */ {R_09876, 3}, /* 8 */ {R_09876, 2},
    /* 9 */ {R_09876, 1},
};

// How long a console keystroke is held. The ROM scans the keyboard once
// per frame and debounces across frames, so a press that lasts one frame
// is a press it can miss entirely.
#define KEY_HOLD_FRAMES 3

static struct { uint8_t row, bit; } held[4];
static unsigned held_count;
static unsigned held_frames;

// Joystick directions held by a console keystroke, released on the same
// timer as the matrix keys above.
static uint8_t held_joy;

#define SYM() hold(R_SPACE, 1)      // SYMBOL SHIFT, which most symbols need

static void hold(const uint8_t row, const uint8_t bit) {
    if (held_count >= count_of(held)) return;
    held[held_count].row = row;
    held[held_count].bit = bit;
    held_count++;
    spec_key_set(row, bit, true);
}

/*
 * Cursor keys arrive as three bytes, so this needs one byte of memory.
 *
 * A terminal sends ESC [ A for up. The Spectrum has no cursor keys at
 * all — they are CAPS SHIFT plus 5, 6, 7 and 8, which is what the 128K
 * menu reads — so the sequence is reassembled here and pressed as the
 * shifted digit. Two bytes of state rather than a parser, because there
 * is exactly one sequence shape worth recognising.
 */
static uint8_t esc_state;   // 0 = idle, 1 = seen ESC, 2 = seen ESC [

void spec_key_ascii(char c) {
    if (esc_state == 1) { esc_state = (c == '[') ? 2 : 0; return; }
    if (esc_state == 2) {
        esc_state = 0;
        if (held_count || held_joy) return;

        /*
         * An arrow is the joystick when there is one to be.
         *
         * Otherwise it is CAPS SHIFT plus a digit, which is what the
         * Spectrum's own cursor keys are and what the 128K menu reads.
         */
        if (settings.joystick != JOY_NONE && settings.cursor_as_joy) {
            held_frames = KEY_HOLD_FRAMES;
            switch (c) {
                case 'A': held_joy = 1u << JOY_BIT_UP;    break;
                case 'B': held_joy = 1u << JOY_BIT_DOWN;  break;
                case 'C': held_joy = 1u << JOY_BIT_RIGHT; break;
                case 'D': held_joy = 1u << JOY_BIT_LEFT;  break;
                default:  held_frames = 0; return;
            }
            joy_state |= held_joy;
            return;
        }

        held_frames = KEY_HOLD_FRAMES;
        hold(R_CSZXCV, 0);                      // CAPS SHIFT
        switch (c) {
            case 'A': hold(R_09876, 3); break;  // up    = CAPS + 7
            case 'B': hold(R_09876, 4); break;  // down  = CAPS + 6
            case 'C': hold(R_09876, 2); break;  // right = CAPS + 8
            case 'D': hold(R_12345, 4); break;  // left  = CAPS + 5
            default:  held_count = 0; held_frames = 0; break;
        }
        return;
    }
    if (c == 0x1B) { esc_state = 1; return; }

    if (held_count || held_joy) return;         // one keystroke at a time

    held_frames = KEY_HOLD_FRAMES;

    if (c >= 'a' && c <= 'z') { hold(key_letters[c - 'a'].row, key_letters[c - 'a'].bit); return; }
    if (c >= 'A' && c <= 'Z') {
        hold(R_CSZXCV, 0);                      // CAPS SHIFT
        hold(key_letters[c - 'A'].row, key_letters[c - 'A'].bit);
        return;
    }
    if (c >= '0' && c <= '9') { hold(key_digits[c - '0'].row, key_digits[c - '0'].bit); return; }

    /*
     * Everything else is a chord.
     *
     * A Spectrum keyboard has forty keys and no punctuation at all: every
     * symbol is SYMBOL SHIFT with a letter or a digit, and the pairings
     * are printed on the keys rather than following any pattern. This is
     * that table. Without it a BASIC line containing a colon or a bracket
     * cannot be typed, which rules out most of the useful ones.
     */
    switch (c) {
        case '\r': case '\n': hold(R_ENTER,  0); break;
        case ' ':              hold(R_SPACE,  0); break;
        case 0x7F: case '\b':  hold(R_CSZXCV, 0); hold(R_09876, 0); break;  // DELETE

        case '!': SYM(); hold(R_12345, 0); break;
        case '@': SYM(); hold(R_12345, 1); break;
        case '#': SYM(); hold(R_12345, 2); break;
        case '$': SYM(); hold(R_12345, 3); break;
        case '%': SYM(); hold(R_12345, 4); break;
        case '&': SYM(); hold(R_09876, 4); break;
        case '\'':SYM(); hold(R_09876, 3); break;
        case '(': SYM(); hold(R_09876, 2); break;
        case ')': SYM(); hold(R_09876, 1); break;
        case '_': SYM(); hold(R_09876, 0); break;

        case '<': SYM(); hold(R_QWERT, 3); break;   // R
        case '>': SYM(); hold(R_QWERT, 4); break;   // T
        case ';': SYM(); hold(R_POIUY, 1); break;   // O
        case '"': SYM(); hold(R_POIUY, 0); break;   // P
        case '-': SYM(); hold(R_ENTER, 3); break;   // J
        case '+': SYM(); hold(R_ENTER, 2); break;   // K
        case '=': SYM(); hold(R_ENTER, 1); break;   // L
        case '^': SYM(); hold(R_ENTER, 4); break;   // H
        case ':': SYM(); hold(R_CSZXCV, 1); break;  // Z
        case '?': SYM(); hold(R_CSZXCV, 3); break;  // C
        case '/': SYM(); hold(R_CSZXCV, 4); break;  // V
        case '*': SYM(); hold(R_SPACE, 4); break;   // B
        case ',': SYM(); hold(R_SPACE, 3); break;   // N
        case '.': SYM(); hold(R_SPACE, 2); break;   // M

        default: held_frames = 0; break;
    }
}

// ---------------------------------------------------------------------------
// Typing a whole line
// ---------------------------------------------------------------------------
/*
 * One character per few frames, with a gap between them.
 *
 * The gap is the point. spec_key_ascii() presses a key and holds it for
 * three frames, and the ROM's debounce will not register a second press
 * until it has seen the key released -- so a string fed straight in
 * arrives as one character. Feeding it on a timer, with the release
 * visible in between, is what makes LOAD "" typeable from here.
 *
 * The initial delay exists because the thing that wants this is a tape
 * being mounted, which cold-starts the machine first: the ROM needs a
 * second to clear memory and put up a menu before it is listening.
 */
static const char *type_str;
static unsigned    type_wait;

#define TYPE_GAP_FRAMES   4

void spec_type(const char *s, const unsigned delay_frames) {
    type_str  = s;
    type_wait = delay_frames;
}

void spec_type_cancel(void) { type_str = NULL; }

static void type_tick(void) {
    if (!type_str) return;
    if (type_wait) { type_wait--; return; }
    if (held_count || held_joy) return; // still finishing the last one

    if (!*type_str) { type_str = NULL; return; }
    spec_key_ascii(*type_str++);
    type_wait = TYPE_GAP_FRAMES;
}

void spec_key_tick(void) {
    if (!held_count && !held_joy) { type_tick(); return; }
    if (held_frames) { held_frames--; return; }

    for (unsigned i = 0; i < held_count; i++)
        spec_key_set(held[i].row, held[i].bit, false);
    held_count = 0;

    joy_state &= (uint8_t)~held_joy;
    held_joy = 0;
}

// ============================================================================
// Bring-up and the frame loop
// ============================================================================

void spec_init(void) {
    memset(spec_ram, 0, sizeof spec_ram);
    memset((void *)kbd, 0x1F, sizeof kbd);      // every key up
    held_count = 0;

    ay_reset(&ay[0]);
    ay_reset(&ay[1]);
    ay_sel         = 0;
    spec_beeper    = false;
    last_fe        = 0;
    covox_level    = 0x80;
    memset((void *)sd_level, 0x80, sizeof sd_level);
    joy_state      = 0;
    spec_ay_writes = 0;

    esc_state      = 0;
    held_joy       = 0;
    type_str       = NULL;
    paging_locked  = false;

    spec_port_7ffd = 0;
    spec_border    = 7;
    spec_frames    = 0;

    /*
     * A Beta Disk 128 comes up with its own ROM at 0x0000 and lets
     * TR-DOS decide what happens next, which is how a disk with a `boot`
     * file on it starts by itself. Only after a .trd has been mounted,
     * though: on an empty drive that start-up is a wait for a disk that
     * is not there.
     *
     * On a 128K, a +2 or a Pentagon there is a second thing to do.
     * TR-DOS is a 48K program: it expects the 48 BASIC ROM underneath it
     * and its own paging rule only fires while that ROM is selected, so
     * a machine that comes up in the editor ROM pages TR-DOS out on its
     * first fetch above 0x4000 and can never page it back in. Selecting
     * ROM 1 first -- without the lock bit, so a 128K program on the disk
     * still has its banks -- is what a Beta Disk 128 is.
     */
    beta_reset();
    const bool beta_boot = settings.betadisk && beta_take_boot_request();
    if (beta_boot && spec_machine != MACHINE_48K) spec_port_7ffd = 0x10;
    beta_set_rom_paged(beta_boot);

    apply_paging();

    z80_io_in  = spec_io_in;
    z80_io_out = spec_io_out;
    update_opfetch_hook();

    // The stage-0 wiring watch, if a quick check left it on. A machine
    // never wants it and it is expensive: see z80_bus.h.
    z80_edge_watch_stop();

    z80_reset();
}

/*
 * One audio sample's worth of Z80, then one audio sample.
 *
 * A frame is 70908 T-states and 882 samples, which is 80.394 T-states
 * each — so the slice length alternates between 80 and 81 under a
 * fractional accumulator rather than being rounded, because rounding
 * 0.394 away 882 times a frame is 347 T-states of drift every 20 ms.
 */
static uint32_t slice_acc;

static void run_frame(void);

__force_inline static void run_slice(const uint32_t tstates, const uint32_t num) {
    z80_run(tstates);

    /*
     * The mix, once per sample.
     *
     * The beeper is read here rather than pushed by the port handler, so
     * its level is whatever it was at the end of the slice — which is
     * the sample it belongs in. The AYs are advanced by exactly the
     * cycles this slice covered, so nothing drifts against anything.
     */
    int32_t s = 0;

    if (likely(have_ay())) {
        // T-states to AY clocks: half at 3.5 MHz, and a smaller share as
        // the CPU is pushed past it, so the chip's own rate never moves.
        const uint32_t ticks = tstates / num;
        int32_t a = ay_render(&ay[0], ticks);
        if (settings.turbosound) a += ay_render(&ay[1], ticks);
        s += a * settings.ay_vol / 10;
    }

    /*
     * The beeper at about a quarter of full scale.
     *
     * On a real Spectrum the beeper is considerably louder than the AY,
     * and reproducing that faithfully makes every 128K game's music sit
     * under its own sound effects. This is the compromise every emulator
     * makes; it is a mix decision, not a measurement.
     */
    s += (spec_beeper ? 6000 : -6000) * settings.beeper_vol / 10;

    // The extra DACs, if the machine has been told it has them. Each is a
    // byte centred on 0x80, at a quarter scale so four of them together
    // still leave room for everything else.
    if (unlikely(settings.covox)) s += ((int32_t)covox_level - 128) * 64;
    if (unlikely(settings.soundrive))
        for (unsigned i = 0; i < 4; i++) s += ((int32_t)sd_level[i] - 128) * 32;

    audio_push(s * settings.volume / 10);
}

uint32_t spec_tune_clock(void) {
    // Muted, because the machine is being run at deliberately wrong
    // speeds and nobody needs to hear that.
    audio_set_enabled(false);

    /*
     * Let the machine get on with something typical first.
     *
     * How long a frame takes depends on what the CPU is doing in it: an
     * I/O cycle costs more engine time than a memory one, and the ROM's
     * power-on RAM test is almost nothing but memory. Tuning against
     * that -- which is exactly what happens when this is called straight
     * after a reset -- picks a pace that is six per cent too fast for
     * the machine the user then sits in front of, and six per cent is a
     * semitone on everything it plays.
     *
     * A second and a half is enough for a 128K to finish testing its
     * memory and reach its menu.
     */
    for (int f = 0; f < 75; f++) run_frame();

    // What five frames should take, in microseconds.
    const int64_t want = 5ll * 1000000ll * SPEC_TSTATES_PER_FRAME / SPEC_CPU_HZ;
    const int64_t tol  = want / 400;        // a quarter of a per cent

    int32_t pace = (int32_t)z80_get_pace();
    int64_t us   = want;

    /*
     * Proportional, then fine.
     *
     * Stepping by one and measuring five frames each time is 100 ms an
     * iteration, and the starting point comes from z80_calibrate(), which
     * timed the bus engine on its own and therefore lands a long way off
     * once two AY chips are in the loop. The first version walked ±1 and
     * ran out of its iteration budget mid-descent, leaving the machine
     * 1.9 % fast with the tuner reporting success.
     *
     * One pace unit is about two per cent of a frame, so the error in per
     * mille divided by twenty is roughly how many units to move. That
     * converges in three or four measurements; the ±1 fallback at the end
     * takes care of the last step, where the estimate rounds to nothing.
     */
    for (int iter = 0; iter < 16; iter++) {
        const uint64_t t0 = time_us_64();
        for (int f = 0; f < 5; f++) run_frame();
        us = (int64_t)(time_us_64() - t0);

        const int64_t err = us - want;                  // >0 means too slow
        if (err > -tol && err < tol) break;

        int32_t step = (int32_t)((err * 1000 / want) / 20);
        if (step == 0) step = err > 0 ? 1 : -1;

        /*
         * Down to the floor and no further.
         *
         * Padding is the only thing this can trade, and below
         * Z80_PACE_MIN the engine samples the CPU's pins before they are
         * valid -- so a frame that is still too long at the floor means
         * the machine cannot run at 50 Hz, not that it should be driven
         * harder. The alternative is a Spectrum that keeps perfect time
         * and computes nonsense, which is what happened.
         */
        pace -= step;                                   // slower -> less padding
        if (pace < (int32_t)Z80_PACE_MIN) pace = (int32_t)Z80_PACE_MIN;
        z80_set_pace((uint32_t)pace);
        if (pace == (int32_t)Z80_PACE_MIN && err > 0) break;
    }

    /*
     * One more measurement, so the number returned is the one that is
     * true now.
     *
     * The loop's last `us` was taken *before* its last adjustment, which
     * made the report disagree with the machine — it announced 51.5 Hz
     * while the board was actually running 49.75. A figure that is wrong
     * in the direction of looking worse is still wrong, and this one is
     * printed on the boot screen where it will be believed.
     */
    const uint64_t t0 = time_us_64();
    for (int f = 0; f < 5; f++) run_frame();
    us = (int64_t)(time_us_64() - t0);

    audio_set_enabled(true);
    return (uint32_t)((5ll * 1000000000ll) / (us ? us : 1));   // fps x1000
}

static volatile bool pause_req, paused;

void spec_pause(void) {
    pause_req = true;
    // A machine that is not running cannot acknowledge, and one that has
    // not started yet never will -- so this waits, but not for ever.
    const absolute_time_t deadline = make_timeout_time_ms(200);
    while (!paused && absolute_time_diff_us(get_absolute_time(), deadline) > 0)
        tight_loop_contents();
}

void spec_resume(void)    { pause_req = false; }
bool spec_is_paused(void) { return paused; }

static void pause_point(void) {
    if (!pause_req) return;
    paused = true;
    while (pause_req) tight_loop_contents();
    paused = false;
}

void spec_set_machine(const machine_t m) {
    if (m >= MACHINE_COUNT) return;
    spec_machine = m;
    // A 48K's BASIC ROM and a 128K's ROM 1 are different images, so a
    // patched copy of one is not a patched copy of the other. Anything
    // mounted has to be re-patched before the new machine is started.
    tape_machine_changed();
    spec_init();
}

void spec_next_machine(void) {
    spec_set_machine((machine_t)((spec_machine + 1) % MACHINE_COUNT));
}

/*
 * One frame, without the pause check.
 *
 * Split out because the clock tuner runs frames too, and it runs them
 * from core 0 with the machine already paused -- so a pause point inside
 * them would park the core that is holding the pause, which is a
 * deadlock and was one: changing the Z80 speed from the settings page
 * hung the firmware with the page still on the screen.
 */
static void run_frame(void) {
    /*
     * The whole main loop of the machine, sliced.
     *
     * An emulator spends a frame stepping a CPU and counting contention.
     * This spends it letting a Z84C00 be a Z80, stopping 882 times to
     * take a sample. The only thing the ULA owes the CPU is an interrupt
     * every 70908 T-states, and the only thing that has to be right is
     * the count.
     */
    const uint32_t num   = turbo_num();
    const uint32_t total = spec_frame_tstates();
    const uint32_t per   = total / AUDIO_SAMPLES_PER_FRAME;
    const uint32_t rem   = total % AUDIO_SAMPLES_PER_FRAME;

    // The ULA holds ~INT low for about ten microseconds, which is 36
    // T-states at 3.5 MHz and proportionally more at any other speed --
    // it is a length of time, not a number of instructions.
    const uint32_t held = SPEC_INT_TSTATES * num / 2;

    uint32_t budget = total - held;

    for (uint32_t i = 0; i < AUDIO_SAMPLES_PER_FRAME; i++) {
        uint32_t slice = per;
        slice_acc += rem;
        if (slice_acc >= AUDIO_SAMPLES_PER_FRAME) {
            slice_acc -= AUDIO_SAMPLES_PER_FRAME;
            slice++;
        }

        if (slice > budget) slice = budget;
        budget -= slice;
        run_slice(slice, num);
        if (!budget) break;
    }

    if (budget) run_slice(budget, num);

    z80_set_int(true);
    run_slice(held, num);
    z80_set_int(false);

    const uint32_t f = spec_frames + 1;
    spec_frames = f;

    // The ULA inverts FLASH cells every 16 frames, so the full cycle is
    // 32 and works out at about two-thirds of a second.
    spec_flash = (f & 0x10) != 0;

    spec_key_tick();
}

void spec_run_frame(void) {
    pause_point();
    run_frame();
}
