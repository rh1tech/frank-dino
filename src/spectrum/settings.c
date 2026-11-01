/*
 * frank-dino — an RP2350B acting as the whole machine around a real Z80
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-dino
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "settings.h"
#include "joymap.h"
#include "ide.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ay.h"
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
    { "ay_stereo",      &settings.ay_stereo,      AY_STEREO_COUNT - 1 },
    { "ay48",           &settings.ay48,           1  },
    { "turbosound",     &settings.turbosound,     1  },
    { "covox",          &settings.covox,          COVOX_COUNT - 1 },
    { "specdrum",       &settings.specdrum,       1  },
    { "tsfm",           &settings.tsfm,           1  },
    { "opl3",           &settings.opl3,           1  },
    { "opll",           &settings.opll,           1  },
    { "sn76489",        &settings.sn76489,        1  },
    { "cms",            &settings.cms,            1  },
    { "soundrive",      &settings.soundrive,      1  },
    { "saa",            &settings.saa,            1  },
    { "real_player",    &settings.real_player,    1  },

    { "beam",           &settings.beam,           1  },
    { "contention",     &settings.contention,     1  },
    { "rom_spanish",    &settings.rom_spanish,    1  },
    { "scanlines",      &settings.scanlines,      4  },
    { "palette",        &settings.palette,        5  },
    { "crt",            &settings.crt,            6  },
    { "ulaplus",        &settings.ulaplus,        1  },
    { "disk_wrprot",    &settings.disk_wrprot,    1  },
    { "esxdos",         &settings.esxdos,         1  },
    { "gigascreen",     &settings.gigascreen,     2  },
    { "timex",          &settings.timex,          1  },
    { "ui_theme",       &settings.ui_theme,       UI_THEME_COUNT - 1 },

    { "joystick",       &settings.joystick,       JOY_COUNT - 1 },
    { "kempston_port",  &settings.kempston_port,  3  },
    { "mouse",          &settings.mouse,          1  },
    { "rtc",            &settings.rtc,            1  },
    { "trdos_rom",      &settings.trdos_rom,      4  },
    { "beta_target",    &settings.beta_target,    3  },
    { "p3_speedlock",   &settings.p3_speedlock,   1  },
    { "zcontroller",    &settings.zcontroller,    1  },
    { "ide",            &settings.ide,            IDE_COUNT - 1 },
    { "mouse_sens",     &settings.mouse_sens,     8  },
    { "cursor_as_joy",  &settings.cursor_as_joy,  1  },
    { "joymap0",      &settings.joymap[0],   JT_COUNT - 1 },
    { "joymap1",      &settings.joymap[1],   JT_COUNT - 1 },
    { "joymap2",      &settings.joymap[2],   JT_COUNT - 1 },
    { "joymap3",      &settings.joymap[3],   JT_COUNT - 1 },
    { "joymap4",      &settings.joymap[4],   JT_COUNT - 1 },
    { "joymap5",      &settings.joymap[5],   JT_COUNT - 1 },
    { "joymap6",      &settings.joymap[6],   JT_COUNT - 1 },
    { "joymap7",      &settings.joymap[7],   JT_COUNT - 1 },
    { "joymap8",      &settings.joymap[8],   JT_COUNT - 1 },
    { "joymap9",      &settings.joymap[9],   JT_COUNT - 1 },
    { "joymap10",     &settings.joymap[10],  JT_COUNT - 1 },
    { "joymap11",     &settings.joymap[11],  JT_COUNT - 1 },
    { "joymap12",     &settings.joymap[12],  JT_COUNT - 1 },
    { "joymap13",     &settings.joymap[13],  JT_COUNT - 1 },

    { "bus_trace",      &settings.bus_trace,      1  },
    { "betadisk",       &settings.betadisk,       1  },
    { "tape_autostart", &settings.tape_autostart, 1  },
    { "tape_realtime",  &settings.tape_realtime,  1  },
    { "tape_rg",        &settings.tape_rg,        1  },
    { "tape_wear",      &settings.tape_wear,      3  },
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
        /*
         * ABC, because the board has two channels now and ABC is what
         * most stereo AY music was mixed on -- the Pentagon and nearly
         * every clone that had stereo at all. A tune written for ACB
         * sounds lopsided in it, not wrong, and it is one setting away.
         */
        .ay_stereo      = AY_STEREO_ABC,
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
        .saa            = 0,
        .real_player    = 0,

        /*
         * On: the picture is the one the ULA would have drawn, which is
         * the only way multicolour and border effects come out at all.
         * Off reads the screen memory live, as it used to.
         */
        .beam           = 1,
        /*
         * On: a 48K, 128K or +3 loses time to its ULA the way the real
         * ones did, which is what demo-scene timing and some loaders
         * count on. Turbo turns it off by itself.
         */
        .contention     = 1,
        .scanlines      = 0,
        .ulaplus        = 0,
        .disk_wrprot    = 0,
        .esxdos         = 0,
        .gigascreen     = 0,
        .timex          = 0,
        .ui_theme       = UI_THEME_BLACK,

        .joystick       = JOY_KEMPSTON,
        .kempston_port  = 0,
        .mouse          = 1,
        .rtc            = 1,
        .mouse_sens     = 2,
        /*
         * The arrows are always the Spectrum's own cursor keys, CAPS
         * SHIFT and 5-8; this only adds the joystick on top, as
         * pico-speccy's CursorAsJoy does. So it can default to on
         * without the 128K menu or BASIC losing them.
         */
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

bool settings_save_to(const char *path, const char *name) {
    FIL f;
    if (f_open(&f, path, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) return false;

    if (name) f_printf(&f, "# name: %s\n", name);
    else      f_printf(&f, "# frank-dino settings\n");
    for (unsigned i = 0; i < FIELD_COUNT; i++)
        f_printf(&f, "%s %u\n", fields[i].key, *fields[i].at);

    return f_close(&f) == FR_OK;
}

bool settings_save(void) { return settings_save_to(CFG_PATH, NULL); }

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

bool settings_load(void) { return settings_load_from(CFG_PATH); }

bool settings_load_from(const char *path) {
    settings_defaults();

    FIL f;
    if (f_open(&f, path, FA_READ) != FR_OK) return false;

    char line[64];
    while (f_gets(line, sizeof line, &f)) {
        char *nl = strpbrk(line, "\r\n");
        if (nl) *nl = 0;
        apply_line(line);
    }

    f_close(&f);
    return true;
}

// ---------------------------------------------------------------------------
// Profiles
// ---------------------------------------------------------------------------
/*
 * Whole settings files kept by name in /profiles: a machine, its sound,
 * its joystick, saved together and brought back together. pico-speccy's
 * (Config::profileSave/profileLoad) in this file's own format -- a
 * profile is a settings file with its name in the first line.
 */
#define PROF_DIR "/profiles"

static bool prof_path(const int slot, char *out, const unsigned n) {
    if (slot < 0 || slot >= SETTINGS_PROFILES) return false;
    snprintf(out, n, PROF_DIR "/profile%02d.cfg", slot);
    return true;
}

bool settings_profile_name(const int slot, char *out, const unsigned n) {
    char path[40];
    out[0] = 0;
    if (!prof_path(slot, path, sizeof path)) return false;
    FIL f;
    if (f_open(&f, path, FA_READ) != FR_OK) return false;
    char line[64];
    const bool got = f_gets(line, sizeof line, &f) != NULL;
    f_close(&f);
    if (!got || strncmp(line, "# name: ", 8) != 0) { snprintf(out, n, "Profile %d", slot + 1); return true; }
    char *nl = strpbrk(line + 8, "\r\n");
    if (nl) *nl = 0;
    snprintf(out, n, "%s", line + 8);
    return true;
}

int settings_profile_save(const char *name) {
    f_mkdir(PROF_DIR);
    char path[40], have[32];
    int slot = -1;
    for (int i = 0; i < SETTINGS_PROFILES; i++) {          // the same name, overwritten
        if (settings_profile_name(i, have, sizeof have) && !strcmp(have, name)) { slot = i; break; }
    }
    for (int i = 0; slot < 0 && i < SETTINGS_PROFILES; i++) // else the first free slot
        if (!settings_profile_name(i, have, sizeof have)) slot = i;
    if (slot < 0 || !prof_path(slot, path, sizeof path)) return -1;
    return settings_save_to(path, name) ? slot : -1;
}

bool settings_profile_load(const int slot) {
    char path[40];
    return prof_path(slot, path, sizeof path) && settings_load_from(path);
}

bool settings_profile_delete(const int slot) {
    char path[40];
    return prof_path(slot, path, sizeof path) && f_unlink(path) == FR_OK;
}
