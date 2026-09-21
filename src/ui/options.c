/*
 * frank-dino — an RP2350B acting as the whole machine around a real Z80
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-dino
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "options.h"

#include <stdio.h>
#include <string.h>

#include "menu.h"
#include "psram.h"
#include "screen.h"
#include "settings.h"
#include "spectrum.h"
#include "storage.h"
#include "tape.h"
#include "vga.h"
#include "z80_bus.h"

// ---------------------------------------------------------------------------
// The rows
// ---------------------------------------------------------------------------
/*
 * A table, not a screenful of code.
 *
 * Every setting is a byte with a small range, which is what makes this
 * possible: a row is a label, the byte, how many values it has and what
 * to call them. Drawing is then one loop and changing a value is one
 * increment, so adding a setting is adding a line here.
 *
 * The rows that are not settings -- the headings and the three actions
 * at the bottom -- are the same struct with the value pointer left null,
 * rather than a second list interleaved with this one.
 */
typedef enum : uint8_t {
    ROW_HEADING,
    ROW_CHOICE,     // a value with named options
    ROW_NUMBER,     // a value shown as itself
    ROW_ACTION,     // Enter does something
    ROW_INFO,       // a line of text the page computes
} row_kind_t;

typedef struct {
    row_kind_t          kind;
    const char         *label;
    uint8_t            *value;
    uint8_t             count;          // number of options, for CHOICE
    const char *const  *names;          // count of them, for CHOICE
    void              (*changed)(void); // after the value moves
    void              (*action)(void);  // for ACTION rows
    const char       *(*info)(void);    // for INFO rows
} row_t;

static const char *const off_on[]   = { "Off", "On" };
static const char *const machines[] = { "ZX Spectrum 48K", "ZX Spectrum 128K", "ZX Spectrum +2" };
static const char *const turbos[]   = { "3.5 MHz (real)", "5.3 MHz", "7 MHz" };
static const char *const joysticks[]= { "None", "Kempston", "Sinclair 1", "Sinclair 2",
                                        "Cursor", "Fuller" };
static const char *const kports[]   = { "#1F", "#DF" };

// --- what the rows do -------------------------------------------------------

static bool dirty;              // something changed since the page opened
static char status[VGA_COLS];

static void machine_changed(void) {
    spec_set_machine((machine_t)settings.machine);
    snprintf(status, sizeof status, "%s started", spec_machine_name());
}

/*
 * A faster Z80 than the one Sinclair specified.
 *
 * The clock is ours, edge by edge, and the part in the socket is a
 * Z84C00-10 -- so this is not a trick, it is the CPU actually running
 * faster. The frame is still a fiftieth of a second and the interrupt
 * still comes fifty times a second; what changes is how much the machine
 * gets done in between. A game that paces itself off the interrupt runs
 * at the right speed and simply thinks faster; one that counts
 * instructions runs at double speed, which is the price.
 *
 * Music is not affected. The AY keeps its own 1.7734 MHz whatever the
 * CPU is doing, which is what the divisor in run_slice() is for.
 */
static void turbo_changed(void) {
    /*
     * Re-tune and say what actually happened.
     *
     * Asking for 7 MHz does not make it so: the RP2350 has to generate
     * every edge and answer every bus cycle, and past some speed it
     * simply cannot keep up -- the pace bottoms out at zero and the
     * machine runs slow rather than fast. That failure is invisible from
     * the inside, so the measured frame rate goes on the status line and
     * the user can see 41 Hz and pick a lower setting.
     */
    const uint32_t fps = spec_tune_clock();
    snprintf(status, sizeof status, "Z80 at %s - %lu.%02lu frames a second",
             turbos[settings.turbo],
             (unsigned long)(fps / 1000), (unsigned long)((fps % 1000) / 10));
}

static void nothing(void) { }

static const char *tape_info(void) {
    static char buf[48];
    if (!tape_mounted()) { snprintf(buf, sizeof buf, "no tape"); return buf; }
    snprintf(buf, sizeof buf, "block %lu of %lu",
             (unsigned long)tape_block_index() + 1, (unsigned long)tape_block_count());
    return buf;
}

static void tape_action(void) {
    if (!tape_mounted()) { snprintf(status, sizeof status, "No tape is mounted."); return; }
    tape_eject();
    snprintf(status, sizeof status, "Tape ejected.");
}

static void save_action(void) {
    if (!storage_mounted()) { snprintf(status, sizeof status, "No card to save to."); return; }
    snprintf(status, sizeof status,
             settings_save() ? "Settings saved to /frank-dino.cfg"
                             : "Could not write /frank-dino.cfg");
}

static void defaults_action(void) {
    settings_defaults();
    spec_set_machine((machine_t)settings.machine);
    dirty = true;
    snprintf(status, sizeof status, "Defaults restored.");
}

/*
 * What the board turned out to have, rather than what it was built with.
 *
 * The PSRAM is a fitted-or-not part and the probe runs too early for its
 * answer to reach a console that has not enumerated yet, so the one
 * place it is reliably readable is here.
 */
static const char *psram_info(void) {
    static char buf[32];
    const uint32_t n = psram_size();
    if (n) snprintf(buf, sizeof buf, "%lu MB", (unsigned long)(n >> 20));
    else   snprintf(buf, sizeof buf, "not fitted");
    return buf;
}

static void reset_action(void) {
    spec_set_machine((machine_t)settings.machine);
    snprintf(status, sizeof status, "%s reset.", spec_machine_name());
}

#define HEAD(t)            { .kind = ROW_HEADING, .label = (t) }
#define CHOICE(t, v, n, c) { .kind = ROW_CHOICE, .label = (t), .value = (v), \
                             .count = (uint8_t)(sizeof (n) / sizeof (n)[0]), \
                             .names = (n), .changed = (c) }
#define NUMBER(t, v, m, c) { .kind = ROW_NUMBER, .label = (t), .value = (v), \
                             .count = (m) + 1, .changed = (c) }
#define ACTION(t, a)       { .kind = ROW_ACTION, .label = (t), .action = (a) }
#define INFO(t, i, a)      { .kind = ROW_INFO, .label = (t), .info = (i), .action = (a) }

static const row_t rows[] = {
    HEAD("MACHINE"),
    CHOICE("Machine",            &settings.machine,        machines,  machine_changed),
    CHOICE("Issue 2 keyboard",   &settings.issue2,         off_on,    nothing),
    CHOICE("Z80 speed",          &settings.turbo,          turbos,    turbo_changed),

    HEAD("AUDIO"),
    NUMBER("Master volume",      &settings.volume,         10,        nothing),
    NUMBER("Beeper volume",      &settings.beeper_vol,     10,        nothing),
    NUMBER("AY volume",          &settings.ay_vol,         10,        nothing),
    CHOICE("AY-3-8912 on a 48K", &settings.ay48,           off_on,    nothing),
    CHOICE("TurboSound",         &settings.turbosound,     off_on,    nothing),
    CHOICE("Covox (#FB)",        &settings.covox,          off_on,    nothing),
    CHOICE("SounDrive",          &settings.soundrive,      off_on,    nothing),

    HEAD("VIDEO"),
    CHOICE("Scanlines",          &settings.scanlines,      off_on,    nothing),

    HEAD("INPUT"),
    CHOICE("Joystick",           &settings.joystick,       joysticks, nothing),
    CHOICE("Kempston port",      &settings.kempston_port,  kports,    nothing),
    CHOICE("Cursor keys as joystick", &settings.cursor_as_joy, off_on, nothing),

    HEAD("TAPE"),
    CHOICE("Auto-start on load", &settings.tape_autostart, off_on,    nothing),
    INFO  ("Eject tape",         tape_info,  tape_action),

    HEAD("SYSTEM"),
    INFO  ("PSRAM",                     psram_info, NULL),
    ACTION("Save settings to the card", save_action),
    ACTION("Restore defaults",          defaults_action),
    ACTION("Reset the machine",         reset_action),
};

#define ROW_COUNT (sizeof rows / sizeof rows[0])

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------

#define VALUE_COL (VGA_COLS - 26)

static void value_text(const row_t *r, char *out, const unsigned len) {
    switch (r->kind) {
        case ROW_CHOICE:
            snprintf(out, len, "%s", *r->value < r->count ? r->names[*r->value] : "?");
            break;
        case ROW_NUMBER:
            snprintf(out, len, "%u", *r->value);
            break;
        case ROW_INFO:
            snprintf(out, len, "%s", r->info());
            break;
        default:
            out[0] = 0;
            break;
    }
}

/*
 * How many rows fit, and which one is at the top.
 *
 * The list is already longer than a 30-row screen and every setting
 * added makes it longer, so it scrolls rather than being trimmed to fit
 * -- the alternative is a page that silently loses its last row the next
 * time somebody adds one.
 */
#define ROWS_SHOWN (VGA_ROWS - 8)

static int top;

static void scroll_to(const int cursor) {
    if (cursor < top) top = cursor;
    if (cursor >= top + ROWS_SHOWN) top = cursor - ROWS_SHOWN + 1;

    // A heading immediately above the first visible row is worth keeping
    // on screen: without it the top of a scrolled page is a list of
    // values with nothing saying what section they belong to.
    if (top > 0 && rows[top - 1].kind == ROW_HEADING) top--;
    if (top < 0) top = 0;
}

static void draw(const int cursor) {
    scr_clear(ATTR(C_GREY, C_BLACK));
    scr_box(0, 0, VGA_COLS, VGA_ROWS, NULL, ATTR(C_BLUE, C_BLACK));

    scr_printf(2, 1, ATTR(C_YELLOW, C_BLACK), "SETTINGS");
    scr_printf(VGA_COLS - 24, 1, ATTR(C_DGREY, C_BLACK), "%s", spec_machine_name());
    for (int x = 1; x < VGA_COLS - 1; x++) scr_put(x, 2, "\xC4", ATTR(C_BLUE, C_BLACK));

    for (int i = top; i < (int)ROW_COUNT && i - top < ROWS_SHOWN; i++) {
        const row_t *r = &rows[i];
        const int y = 4 + (i - top);

        if (r->kind == ROW_HEADING) {
            scr_printf(2, y, ATTR(C_YELLOW, C_BLACK), "%s", r->label);
            continue;
        }

        const bool sel = (i == cursor);
        const uint8_t attr = sel ? ATTR(C_BLACK, C_LCYAN) : ATTR(C_WHITE, C_BLACK);

        char val[40];
        value_text(r, val, sizeof val);

        // The whole row is painted so the highlight is a bar rather than
        // the length of the label.
        char line[VGA_COLS];
        snprintf(line, sizeof line, " %-*s%s", VALUE_COL - 5, r->label, val);
        scr_printf(4, y, attr, "%-*.*s", VGA_COLS - 9, VGA_COLS - 9, line);
    }

    scr_printf(2, VGA_ROWS - 4, ATTR(C_LGREEN, C_BLACK), "%s", status);
    scr_put(2, VGA_ROWS - 2,
            "Up/Down move   Left/Right change   Enter act   S save   Tab back",
            ATTR(C_DGREY, C_BLACK));
}

// ---------------------------------------------------------------------------
// The loop
// ---------------------------------------------------------------------------

static bool selectable(const unsigned i) { return rows[i].kind != ROW_HEADING; }

static int step_cursor(int cursor, const int delta) {
    for (int i = cursor + delta; i >= 0 && i < (int)ROW_COUNT; i += delta)
        if (selectable((unsigned)i)) return i;
    return cursor;
}

static void change(const row_t *r, const int delta) {
    if (!r->value) return;
    // Wrapping, because the lists are short and stopping at the end of a
    // three-item list is an extra keystroke for nothing.
    const int n = r->count;
    int v = (int)*r->value + delta;
    if (v < 0)  v = n - 1;
    if (v >= n) v = 0;
    *r->value = (uint8_t)v;
    dirty = true;
    if (r->changed) r->changed();
}

bool options_run(void) {
    dirty = false;
    status[0] = 0;

    int cursor = 0;
    top = 0;
    if (!selectable(0)) cursor = step_cursor(0, +1);
    scroll_to(cursor);
    draw(cursor);

    for (;;) {
        const int k = menu_wait_key();

        if (k == 0x1B) {
            if (menu_wait_key() != '[') continue;
            switch (menu_wait_key()) {
                case 'A': cursor = step_cursor(cursor, -1); scroll_to(cursor); break;
                case 'B': cursor = step_cursor(cursor, +1); scroll_to(cursor); break;
                case 'C': change(&rows[cursor], +1); break;
                case 'D': change(&rows[cursor], -1); break;
                default: break;
            }
            draw(cursor);
            continue;
        }

        switch (k) {
            case '\r': case '\n':
                if (rows[cursor].action)      rows[cursor].action();
                else if (rows[cursor].value)  change(&rows[cursor], +1);
                draw(cursor);
                break;

            case '+': case '=': change(&rows[cursor], +1); draw(cursor); break;
            case '-': case '_': change(&rows[cursor], -1); draw(cursor); break;

            case 's': case 'S': save_action(); draw(cursor); break;

            case '\t': case 'q': case 'Q':
                return dirty;

            default: break;
        }
    }
}
