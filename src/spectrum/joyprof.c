/*
 * frank-dino — an RP2350B acting as the whole machine around a real Z80
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-dino
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Pad maps by name, so a game's map comes back with the game.
 *
 * pico-speccy's idea (JoyProfiles.cpp, its joystick.cfg) in its format:
 * one profile a line, `name <TAB> joystick type <TAB> t0,t1,...,t13`, the
 * targets as joymap.h numbers. A text file, so it can be written by hand.
 */
#include "joyprof.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ff.h"
#include "joymap.h"
#include "settings.h"

#define PATH "/joystick.cfg"

typedef struct {
    char    name[JOYPROF_NAME];
    uint8_t type;
    uint8_t map[PAD_SLOTS];
} prof_t;

static prof_t profs[JOYPROF_MAX];
static int    nprofs;

static void read_all(void) {
    nprofs = 0;
    FIL f;
    if (f_open(&f, PATH, FA_READ) != FR_OK) return;
    char line[128];
    while (nprofs < JOYPROF_MAX && f_gets(line, sizeof line, &f)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        char *tab1 = strchr(line, '\t');
        if (!tab1) continue;
        char *tab2 = strchr(tab1 + 1, '\t');
        if (!tab2) continue;
        prof_t *p = &profs[nprofs];
        memset(p, 0, sizeof *p);
        *tab1 = 0;
        snprintf(p->name, sizeof p->name, "%.*s", (int)(sizeof p->name - 1), line);
        p->type = (uint8_t)atoi(tab1 + 1);
        char *c = tab2 + 1;
        for (int i = 0; i < PAD_SLOTS && *c; i++) {
            const int v = atoi(c);
            p->map[i] = (uint8_t)(v >= 0 && v < JT_COUNT ? v : 0);
            c = strchr(c, ',');
            if (!c) break;
            c++;
        }
        nprofs++;
    }
    f_close(&f);
}

static bool write_all(void) {
    FIL f;
    if (f_open(&f, PATH, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) return false;
    f_printf(&f, "# frank-dino pad maps: name <TAB> joystick type <TAB> 14 targets\n");
    for (int n = 0; n < nprofs; n++) {
        const prof_t *p = &profs[n];
        f_printf(&f, "%s\t%d\t", p->name, p->type);
        for (int i = 0; i < PAD_SLOTS; i++) f_printf(&f, i ? ",%d" : "%d", p->map[i]);
        f_printf(&f, "\n");
    }
    return f_close(&f) == FR_OK;
}

int joyprof_count(void) { read_all(); return nprofs; }

const char *joyprof_name(const int i) { return (i >= 0 && i < nprofs) ? profs[i].name : ""; }

bool joyprof_save(const char *name) {
    read_all();
    int slot = nprofs;
    for (int i = 0; i < nprofs; i++) if (!strcmp(profs[i].name, name)) slot = i;
    if (slot >= JOYPROF_MAX) return false;
    prof_t *p = &profs[slot];
    snprintf(p->name, sizeof p->name, "%s", name);
    for (char *c = p->name; *c; c++) if (*c == '\t' || *c == '\n') *c = ' ';
    p->type = settings.joystick;
    memcpy(p->map, settings.joymap, PAD_SLOTS);
    if (slot == nprofs) nprofs++;
    return write_all();
}

bool joyprof_load(const int i) {
    if (i < 0 || i >= nprofs) return false;
    settings.joystick = profs[i].type;
    memcpy(settings.joymap, profs[i].map, PAD_SLOTS);
    return true;
}
