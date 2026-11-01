/*
 * frank-dino — an RP2350B acting as the whole machine around a real Z80
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-dino
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * pico-speccy's file browser, from its src/ui/UiBrowser.cpp.
 *
 *  +-----------------------------------------------------+
 *  | [rainbow] Files                              7/31   |  header + position
 *  |-----------------------------------------------------|
 *  | /games/arcade                                       |  path bar
 *  |----------------------------------------+------------|
 *  | ..                                   > | MANIC.TAP  |  list      info pane
 *  | MANIC.TAP                              | Tape image |
 *  | JETPAC.Z80                             | 48 KB      |
 *  |                                        | --------   |
 *  |                                        | ^ Run      |
 *  |----------------------------------------+------------|
 *  | ^v Move  ^ Open  < Up  F3 Find  Esc Close           |  footer
 *  +-----------------------------------------------------+
 *
 * The drawing is theirs. The model underneath is this firmware's
 * storage_list(), which is an array and not their .idx cache, so their
 * index, their per-type cursor memory and their remote back ends have
 * nothing here to attach to and did not come.
 */

#include "ui_browser.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "loader.h"
#include "menu.h"
#include "psram.h"
#include "spectrum.h"
#include "storage.h"
#include "usbdisk.h"
#include "beta.h"
#include "fdc.h"
#include "ui_dialog.h"
#include "zip.h"
#include "ui_gfx.h"
#include "ui_nav.h"
#include "ui_render.h"
#include "vga.h"

/* ── where we are ────────────────────────────────────────────────────── */

/*
 * The listing, over in the PSRAM.
 *
 * Eighteen kilobytes of directory entries in a 512 KB part that is 93 %
 * spoken for, read only with the Z80 stopped and never on the bus
 * engine's path -- which is the whole test for what belongs over there.
 */
#define MAX_ENTRIES 256
static storage_entry_t entries[MAX_ENTRIES] __attribute__((section(".psram")));

static int  entry_count;
static int  sel, top;                   /* selection and first visible row */
static char cwd[STORAGE_MAX_PATH] = "/";
static char status[64];
static bool loaded_something;

/* Incremental search: their F3, with the query in the footer. */
static bool searching;
static char query[32];

/* ── layout ──────────────────────────────────────────────────────────── */

/*
 * Theirs, from computeBL(). The one number that differs is the info
 * pane's width, and only because their `cols() >= 52` test lands on the
 * other side of this surface's 53.
 */
static struct {
    int pad, margin, ix, iy, iw, ih;
    int hdr_h, path_h, foot_h, row_h;
    int body_y, body_h, rows;
    int lx, lw, sep_x, rx, rw;
} L;

static void compute_layout(void) {
    L.pad    = 2;
    L.margin = 6;                       /* the menu's grey border, see ui_render.c */
    L.ix = L.margin + 1;
    L.iy = L.margin + 1;
    L.iw = UI_SURF_W - 2 * L.ix;
    L.ih = UI_SURF_H - 2 * L.iy;

    L.hdr_h  = UI_FONT_H + 4;
    L.path_h = UI_FONT_H + 4;
    L.foot_h = UI_FONT_H + 4;
    L.row_h  = UI_FONT_H + 2;

    L.body_y = L.iy + L.hdr_h + L.path_h;
    L.body_h = L.iy + L.ih - L.foot_h - L.body_y;
    L.rows   = L.body_h / L.row_h;

    /* Info pane: a fixed character budget; the list takes the rest. */
    const int ichars = 15;
    L.rw    = ichars * ui_glyph_w() + 2 * L.pad;
    L.rx    = L.ix + L.iw - L.rw;
    L.sep_x = L.rx - 1;
    L.lx    = L.ix;
    L.lw    = L.sep_x - L.lx;
}

/* ── the filter ──────────────────────────────────────────────────────── */

/*
 * Directories always pass; files pass on a case-insensitive substring
 * match. Same semantics as their nameMatches(), over this array instead
 * of their index -- so `vis_to_idx` is the equivalent of their visToIdx
 * and every cursor number below is in the visible domain.
 */
static bool name_matches(const storage_entry_t *e) {
    if (!query[0] || e->kind == FILE_DIR) return true;
    const size_t qn = strlen(query);
    for (const char *p = e->name; *p; p++) {
        size_t i = 0;
        while (i < qn && p[i] && toupper((uint8_t)p[i]) == toupper((uint8_t)query[i])) i++;
        if (i == qn) return true;
    }
    return false;
}

static int vis_total(void) {
    if (!query[0]) return entry_count;
    int n = 0;
    for (int i = 0; i < entry_count; i++) if (name_matches(&entries[i])) n++;
    return n;
}

static int vis_to_idx(const int v) {
    if (!query[0]) return (v >= 0 && v < entry_count) ? v : -1;
    int n = 0;
    for (int i = 0; i < entry_count; i++)
        if (name_matches(&entries[i]) && n++ == v) return i;
    return -1;
}

static const storage_entry_t *vis_entry(const int v) {
    const int i = vis_to_idx(v);
    return i < 0 ? NULL : &entries[i];
}

/* ── what a file is ──────────────────────────────────────────────────── */

/* Theirs, cut down to the formats this firmware can actually open. */
static const char *type_label(const storage_entry_t *e) {
    switch (e->kind) {
        case FILE_DIR: return "Folder";
        case FILE_TAP:
        case FILE_TZX:
        case FILE_PZX:
        case FILE_CSW:
        case FILE_WAV: return "Tape image";
        case FILE_GZ:  return "Gzipped file";
        case FILE_ROM: return "ROM cartridge";
        case FILE_SNA:
        case FILE_Z80: return "Snapshot";
        case FILE_TRD:
        case FILE_SCL:
        case FILE_FDI:
        case FILE_UDI:
        case FILE_TD0: return "TR-DOS disk";
        case FILE_DSK: return "+3 disk";
        case FILE_DCK: return "Timex cartridge";
        case FILE_MMC: return "esxDOS card";
        case FILE_SCR: return "ZX screen";
        case FILE_ZIP: return "ZIP archive";
        default:       return "File";
    }
}

static bool runnable(const storage_entry_t *e) {
    return e->kind != FILE_OTHER;
}

/* ── drawing ─────────────────────────────────────────────────────────── */

static void draw_chrome(void) {
    ui_fill(0, 0, UI_SURF_W, UI_SURF_H, C_BG);
    ui_round_rect(L.margin, L.iy - 1, UI_SURF_W - 2 * L.margin, L.ih + 2, 4,
                  C_SEP, C_PANEL);
    ui_vline(L.sep_x, L.body_y, L.body_h, C_SEP);
}

static void draw_header(void) {
    char cnt[28];
    const int n = vis_total();
    if (n > 0) snprintf(cnt, sizeof cnt, "%d/%d", sel + 1, n);
    else       snprintf(cnt, sizeof cnt, "empty");
    ui_title_bar(L.ix, L.iy, L.iw, L.hdr_h, "Files", cnt);
}

static void draw_path_bar(void) {
    const int y = L.iy + L.hdr_h;
    ui_fill(L.ix, y, L.iw, L.path_h, C_PANEL_ALT);

    /* Long paths keep their tail: the deepest directories are what
     * orients the user, not the volume root they all share. */
    const int fits = (L.iw - 2 * L.pad) / ui_glyph_w();
    const int len = (int)strlen(cwd);
    if (len > fits && fits > 2) {
        char buf[STORAGE_MAX_PATH + 4];
        snprintf(buf, sizeof buf, "..%s", cwd + len - fits + 2);
        ui_text(L.ix + L.pad, y + 2, buf, C_TEXT);
    } else {
        ui_text(L.ix + L.pad, y + 2, cwd, C_TEXT);
    }
    ui_hline(L.ix, y + L.path_h - 1, L.iw, C_SEP);
}

static void draw_list_row(const int vis_row) {
    if (vis_row < 0 || vis_row >= L.rows) return;
    const int y = L.body_y + vis_row * L.row_h;
    const int v = top + vis_row;
    const int n = vis_total();
    const bool is_sel = (v == sel) && (v < n);

    /* Room for the scrollbar hairline at the right edge of the pane. */
    const int sb = (n > L.rows) ? 2 : 0;
    ui_fill(L.lx, y, L.lw - sb, L.row_h, is_sel ? C_SEL_BG : C_PANEL);
    if (v >= n) return;

    const storage_entry_t *e = vis_entry(v);
    if (!e) return;
    const bool dir = (e->kind == FILE_DIR);

    ui_color_t ink;
    if (is_sel)   ink = C_WHITE;
    else if (dir) ink = C_TEXT;
    else          ink = runnable(e) ? C_TEXT : C_TEXT_DIM;

    const int mark_w = dir ? (ui_chevron_w() + L.pad) : 0;
    ui_text_clip(L.lx + L.pad, y + 1, L.lw - sb - 2 * L.pad - mark_w, e->name, ink);
    if (dir)
        ui_chevron(L.lx + L.lw - sb - L.pad - ui_chevron_w(), y + L.row_h / 2 - 3,
                   is_sel ? C_WHITE : C_DISABLED);
}

static void draw_scrollbar(void) {
    const int n = vis_total();
    if (n <= L.rows) return;
    const int x = L.sep_x - 2;
    ui_fill(x, L.body_y, 1, L.body_h, C_PANEL_ALT);
    int th = L.body_h * L.rows / n;
    if (th < 6) th = 6;
    const int span = L.body_h - th;
    const int max_top = n - L.rows;
    const int ty = L.body_y + (max_top > 0 ? span * top / max_top : 0);
    ui_fill(x, ty, 1, th, C_DISABLED);
}

static void draw_list(void) {
    for (int r = 0; r < L.rows; r++) draw_list_row(r);
    draw_scrollbar();
}

/*
 * The pane is cleared just ahead of the pen rather than all at once: a
 * pane blanked whole and then written shows, for a frame, as the blank --
 * which on every cursor move was a flicker.
 */
static int info_clean_y;

static void info_clear_to(const int y_end) {
    const int end = y_end < L.body_y + L.body_h ? y_end : L.body_y + L.body_h;
    if (end > info_clean_y) ui_fill(L.rx, info_clean_y, L.rw, end - info_clean_y, C_PANEL);
    if (end > info_clean_y) info_clean_y = end;
}

/* The info pane: everything known about the focused entry, then the verbs. */
static void draw_info_body(void);

static void draw_info(void) {
    info_clean_y = L.body_y;
    draw_info_body();
    info_clear_to(L.body_y + L.body_h);         /* whatever the last entry left below */
}

static void draw_info_body(void) {
    const int x = L.rx, w = L.rw;
    int y = L.body_y + 2;
    const int lh = UI_FONT_H + 2;
    const int tx = x + L.pad, tw = w - 2 * L.pad;
    const int fits = tw / ui_glyph_w();

    const storage_entry_t *e = vis_entry(sel);
    if (!e) {
        info_clear_to(y + UI_FONT_H + 2);
        ui_text(tx, y, query[0] ? "No match" : "No files", C_TEXT_DIM);
        return;
    }

    /* Full name, wrapped over up to 3 lines (the list may have clipped it). */
    const int len = (int)strlen(e->name);
    for (int ln = 0; ln < 3 && ln * fits < len; ln++) {
        char buf[64];
        snprintf(buf, sizeof buf, "%.*s", fits, e->name + ln * fits);
        info_clear_to(y + lh);
        ui_text(tx, y, buf, C_WHITE);
        y += lh;
    }
    y += 2;

    if (e->kind == FILE_DIR) {
        info_clear_to(y + lh);
        ui_text(tx, y, "Folder", C_TEXT_DIM);
        y += lh;
    } else {
        info_clear_to(y + lh);
        ui_text(tx, y, type_label(e), runnable(e) ? C_ACCENT : C_TEXT_DIM);
        y += lh;
        char buf[24];
        if (e->size < 10000u)              snprintf(buf, sizeof buf, "%u bytes", (unsigned)e->size);
        else if (e->size < 10u << 20)      snprintf(buf, sizeof buf, "%u KB", (unsigned)(e->size >> 10));
        else                               snprintf(buf, sizeof buf, "%u MB", (unsigned)(e->size >> 20));
        info_clear_to(y + lh);
        ui_text(tx, y, buf, C_TEXT);
        y += lh;
    }
    y += 3;

    /* Verbs from the bottom up would jump around; a fixed block reads better. */
    info_clear_to(y + 3);
    ui_hline(tx, y, tw, C_SEP);
    y += 3;
    const bool dir = (e->kind == FILE_DIR);
    const struct { const char *k, *what; bool on; } verbs[] = {
        { SYM_ENTER, dir ? "Open" : "Run", dir || runnable(e) },
        { SYM_LEFT,  "Up",       strcmp(cwd, "/") != 0 },
        { "F3",      "Find",     true },
        { "F12",     "Settings", true },
    };
    for (unsigned i = 0; i < sizeof verbs / sizeof verbs[0]; i++) {
        if (!verbs[i].on) continue;
        if (y + lh > L.body_y + L.body_h) break;
        const int kw = ui_text_width(verbs[i].k);
        info_clear_to(y + lh);
        ui_text(tx, y, verbs[i].k, C_TEXT_DIM);
        ui_text(tx + kw + ui_glyph_w(), y, verbs[i].what, C_TEXT);
        y += lh;
    }
}

static void draw_footer(void) {
    const int y = L.iy + L.ih - L.foot_h;
    ui_fill(L.ix, y, L.iw, L.foot_h, C_FOOT_BG);
    ui_hline(L.ix, y, L.iw, C_SEP);

    if (searching) {
        const int lx = L.ix + L.pad;
        ui_text(lx, y + 3, "Find:", C_ACCENT);
        const int qx = lx + 6 * ui_glyph_w();
        char buf[sizeof query + 2];
        snprintf(buf, sizeof buf, "%s_", query);
        ui_text_clip(qx, y + 3, L.ix + L.iw - qx - L.pad, buf, C_WHITE);
        return;
    }

    if (status[0]) {
        ui_text_clip(L.ix + L.pad, y + 3, L.iw - 2 * L.pad, status, C_ACCENT);
        return;
    }
    ui_text(L.ix + L.pad, y + 3,
            SYM_UP SYM_DOWN " Move  " SYM_ENTER " Open  " SYM_LEFT " Up  F3 Find  Esc Close",
            C_TEXT);
}

/* Bands square the window corners off — the border re-pass restores them. */
static void frame_fix(void) {
    ui_round_rect_border(L.margin, L.iy - 1, UI_SURF_W - 2 * L.margin, L.ih + 2, 4,
                         C_SEP, C_BG);
}

/* Everything but the screen clear: what changes when the view does. */
static void draw_view(void) {
    draw_header();
    draw_path_bar();
    draw_list();
    draw_info();
    draw_footer();
    frame_fix();
}

static void draw_all(void) {
    draw_chrome();
    draw_header();
    draw_path_bar();
    draw_list();
    draw_info();
    draw_footer();
    frame_fix();
}

/* ── navigation ──────────────────────────────────────────────────────── */

static void clamp_view(void) {
    const int n = vis_total();
    if (sel >= n) sel = n ? n - 1 : 0;
    if (sel < 0) sel = 0;
    if (sel < top) top = sel;
    if (sel >= top + L.rows) top = sel - L.rows + 1;
    if (top > n - L.rows) top = n - L.rows;
    if (top < 0) top = 0;
}

/*
 * True while we are looking at the USB stick rather than the card.
 *
 * FatFs names volumes by a prefix, so this is just what `cwd` starts
 * with -- there is no second path variable to keep in step.
 */
static bool on_usb(void) { return cwd[0] == '1' && cwd[1] == ':'; }

static bool at_volume_root(void) {
    return !strcmp(cwd, "/") || !strcmp(cwd, "1:/");
}

void ui_browser_rescan(void) {
    entry_count = storage_list(cwd, entries, MAX_ENTRIES);
    if (entry_count < 0) entry_count = 0;

    /*
     * The other volume, as a row at the top of this one's root.
     *
     * Their browser has a whole "Open from" level above the volumes for
     * this; with two drives and no network, a row is the same idea
     * without a level to walk through. It only appears where it means
     * something -- at a root, and only while a stick is actually there.
     */
    if (at_volume_root() && (usbdisk_present() || on_usb())
        && entry_count < MAX_ENTRIES) {
        memmove(&entries[1], &entries[0],
                (size_t)entry_count * sizeof entries[0]);
        snprintf(entries[0].name, sizeof entries[0].name, "%s",
                 on_usb() ? "microSD card" : "USB stick");
        entries[0].size = 0;
        entries[0].kind = FILE_DIR;
        entry_count++;
    }

    sel = 0;
    top = 0;
}

/* Is this row the synthetic "other volume" one? */
static bool is_volume_row(const int v) {
    const storage_entry_t *e = vis_entry(v);
    return at_volume_root() && e && e == &entries[0]
        && (!strcmp(e->name, "USB stick") || !strcmp(e->name, "microSD card"));
}

static void move_sel(const int delta) {
    const int n = vis_total();
    if (n <= 0) return;
    sel += delta;
    clamp_view();
}

static void jump_to(const char c) {
    const int n = vis_total();
    if (n <= 0) return;
    const char want = (char)toupper((uint8_t)c);
    for (int k = 1; k <= n; k++) {
        const int v = (sel + k) % n;
        const storage_entry_t *e = vis_entry(v);
        if (e && toupper((uint8_t)e->name[0]) == want) { sel = v; clamp_view(); return; }
    }
}

static void enter_dir(const char *name) {
    const size_t len = strlen(cwd);
    if (len + strlen(name) + 2 >= sizeof cwd) return;
    if (cwd[len - 1] != '/') strcat(cwd, "/");
    strcat(cwd, name);
    query[0] = 0;
    ui_browser_rescan();
}

static void leave_dir(void) {
    if (at_volume_root()) return;       /* nothing above a volume's root */

    char *slash = strrchr(cwd, '/');
    if (!slash) return;
    /*
     * "1:/games" leaves a "1:" that FatFs reads as the volume's current
     * directory rather than its root, so the root keeps its slash.
     */
    if (slash == cwd)          cwd[1] = 0;
    else if (slash == cwd + 2 && on_usb()) cwd[3] = 0;
    else                       *slash = 0;
    query[0] = 0;
    ui_browser_rescan();
}

/*
 * The members of an archive, over in the PSRAM with the listing.
 *
 * A zip of a demo can hold every part of it; theirs takes the first
 * member it can open, which is right when there is one and a guess when
 * there are twelve. This lists them and asks -- and when there is only
 * one, it does not.
 */
static zip_member_t members[ZIP_MAX_MEMBERS] __attribute__((section(".psram")));

static void member_row(const int i, char *out, const unsigned outsz) {
    const uint32_t kb = (members[i].size + 1023u) >> 10;
    snprintf(out, outsz, "%-28s %4lu KB", members[i].name, (unsigned long)kb);
}

/* Unpack the chosen member to a temp file and load it as itself. */
static void open_archive(const char *path) {
    const int n = zip_list(path, members, ZIP_MAX_MEMBERS);
    if (n < 0)  { snprintf(status, sizeof status, "Not a readable archive."); return; }
    if (n == 0) { snprintf(status, sizeof status, "Nothing in it this can open."); return; }

    int pick = 0;
    if (n > 1) {
        pick = ui_pick_list("Open from archive", n, member_row, 0, 34,
                            SYM_ENTER " Open   Esc Cancel");
        draw_all();
        if (pick < 0) return;
    }

    char detail[64];
    if (!zip_extract(path, &members[pick], detail, sizeof detail)) {
        snprintf(status, sizeof status, "%s", detail);
        return;
    }
    if (loader_load(ZIP_TEMP_PATH, members[pick].kind, detail, sizeof detail))
        loaded_something = true;
    snprintf(status, sizeof status, "%s", detail);
}

/*
 * A disk with unsaved writes is about to be replaced.
 *
 * Loading anything at all can do it -- a .trd replaces the Beta Disk's
 * image, a .dsk the +3's, and a snapshot or a tape resets the machine
 * out from under both. So the question is asked here, once, before any
 * of that starts rather than in each of the loaders.
 *
 * Returns false if the user would rather not.
 */
static bool ok_to_lose_disk(void) {
    const bool beta = beta_dirty(), plus3 = fdc_dirty();
    if (!beta && !plus3) return true;

    const char *btns[3] = { "Save first", "Discard", "Cancel" };
    const int c = ui_choice(beta && plus3 ? "Both disks have been written to."
                                          : "The disk has been written to.",
                            btns, 3, 0, 2);
    draw_all();
    if (c == 2 || c < 0) return false;
    if (c == 0) {
        char d[sizeof status];
        if (beta  && !beta_save(d, sizeof d)) { snprintf(status, sizeof status, "%s", d); return false; }
        if (plus3 && !fdc_save(d, sizeof d))  { snprintf(status, sizeof status, "%s", d); return false; }
    }
    return true;
}

static void activate(void) {
    const storage_entry_t *e = vis_entry(sel);
    if (!e) return;

    if (is_volume_row(sel)) {
        snprintf(cwd, sizeof cwd, "%s", on_usb() ? "/" : "1:/");
        query[0] = 0;
        ui_browser_rescan();
        return;
    }

    if (e->kind == FILE_DIR) { enter_dir(e->name); return; }
    if (!runnable(e)) { snprintf(status, sizeof status, "Nothing here opens that."); return; }

    char path[STORAGE_MAX_PATH + STORAGE_MAX_NAME + 2];
    snprintf(path, sizeof path, "%s%s%s", cwd,
             cwd[strlen(cwd) - 1] == '/' ? "" : "/", e->name);

    if (!ok_to_lose_disk()) { snprintf(status, sizeof status, "Left alone."); return; }

    if (e->kind == FILE_ZIP) {
        snprintf(status, sizeof status, "Reading %s...", e->name);
        draw_footer();
        frame_fix();
        open_archive(path);
        return;
    }

    if (e->kind == FILE_GZ) {
        snprintf(status, sizeof status, "Unpacking %s...", e->name);
        draw_footer();
        frame_fix();
        char detail[64];
        file_kind_t inner;
        if (zip_gunzip(path, &inner, detail, sizeof detail)
            && loader_load(ZIP_TEMP_PATH, inner, detail, sizeof detail))
            loaded_something = true;
        snprintf(status, sizeof status, "%s", detail);
        return;
    }

    snprintf(status, sizeof status, "Loading %s...", e->name);
    draw_footer();
    frame_fix();

    char detail[64];
    if (loader_load(path, e->kind, detail, sizeof detail)) loaded_something = true;
    snprintf(status, sizeof status, "%s", detail);
}

/* ── keys ────────────────────────────────────────────────────────────── */

typedef enum {
    BK_NONE = 0, BK_UP, BK_DOWN, BK_LEFT, BK_RIGHT, BK_ENTER, BK_ESC,
    BK_PGUP, BK_PGDN, BK_HOME, BK_END, BK_BACK, BK_CLOSE, BK_SETTINGS,
    BK_FIND, BK_CHAR
} bkey_t;

/*
 * The continuation of an escape sequence is read with a timeout, for the
 * same reason the settings menu reads it that way: Escape is a key here
 * too, and waiting for ever to find out whether a '[' is coming would
 * hang on every press of it.
 */
#define ESC_CONT_MS 20

static bkey_t next_key(char *ch) {
    for (;;) {
        const int c = menu_wait_key();
        if (c < 0) continue;

        if (c == 0x1B) {
            const int b = menu_wait_key_timeout(ESC_CONT_MS);
            if (b != '[' && b != 'O') return BK_ESC;
            switch (menu_wait_key_timeout(ESC_CONT_MS)) {
                case 'A': return BK_UP;
                case 'B': return BK_DOWN;
                case 'C': return BK_RIGHT;
                case 'D': return BK_LEFT;
                case 'H': return BK_HOME;
                case 'F': return BK_END;
                case '5': menu_wait_key_timeout(ESC_CONT_MS); return BK_PGUP;
                case '6': menu_wait_key_timeout(ESC_CONT_MS); return BK_PGDN;
                default:  continue;
            }
        }

        switch (c) {
            case '\r':
            case '\n': return BK_ENTER;
            case 0x7F:
            case '\b': return BK_BACK;
            case '\t': return BK_CLOSE;         /* F11 / Tab */
            case '\\': return BK_SETTINGS;      /* F12 */
            case 0x06: return BK_FIND;          /* F3, as usbkbd sends it */
            default:
                if (c >= ' ' && c < 0x7F) { *ch = (char)c; return BK_CHAR; }
                continue;
        }
    }
}

/* ── the session ─────────────────────────────────────────────────────── */

bool ui_browser_run(void) {
    compute_layout();
    loaded_something = false;
    searching = false;
    query[0] = 0;
    snprintf(status, sizeof status, "%s", storage_mounted() ? "" : "No card mounted.");

    if (!entry_count) ui_browser_rescan();
    clamp_view();
    draw_all();

    const vga_mode_t was = vga_get_mode();
    vga_set_mode(VGA_MODE_UI);

    for (;;) {
        char ch = 0;
        const bkey_t k = next_key(&ch);

        /*
         * Find mode owns the printable keys while it is up: a letter
         * extends the query rather than jumping, which is the one thing
         * the two gestures cannot both have.
         */
        if (searching) {
            switch (k) {
                case BK_CHAR: {
                    const size_t n = strlen(query);
                    if (n + 1 < sizeof query) { query[n] = ch; query[n + 1] = 0; }
                    sel = top = 0;
                    break;
                }
                case BK_BACK: {
                    const size_t n = strlen(query);
                    if (n) query[n - 1] = 0;
                    else   searching = false;
                    sel = top = 0;
                    break;
                }
                case BK_ESC:
                    searching = false;
                    query[0] = 0;
                    sel = top = 0;
                    break;
                case BK_ENTER:
                    searching = false;      /* keep the filter, leave the prompt */
                    break;
                case BK_UP:   move_sel(-1); break;
                case BK_DOWN: move_sel(+1); break;
                default: break;
            }
            clamp_view();
            draw_view();
            continue;
        }

        /*
         * Redraw what moved. The cursor going down a page that does not
         * scroll is two rows and the panes beside them; anything else is
         * the view, still without clearing the screen under it.
         */
        const int was_sel = sel, was_top = top;
        bool whole = false;

        switch (k) {
            case BK_UP:   move_sel(-1); break;
            case BK_DOWN: move_sel(+1); break;
            case BK_PGUP: move_sel(-L.rows); break;
            case BK_PGDN: move_sel(+L.rows); break;
            case BK_HOME: move_sel(-MAX_ENTRIES); break;
            case BK_END:  move_sel(+MAX_ENTRIES); break;
            case BK_RIGHT:
            case BK_ENTER: status[0] = 0; activate(); whole = true; break;
            case BK_LEFT:
            case BK_BACK:  status[0] = 0; leave_dir(); whole = true; break;
            case BK_FIND:  searching = true; query[0] = 0; sel = top = 0; whole = true; break;

            case BK_SETTINGS:
                ui_menu_run();
                if (menu_rescan_wanted) { menu_rescan_wanted = false; ui_browser_rescan(); }
                vga_set_mode(VGA_MODE_UI);
                clamp_view();
                draw_all();             /* the settings drew over all of it */
                continue;

            case BK_ESC:
            case BK_CLOSE:
                vga_set_mode(was);
                return loaded_something;

            case BK_CHAR:
                if (ch != ' ') jump_to(ch);
                break;

            default: break;
        }
        clamp_view();
        if (whole || top != was_top) {
            draw_view();
        } else if (sel != was_sel) {
            draw_list_row(was_sel - top);
            draw_list_row(sel - top);
            draw_header();
            draw_info();
            draw_footer();
            frame_fix();
        }
    }
}
