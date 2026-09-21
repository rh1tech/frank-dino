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
#if USB_KEYBOARD
#include "usbkbd.h"
#endif
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
 * A small queue of keys that arrive from somewhere other than the console.
 *
 * While the menu is open it reads stdin itself, because it is modal: the
 * main loop is blocked inside menu_run() and cannot deliver anything.
 * The first version relied on it to, which deadlocked on the first
 * keypress -- the menu drew once and then waited for ever for a key that
 * only the blocked loop could have handed it.
 *
 * The queue is for the USB keyboard, which arrives through a TinyUSB
 * callback rather than through stdin. A queue and not a single slot,
 * because a cursor key is three bytes -- ESC, '[', then a letter -- and
 * a mailbox that holds one of them delivers the last and loses the shape.
 */
#define KEYQ_SIZE 16

static volatile uint8_t keyq[KEYQ_SIZE];
static volatile uint8_t keyq_head, keyq_tail;

/*
 * The settings page asking for the listing to be read again.
 *
 * "Rescan" used to be a key here. It belongs with the other card-level
 * actions, but the settings page has no idea what a listing is -- so it
 * raises this and the browser acts on it when the page closes.
 */
bool menu_rescan_wanted;

bool menu_is_open(void) { return open_; }
void menu_key(const char c) {
    const uint8_t h = keyq_head;
    if ((uint8_t)(h - keyq_tail) >= KEYQ_SIZE) return;   // full; drop it
    keyq[h % KEYQ_SIZE] = (uint8_t)c;
    keyq_head = (uint8_t)(h + 1);
}

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
        case FILE_SCL: return "SCL";
        case FILE_SCR: return "SCR";
        default:       return "";
    }
}

static void draw(void) {
    /*
     * pico-speccy's browser, in cells.
     *
     * A header strip with the rainbow and the machine at the far end, a
     * breadcrumb under it, the listing on the panel with a blue bar for
     * the selection, and a footer of key hints. See src/ui/UiRender.cpp
     * in their tree -- the layout is drawn in ASCII at the top of it.
     */
    scr_page("Files", spec_machine_name(), cwd);

    if (entry_count > ROWS_SHOWN)
        scr_printf(VGA_COLS - 14, 1, ATTR(C_TEXT_DIM, C_PANEL), "%d/%d",
                   cursor + 1, entry_count);

    if (entry_count <= 0)
        scr_put(3, 4, storage_mounted() ? "(empty directory)"
                                        : "No card. Insert one and press R to retry.",
                ATTR(C_ICON_R, C_PANEL));

    for (int i = 0; i < ROWS_SHOWN && top + i < entry_count; i++) {
        const storage_entry_t *e = &entries[top + i];
        const bool sel = (top + i == cursor);
        const int  y   = 4 + i;

        const uint8_t bg = sel ? C_SEL_BG : C_PANEL;
        const uint8_t fg = sel ? C_WHITE
                         : e->kind == FILE_DIR   ? C_ICON_C
                         : e->kind == FILE_OTHER ? C_DISABLED
                                                 : C_TEXT;

        // The whole row is painted, so the selection is a bar rather than
        // the length of a filename.
        scr_fill(1, y, VGA_COLS - 2, 1, bg);

        // Name on the left, kind and size right-aligned against the edge.
        const int name_w = VGA_COLS - 22;
        scr_printf(3, y, ATTR(fg, bg), "%-*.*s", name_w, name_w, e->name);

        const uint8_t meta = ATTR(sel ? C_WHITE : C_TEXT_DIM, bg);
        if (e->kind == FILE_DIR)
            scr_printf(VGA_COLS - 18, y, meta, "%-4s %10s", "dir", "");
        else
            scr_printf(VGA_COLS - 18, y, meta, "%-4s %10lu",
                       kind_label(e->kind), (unsigned long)e->size);
    }

    if (status[0])
        scr_printf(2, VGA_ROWS - 3, ATTR(C_ACCENT, C_PANEL), "%.*s", VGA_COLS - 4, status);

    scr_footer("Up/Down  Enter open  Backspace up  A-Z jump  F12 settings  F11 resume");
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

/*
 * Jump to the next entry starting with a character.
 *
 * A directory on this card has two hundred and fifty-six entries in it
 * and the arrow keys move one at a time, so typing the first letter is
 * not a convenience, it is the difference between the browser being
 * usable and not.
 *
 * It starts from the entry after the cursor and wraps, so pressing the
 * same letter again walks through everything that begins with it.
 *
 * This is why the browser's own commands are not letters any more: every
 * letter and every digit belongs to the files.
 */
static void jump_to(const char c) {
    if (entry_count <= 0) return;

    const char want = (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;

    for (int n = 1; n <= entry_count; n++) {
        const int i = (cursor + n) % entry_count;
        char first = entries[i].name[0];
        if (first >= 'a' && first <= 'z') first = (char)(first - 'a' + 'A');
        if (first != want) continue;

        cursor = i;
        if (cursor < top) top = cursor;
        if (cursor >= top + ROWS_SHOWN) top = cursor - ROWS_SHOWN + 1;
        return;
    }
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

#if USB_KEYBOARD
        /*
         * And keep the host stack running.
         *
         * This is modal: the main loop is inside it, so nothing else is
         * calling tuh_task(). Without this the keyboard's reports stop
         * being collected the moment the menu opens, and the menu waits
         * for ever for a key it has made itself unable to receive.
         */
        usbkbd_task();
#endif

        if (keyq_tail != keyq_head) {
            const uint8_t k = keyq[keyq_tail % KEYQ_SIZE];
            keyq_tail = (uint8_t)(keyq_tail + 1);
            return k;
        }

        const int c = getchar_timeout_us(0);
        if (c != PICO_ERROR_TIMEOUT) return c;

        sleep_ms(2);
    }
}

/*
 * Open straight onto the settings page, for F12.
 *
 * The browser is the way in and the settings hang off it, so "go to the
 * settings" is "open the browser and immediately open the page" rather
 * than a second modal thing with its own key loop.
 */
static bool straight_to_settings;

void menu_open_settings(const bool yes) { straight_to_settings = yes; }

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

    if (straight_to_settings) {
        straight_to_settings = false;
        options_run();
        if (menu_rescan_wanted) { menu_rescan_wanted = false; rescan(); }
        draw();
    }

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

            case '\\':
                // The one command key that is not a letter, because every
                // letter is a jump. The settings page carries what used to
                // be M and R.
                options_run();
                if (menu_rescan_wanted) { menu_rescan_wanted = false; rescan(); }
                draw();
                break;

            case '\t':
                vga_set_mode(saved);
                spec_resume();
                open_ = false;
                return loaded_something;

            default:
                if (k >= 0x20 && k < 0x7F) { jump_to((char)k); draw(); }
                break;
        }
    }
}
