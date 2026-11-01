/*
 * frank-dino — an RP2350B acting as the whole machine around a real Z80
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-dino
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Everything about the machine that is a preference rather than a fact.
 *
 * The list is pico-speccy's, filtered down to what this board can
 * actually do. Theirs covers twenty machines, four disk interfaces, six
 * sound cards and a network stack; what is left here is the part that
 * means something when the CPU is a real Z84C00, the video is a resistor
 * ladder into an HDMI converter and the sound is one PWM pin.
 *
 * Everything is a small integer, including the booleans, because the
 * options menu walks these as "an index into a list of labels" and a
 * uniform representation is what makes that menu a table rather than a
 * switch statement.
 *
 * The emulator reads these fields directly on hot paths. That is
 * deliberate: a setting that has to be "applied" before it takes effect
 * is a setting that can be out of step with what the menu says, and the
 * cost of reading a byte out of a struct in an I/O handler is nothing
 * next to the bus cycle that got us there.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

// ============================================================================
// Joystick
// ============================================================================
//
// A Spectrum has no joystick port, so every "joystick" is an interface
// somebody sold, and they disagree about everything: Kempston is its own
// port, the Sinclairs and the Cursor are keyboard rows, and Fuller is a
// third port with the bits inverted. A game supports one of them and
// says so on the inlay, which is why this has to be a setting.
typedef enum : uint8_t {
    JOY_NONE = 0,
    JOY_KEMPSTON,       // port #1F (or #DF), active high, 000FUDLR
    JOY_SINCLAIR1,      // keys 6-0 on half-row 4
    JOY_SINCLAIR2,      // keys 1-5 on half-row 3
    JOY_CURSOR,         // 5,6,7,8 and 0 -- what the ROM's own cursor keys are
    JOY_FULLER,         // port #7F, active low, F---RLDU
    JOY_COUNT,
} joystick_t;

/*
 * How hard to drive the outputs, for whichever of them is listening.
 *
 * Full scale is about three volts peak to peak out of the PWM and the
 * LM358 followers, which is what headphones and the PAM8403 want and
 * roughly three times what a line input wants. Feeding that to the HDMI
 * encoder is heard as distortion on everything, and it is not the DAC's
 * fault -- the board has no attenuator between the followers and the
 * encoder, so the level has to come out of the mix.
 */
/*
 * Which of three palettes the menu draws itself in.
 *
 * The menu is pico-speccy's, and so are two of these. Theirs is designed
 * in a slate theme -- 0x1C232F for a panel, 0x101116 behind it -- whose
 * darks sit nowhere near the four levels a two-resistor ladder can make,
 * so their VGA driver offers it two ways: dithered across a 2x2 block of
 * output pixels, or rounded onto the grid by hand.
 *
 * On this board the dither is a visible checkerboard. It has only the
 * one doubled pixel to work in, and the step it has to fill -- black to
 * 0x555555 -- is a third of full scale, so a panel that should be a flat
 * near-black comes out as grey speckle. It is kept because it is their
 * code and on some displays it is the closer answer, but it is not the
 * default here.
 */
typedef enum : uint8_t {
    UI_THEME_BLACK = 0,     // on the grid, panels black -- the default
    UI_THEME_NAVY,          // pico-speccy's own on-grid table, panels navy
    UI_THEME_SLATE,         // their slate theme, Bayer-dithered
    UI_THEME_COUNT,
} ui_theme_t;

typedef enum : uint8_t {
    OUTPUT_JACK = 0,    // the 3.5 mm jack (J2) or the speaker, through U6
    OUTPUT_HDMI,        // the MS9288A's line inputs, which want a third of it
    OUTPUT_LOUD,        // the speaker, two and a half times up, clipping the peaks
    OUTPUT_COUNT,
} output_t;

/* The directions, as the rest of the firmware talks about them. */
enum { JOY_BIT_RIGHT = 0, JOY_BIT_LEFT, JOY_BIT_DOWN, JOY_BIT_UP, JOY_BIT_FIRE };

/*
 * Where the Covox answers. #FB is the Pentagon's and the common one;
 * #DD is the Scorpion's, which pico-speccy offers too.
 */
typedef enum : uint8_t {
    COVOX_OFF = 0,
    COVOX_FB,
    COVOX_DD,
    COVOX_COUNT,
} covox_t;

typedef struct {
    // --- machine ---
    uint8_t machine;        // machine_t; the selected machine
    uint8_t issue2;         // 48K keyboard revision: 0 = issue 3, 1 = issue 2
    uint8_t turbo;          // Z80 clock: 0 = 3.5 MHz, 1 = 5.3, 2 = 7

    // --- audio ---
    /*
     * Where the sound is being listened to, which the firmware cannot see.
     *
     * The jack is always fed; S2 adds the MS9288A's audio inputs and the
     * speaker amplifier, and it is a switch with no sense line -- so this
     * is the one thing about the audio path that has to be told rather
     * than measured. It matters because the destinations want very
     * different levels: headphones and the PAM8403 want the duty swung
     * as wide as it will go, and the encoder's line inputs want about a
     * volt peak to peak and were getting three.
     */
    uint8_t output;         // output_t
    uint8_t volume;         // master, 0-10
    uint8_t beeper_vol;     // 0-10
    uint8_t ay_vol;         // 0-10
    uint8_t ay_stereo;      // ay_stereo_t: mono, ABC or ACB
    uint8_t ay48;           // an AY on a 48K, as half the clones had
    uint8_t turbosound;     // a second AY behind the same pair of ports
    uint8_t covox;          // covox_t: an 8-bit DAC on #FB or #DD
    uint8_t specdrum;       // the Cheetah SpecDrum's DAC on #DF
    uint8_t tsfm;           // TurboSound FM: a YM2203's FM half behind each AY
    uint8_t opl3;           // the VGM card's YMF262 on #C4-#C7
    uint8_t opll;           // its YM2413 on #C0/#C1
    uint8_t sn76489;        // its SN76489 pair on #C2/#C3 (#C9)
    uint8_t cms;            // Creative Music System: two SAA1099s on the #FF family
    uint8_t soundrive;      // four 8-bit DACs on #0F/#1F/#4F/#5F
    uint8_t saa;            // a SAA1099 on the #FF family, as the Profi has
    uint8_t real_player;    // load from a cassette on J8 rather than an image

    // --- video ---
    uint8_t rom_spanish;    // Investronica's ROMs on the 48K, 128K and +2
    uint8_t contention;     // the ULA stops the CPU for screen fetches, as on a real 48K/128K
    uint8_t beam;           // build the picture as the ULA saw it: multicolour, border effects
    uint8_t scanlines;      // dim every second output line
    /*
     * ULA+: sixty-four programmable colours through ports #BF3B/#FF3B.
     *
     * Off by default, as it is on a real machine: the attribute byte
     * means something different while it is on, so software that does
     * not know about it would come out in whatever the palette happened
     * to hold. Turning it on only makes the ports answer -- the guest
     * still has to enable the palette itself.
     */
    uint8_t ulaplus;
    /*
     * Gigascreen: show both screens at once instead of alternately.
     *
     * The trick the software plays is to write two pictures, into banks
     * 5 and 7, and flip the ULA between them every frame -- at 50 Hz the
     * eye averages the pair and sees colours the fifteen do not contain.
     * On a fifty-hertz television it flickers; on anything since, it
     * flickers worse.
     *
     * pico-speccy answers that by blending the two into one displayed
     * colour, and so does this -- but where theirs has a framebuffer and
     * a 120-entry blend table, this has neither, so the blend happens in
     * the 2x2 output block every Spectrum pixel already occupies. Half
     * the block is bank 5 and half is bank 7, and the averaging is the
     * eye's, in space rather than in time. No flicker at all.
     *
     * With it on the ULA's own screen-select bit is ignored: both are
     * always on screen, so there is nothing left for it to select.
     */
    uint8_t gigascreen;
    /*
     * The Timex SCLD's video modes, through port #FF.
     *
     * Off by default because #FF is the floating-bus address: a great
     * deal of ordinary software writes to it by accident, and a machine
     * that switched to hi-res because a game happened to OUT there would
     * look broken with no way to guess why.
     */
    uint8_t timex;
    uint8_t ui_theme;       // no longer chosen: the menus have one look; kept so old files load

    // --- input ---
    uint8_t joystick;       // joystick_t
    uint8_t kempston_port;  // 0 = #1F, 1 = #DF, 2 = #37, 3 = #5F
    uint8_t mouse;          // a USB mouse is a Kempston mouse
    uint8_t rtc;            // Mr Gluk's clock on #DFF7/#BFF7
    uint8_t ide;            // ide_scheme_t: a hard disk behind NEMO, DivIDE or ATM ports
    uint8_t zcontroller;    // a Z-Controller on #57/#77, on the DivMMC's card image
    uint8_t trdos_rom;      // 0 5.04T, 1 5.03, 2 5.04TM, 3 5.05D, 4 6.11e
    uint8_t mouse_sens;     // 1-8, in halves: 2 is one count per USB count
    uint8_t cursor_as_joy;  // the cursor keys drive the selected joystick too

    // --- diagnostics ---
    uint8_t bus_trace;      // record every bus cycle into the ring

    // --- storage ---
    uint8_t betadisk;       // the Beta Disk interface and its TR-DOS ROM
    /*
     * The write-protect tab, for both drives.
     *
     * On means the guest's writes never reach the image at all, which is
     * what the switch does on the real interface -- not "refuse to save
     * afterwards" but "the disk does not change". Off by default,
     * because a game that saves progress should be able to.
     */
    uint8_t disk_wrprot;
    /*
     * DivMMC, and esxDOS in its EEPROM.
     *
     * Off by default and mutually exclusive with the Beta Disk in
     * practice: both page themselves in on a fetch at #3D00-#3DFF, so
     * with the two on they fight over the bottom of the address space.
     */
    uint8_t esxdos;
    uint8_t tape_autostart; // type LOAD "" after mounting a .tap
    uint8_t tape_realtime;  // play every tape as a signal, never by the ROM trap
    uint8_t tape_rg;        // Rodolfo Guerra's fast-load ROM timings for .tap blocks
} settings_t;

extern settings_t settings;

/* What a machine with nothing configured should be. */
void settings_defaults(void);

/*
 * Read and write /frank-dino.cfg on the card.
 *
 * Plain "key value" text rather than a packed struct: it survives this
 * file gaining a field, it can be fixed with a text editor when a setting
 * makes the machine unbootable, and an unknown key is skipped rather than
 * being a version mismatch that throws the whole file away.
 *
 * Both need the card, so both are core 0's business, with the machine
 * paused -- the same rule as every other FatFs call in this firmware.
 */
bool settings_load(void);
bool settings_save(void);
