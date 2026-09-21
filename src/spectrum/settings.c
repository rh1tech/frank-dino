/*
 * frank-dino — an RP2350B acting as the whole machine around a real Z80
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-dino
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "settings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ff.h"
#include "spectrum.h"

settings_t settings;

#define CFG_PATH "/frank-dino.cfg"

/*
 * The one table.
 *
 * Every persisted setting is a name and the byte it lives in, so saving
 * is a loop and loading is a lookup. Adding a setting means adding a row
 * here and nothing else -- which is the whole reason the struct is all
 * uint8_t.
 */
typedef struct { const char *key; uint8_t *at; uint8_t max; } field_t;

static const field_t fields[] = {
    { "machine",        &settings.machine,        MACHINE_COUNT - 1 },
    { "issue2",         &settings.issue2,         1  },
    { "turbo",          &settings.turbo,          2  },

    { "output",         &settings.output,         OUTPUT_COUNT - 1 },
    { "volume",         &settings.volume,         10 },
    { "beeper_vol",     &settings.beeper_vol,     10 },
    { "ay_vol",         &settings.ay_vol,         10 },
    { "ay48",           &settings.ay48,           1  },
    { "turbosound",     &settings.turbosound,     1  },
    { "covox",          &settings.covox,          1  },
    { "soundrive",      &settings.soundrive,      1  },

    { "scanlines",      &settings.scanlines,      1  },

    { "joystick",       &settings.joystick,       JOY_COUNT - 1 },
    { "kempston_port",  &settings.kempston_port,  1  },
    { "cursor_as_joy",  &settings.cursor_as_joy,  1  },

    { "bus_trace",      &settings.bus_trace,      1  },
    { "betadisk",       &settings.betadisk,       1  },
    { "tape_autostart", &settings.tape_autostart, 1  },
};

#define FIELD_COUNT (sizeof fields / sizeof fields[0])

void settings_defaults(void) {
    settings = (settings_t){
        .machine        = MACHINE_128K,
        .issue2         = 0,
        .turbo          = 0,

        /*
         * Line level by default, which is the quiet choice.
         *
         * Too quiet into a buzzer is a buzzer that wants turning up; too
         * loud into a line input is distortion on everything, and it is
         * not obvious what is causing it. One of those is a setting away
         * from right and the other cost an evening.
         */
        .output         = OUTPUT_HDMI,
        .volume         = 8,
        .beeper_vol     = 8,
        .ay_vol         = 8,
        .ay48           = 0,
        /*
         * TurboSound on, which is not the obvious default.
         *
         * A real TurboSound board is always both chips, and software
         * that does not know about it never writes 0xFE or 0xFF to the
         * select port -- the 128K ROM only ever names registers 0-13. So
         * the second chip costs nothing to have and is the difference
         * between hearing a TS tune and hearing half of one.
         */
        .turbosound     = 1,
        .covox          = 0,
        .soundrive      = 0,

        .scanlines      = 0,

        .joystick       = JOY_KEMPSTON,
        .kempston_port  = 0,
        .cursor_as_joy  = 1,

        /*
         * The Beta Disk off by default, which a Pentagon owner would
         * find odd.
         *
         * It is not free: its ROM replaces the BASIC ROM whenever the
         * CPU fetches from 0x3Dxx, which means every opcode fetch has to
         * be inspected for it. Mounting a .trd turns it on.
         */
        .bus_trace      = 0,
        .betadisk       = 0,
        .tape_autostart = 1,
    };
}

// ---------------------------------------------------------------------------

bool settings_save(void) {
    FIL f;
    if (f_open(&f, CFG_PATH, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) return false;

    f_printf(&f, "# frank-dino settings\n");
    for (unsigned i = 0; i < FIELD_COUNT; i++)
        f_printf(&f, "%s %u\n", fields[i].key, *fields[i].at);

    return f_close(&f) == FR_OK;
}

static void apply_line(char *line) {
    while (*line == ' ' || *line == '\t') line++;
    if (*line == '#' || *line == 0) return;

    char *sp = line;
    while (*sp && *sp != ' ' && *sp != '\t') sp++;
    if (!*sp) return;
    *sp++ = 0;
    while (*sp == ' ' || *sp == '\t') sp++;

    const long v = strtol(sp, NULL, 10);
    if (v < 0) return;

    for (unsigned i = 0; i < FIELD_COUNT; i++) {
        if (strcmp(line, fields[i].key) != 0) continue;
        // Clamped rather than rejected: a file written by a later version
        // with more machines in it should still start this one.
        *fields[i].at = (uint8_t)(v > fields[i].max ? fields[i].max : v);
        return;
    }
}

bool settings_load(void) {
    settings_defaults();

    FIL f;
    if (f_open(&f, CFG_PATH, FA_READ) != FR_OK) return false;

    char line[64];
    while (f_gets(line, sizeof line, &f)) {
        char *nl = strpbrk(line, "\r\n");
        if (nl) *nl = 0;
        apply_line(line);
    }

    f_close(&f);
    return true;
}
