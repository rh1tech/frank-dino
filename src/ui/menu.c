/*
 * frank-dino — an RP2350B acting as the whole machine around a real Z80
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-dino
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "menu.h"

#include <stdio.h>
#include <string.h>

#include <pico/stdlib.h>

#include "audio.h"
#include "loader.h"
#include "options.h"
#include "settings.h"
#include "screen.h"
#include "spectrum.h"
#include "storage.h"
#include "vga.h"

// ---------------------------------------------------------------------------
// Where we are
// ---------------------------------------------------------------------------

#define MAX_ENTRIES 256
#define ROWS_SHOWN  20

static storage_entry_t entries[MAX_ENTRIES];
static int   entry_count;
static int   cursor;
static int   top;
static char  cwd[STORAGE_MAX_PATH] = "/";
static bool  open_;
static bool  loaded_something;
static char  status[VGA_COLS];

/*
 * A mailbox for keys that arrive from somewhere other than the console.
 *
 * While the menu is open it reads stdin itself, because it is modal: the
 * main loop is blocked inside menu_run() and cannot deliver anything.
 * The first version relied on it to, which deadlocked on the first
 * keypress -- the menu drew once and then waited for ever for a key that
 * only the blocked loop could have handed it.
 *
 * The mailbox survives for the USB keyboard, which arrives through a
 * TinyUSB callback rather than through stdin.
 */
static volatile int pending_key = -1;

bool menu_is_open(void) { return open_; }
void menu_key(const char c) { pending_key = (unsigned char)c; }

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------

static const char *kind_label(const file_kind_t k) {
    switch (k) {
        case FILE_DIR: return "dir";
        case FILE_TAP: return "TAP";
        case FILE_TZX: return "TZX";
        case FILE_SNA: return "SNA";
        case FILE_Z80: return "Z80";
        case FILE_TRD: return "TRD";
        case FILE_SCR: return "SCR";
        default:       return "";
    }
}

static void draw(void) {
    scr_clear(ATTR(C_GREY, C_BLACK));
    scr_box(0, 0, VGA_COLS, VGA_ROWS, NULL, ATTR(C_BLUE, C_BLACK));

    scr_printf(2, 1, ATTR(C_YELLOW, C_BLACK), "FRANK DINO  %s", FIRMWARE_VERSION);
    scr_printf(VGA_COLS - 24, 1, ATTR(C_DGREY, C_BLACK), "%s", spec_machine_name());
    scr_printf(2, 2, ATTR(C_WHITE, C_BLACK), "%.*s", VGA_COLS - 4, cwd);

    for (int x = 1; x < VGA_COLS - 1; x++) scr_put(x, 3, "\xC4", ATTR(C_BLUE, C_BLACK));

    if (entry_count <= 0) {
        scr_put(3, 5, storage_mounted() ? "(empty directory)"
                                        : "No card. Insert one and press R to retry.",
                ATTR(C_LRED, C_BLACK));
    }

    for (int i = 0; i < ROWS_SHOWN && top + i < entry_count; i++) {
        const storage_entry_t *e = &entries[top + i];
        const bool sel = (top + i == cursor);
        const uint8_t attr = sel ? ATTR(C_BLACK, C_LCYAN)
                                 : ATTR(e->kind == FILE_DIR ? C_LCYAN
                                        : e->kind == FILE_OTHER ? C_DGREY : C_WHITE, C_BLACK);

        // The whole row is painted, selected or not, so the highlight is a
        // bar rather than the length of the filename.
        char row[VGA_COLS];
        if (e->kind == FILE_DIR)
            snprintf(row, sizeof row, " %-44.44s %s", e->name, "dir");
        else
            snprintf(row, sizeof row, " %-44.44s %-3s %7lu", e->name,
                     kind_label(e->kind), (unsigned long)e->size);
        scr_printf(2, 5 + i, attr, "%-*s", VGA_COLS - 5, row);
    }

    if (entry_count > ROWS_SHOWN)
        scr_printf(VGA_COLS - 14, 3, ATTR(C_DGREY, C_BLACK), " %d/%d ",
                   cursor + 1, entry_count);

    scr_printf(2, VGA_ROWS - 4, ATTR(C_LGREEN, C_BLACK), "%s", status);
    scr_put(2, VGA_ROWS - 2,
            "Up/Down move  Enter open  Backspace up  O settings  M machine  R rescan  Tab resume",
            ATTR(C_DGREY, C_BLACK));
}

// ---------------------------------------------------------------------------
// Navigating
// ---------------------------------------------------------------------------

static void rescan(void) {
    entry_count = storage_list(cwd, entries, MAX_ENTRIES);
    if (entry_count < 0) entry_count = 0;
    cursor = 0;
    top = 0;
}

static void enter_dir(const char *name) {
    const size_t len = strlen(cwd);
    if (len + strlen(name) + 2 >= sizeof cwd) return;
    if (cwd[len - 1] != '/') strcat(cwd, "/");
    strcat(cwd, name);
    rescan();
}

static void leave_dir(void) {
    char *slash = strrchr(cwd, '/');
    if (!slash) return;
    if (slash == cwd) cwd[1] = 0;       // back to "/"
    else              *slash = 0;
    rescan();
}

static void move(const int delta) {
    if (entry_count <= 0) return;
    cursor += delta;
    if (cursor < 0) cursor = 0;
    if (cursor >= entry_count) cursor = entry_count - 1;
    if (cursor < top) top = cursor;
    if (cursor >= top + ROWS_SHOWN) top = cursor - ROWS_SHOWN + 1;
}

static void activate(void) {
    if (entry_count <= 0) return;
    const storage_entry_t *e = &entries[cursor];

    if (e->kind == FILE_DIR) { enter_dir(e->name); return; }

    // Big enough for a full path plus a full name plus the separator, so
    // the compiler can see it cannot truncate.
    char path[STORAGE_MAX_PATH + STORAGE_MAX_NAME + 2];
    snprintf(path, sizeof path, "%s%s%s", cwd, cwd[strlen(cwd) - 1] == '/' ? "" : "/", e->name);

    snprintf(status, sizeof status, "Loading %s...", e->name);
    draw();

    char detail[VGA_COLS];
    if (loader_load(path, e->kind, detail, sizeof detail)) {
        snprintf(status, sizeof status, "%s", detail);
        loaded_something = true;
    } else {
        snprintf(status, sizeof status, "%s", detail);
    }
}

// ---------------------------------------------------------------------------
// The modal loop
// ---------------------------------------------------------------------------

int menu_wait_key(void) {
    // Keep the DMA fed while waiting. The machine is paused so nothing is
    // producing samples, but the blocks still drain and still have to be
    // written, or the carrier is left playing whatever was in them.
    for (;;) {
        audio_task();

        const int k = pending_key;
        if (k >= 0) { pending_key = -1; return k; }

        const int c = getchar_timeout_us(0);
        if (c != PICO_ERROR_TIMEOUT) return c;

        sleep_ms(2);
    }
}

bool menu_run(void) {
    open_ = true;
    loaded_something = false;
    snprintf(status, sizeof status, "%s",
             storage_mounted() ? "" : "No card mounted.");

    spec_pause();
    const vga_mode_t saved = VGA_MODE_SPECTRUM;
    vga_set_mode(VGA_MODE_TEXT);

    if (!entry_count) rescan();
    draw();

    for (;;) {
        const int k = menu_wait_key();

        // Cursor keys arrive as three bytes; the same shape the Spectrum
        // key path reassembles, handled here because the menu owns the
        // console while it is open.
        if (k == 0x1B) {
            if (menu_wait_key() != '[') continue;
            switch (menu_wait_key()) {
                case 'A': move(-1); break;
                case 'B': move(+1); break;
                case 'C': move(+ROWS_SHOWN); break;
                case 'D': move(-ROWS_SHOWN); break;
                default: break;
            }
            draw();
            continue;
        }

        switch (k) {
            case '\r': case '\n': activate(); draw(); break;
            case 0x7F: case '\b': leave_dir(); draw(); break;
            case 'r': case 'R':   if (!storage_mounted()) storage_mount();
                                  rescan(); draw(); break;
            case 'o': case 'O':
                options_run();
                // The settings page may have started a different machine,
                // and it draws over this one either way.
                draw();
                break;

            case 'm': case 'M':
                // Still here because it is one keystroke, but it now goes
                // through the setting rather than around it -- otherwise
                // the machine and what the settings page says disagree.
                settings.machine = (uint8_t)((settings.machine + 1) % MACHINE_COUNT);
                spec_set_machine((machine_t)settings.machine);
                draw();
                break;
            case '\t': case 'q': case 'Q':
                vga_set_mode(saved);
                spec_resume();
                open_ = false;
                return loaded_something;
            default: break;
        }
    }
}
