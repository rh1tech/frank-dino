/*
 * frank-dino — an RP2350B acting as the whole machine around a real Z80
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * pico-speccy's menu navigation, from the portable half of its
 * src/ui/UiNav.cpp (GPL-3.0-or-later): buildVisible, the two move
 * functions, enterLevel/leaveLevel, refreshRightPane, activate, stepInt,
 * the first-letter jump and handleKey, all of which are about the model
 * and not about the machine underneath it.
 *
 * Their session plumbing is not here. The row pool is static rather than
 * heap (this firmware has no heap worth the name, and 6 KB of BSS is
 * cheaper than the allocator); the staging commit dialog is gone with
 * the staging; and the key decoder reads this board's USB keyboard
 * instead of fabgl.
 */

#include "ui_nav.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "ui_render.h"
#include "psram.h"
#include "menu.h"
#include "vga.h"

ui_state_t ui_S;

/* The dynamic-row pool. ~5 KB, and the only thing in this UI that is big. */
static ui_dynrows_t dyn_pool __attribute__((section(".psram")));

const ui_option_t ui_opt_offon[2] = { { "Off", 0, NULL }, { "On", 1, NULL } };

/* What the right pane offers on a dynamic level, when the level names nothing. */
static const char *const kDynHints[] = { SYM_ENTER " Select" };

void ui_dyn_clear(ui_dynrows_t *d) { d->n = 0; d->focus_set = false; }

void ui_dyn_add(ui_dynrows_t *d, const char *lbl, const char *val,
                const int32_t tag, const bool dimmed) {
    if (d->n >= UI_DYN_MAX_ROWS) return;
    snprintf(d->label[d->n], UI_DYN_LABEL_LEN, "%s", lbl ? lbl : "");
    snprintf(d->value[d->n], UI_DYN_VALUE_LEN, "%s", val ? val : "");
    d->tag[d->n] = tag;
    d->dim[d->n] = dimmed ? 1 : 0;
    d->n++;
}

void ui_dyn_focus_tag(ui_dynrows_t *d, const int32_t tag) {
    d->focus_tag = tag;
    d->focus_set = true;
}

void ui_request_close(void) { ui_S.quit = true; }

/* ── level helpers ───────────────────────────────────────────────────── */

void ui_build_visible(ui_level_t *L) {
    L->nvis = 0;
    if (L->dyn) {
        /* The pool is already exactly what should be shown; dimmed rows stay
         * in the list (they carry information) and are refused at activation. */
        for (uint8_t i = 0; i < ui_S.dyn->n && L->nvis < UI_MAX_ROWS; i++)
            L->vis[L->nvis++] = i;
    } else {
        for (uint8_t i = 0; i < L->count && L->nvis < UI_MAX_ROWS; i++)
            if (!L->nodes[i].visible || L->nodes[i].visible())
                L->vis[L->nvis++] = i;
    }
    if (L->sel >= L->nvis) L->sel = L->nvis ? L->nvis - 1 : 0;
    if (L->top > L->sel)   L->top = L->sel;
    if (L->sel >= L->top + LY.body_rows)
        L->top = L->sel - LY.body_rows + 1;
}

/* Hidden children must not appear in the right-pane preview either. */
const ui_node_t *ui_sub_visible_child(const ui_node_t *n, const int i) {
    if (!n || n->kind != K_SUB) return NULL;
    int seen = 0;
    for (uint8_t k = 0; k < n->count; k++) {
        const ui_node_t *c = &n->kids[k];
        if (c->visible && !c->visible()) continue;
        if (seen == i) return c;
        seen++;
    }
    return NULL;
}

int ui_right_row_count(const ui_node_t *n) {
    if (!n) return 0;
    switch (n->kind) {
        case K_RADIO:
        case K_BOOL: { uint8_t c; ui_node_options(n, &c); return c; }
        case K_SUB: {
            int v = 0;
            for (uint8_t k = 0; k < n->count; k++)
                if (!n->kids[k].visible || n->kids[k].visible()) v++;
            return v;
        }
        default: return 1;
    }
}

const char *ui_right_title(const ui_node_t *n) { return n ? n->label : ""; }

const char *ui_dyn_hint(const int i) {
    const ui_node_t *o = ui_dyn_owner();
    if (o && o->opts && o->count)
        return (i >= 0 && i < (int)o->count) ? o->opts[i].label : NULL;
    const int n = (int)(sizeof kDynHints / sizeof kDynHints[0]);
    return (i >= 0 && i < n) ? kDynHints[i] : NULL;
}

static bool has_value_pane(const ui_node_t *n) {
    /* "the right pane is a list you move through" */
    return n && (n->kind == K_RADIO || n->kind == K_BOOL);
}

static bool is_int_node(const ui_node_t *n) { return n && n->kind == K_INT; }

static void refresh_right_pane(void) {
    if (ui_cur_level()->dyn) {
        const ui_node_t *o = ui_dyn_owner();
        ui_S.rcount = (o && o->opts && o->count)
                          ? o->count
                          : (uint8_t)(sizeof kDynHints / sizeof kDynHints[0]);
        ui_S.rtop = ui_S.rsel = 0;
        return;
    }
    const ui_node_t *n = ui_cur_node();
    if (n && !ui_node_enabled(n)) {         /* greyed row: nothing to interact with */
        ui_S.rcount = 0;
        ui_S.rtop = ui_S.rsel = 0;
        return;
    }
    ui_S.rcount = (uint8_t)ui_right_row_count(n);
    ui_S.rtop = 0;
    ui_S.rsel = 0;
    if (has_value_pane(n)) {                /* land on the currently selected option */
        const int32_t v = ui_node_value(n);
        uint8_t cnt;
        const ui_option_t *o = ui_node_options(n, &cnt);
        for (uint8_t i = 0; i < cnt; i++)
            if (o[i].value == v) { ui_S.rsel = i; break; }
        if (ui_S.rsel >= LY.body_rows) ui_S.rtop = ui_S.rsel - LY.body_rows + 1;
    }
}

/* wrap: single-step moves roll over top<->bottom; paging keeps clamping. */
static void move_left(const int delta, const bool wrap) {
    ui_level_t *L = ui_cur_level();
    if (!L->nvis) return;
    const int old = L->sel, old_top = L->top;
    int s = L->sel + delta;
    if (wrap) s = (s < 0) ? L->nvis - 1 : (s > L->nvis - 1) ? 0 : s;
    if (s < 0) s = 0;
    if (s > L->nvis - 1) s = L->nvis - 1;
    L->sel = (uint8_t)s;
    if (L->sel < L->top) L->top = L->sel;
    if (L->sel >= L->top + LY.body_rows) L->top = L->sel - LY.body_rows + 1;

    if (L->sel == old && L->top == old_top) return;
    if (L->top != old_top) ui_mark_dirty(D_LEFT);
    else { ui_mark_left_row(old - L->top); ui_mark_left_row(L->sel - L->top); }
    refresh_right_pane();
    ui_mark_dirty(D_PTITLE | D_RIGHT | D_FOOT);
}

static void move_right(const int delta, const bool wrap) {
    const ui_node_t *n = ui_cur_node();
    if (!has_value_pane(n) || !ui_S.rcount) return;
    const int old = ui_S.rsel, old_top = ui_S.rtop;
    int s = ui_S.rsel + delta;
    if (wrap) s = (s < 0) ? ui_S.rcount - 1 : (s > ui_S.rcount - 1) ? 0 : s;
    if (s < 0) s = 0;
    if (s > ui_S.rcount - 1) s = ui_S.rcount - 1;
    ui_S.rsel = (uint8_t)s;
    if (ui_S.rsel < ui_S.rtop) ui_S.rtop = ui_S.rsel;
    if (ui_S.rsel >= ui_S.rtop + LY.body_rows) ui_S.rtop = ui_S.rsel - LY.body_rows + 1;

    if (ui_S.rsel == old && ui_S.rtop == old_top) return;
    if (ui_S.rtop != old_top) ui_mark_dirty(D_RIGHT);
    else { ui_mark_right_row(old - ui_S.rtop); ui_mark_right_row(ui_S.rsel - ui_S.rtop); }
    ui_mark_dirty(D_FOOT);
}

/*
 * Re-evaluate visible() for the open level without losing the cursor: rows
 * can appear or disappear underneath it, so the NODE index is what gets
 * restored, not the row number.
 */
static void rebuild_keeping_selection(void) {
    ui_level_t *L = ui_cur_level();
    const uint8_t wanted = L->nvis ? L->vis[L->sel] : 0;
    ui_build_visible(L);
    for (uint8_t i = 0; i < L->nvis; i++)
        if (L->vis[i] == wanted) { L->sel = i; break; }
    if (L->sel < L->top) L->top = L->sel;
    if (L->sel >= L->top + LY.body_rows) L->top = L->sel - LY.body_rows + 1;
}

static void enter_level(const ui_node_t *n) {
    if (ui_S.depth + 1 >= UI_MAX_DEPTH) return;
    ui_S.depth++;
    ui_level_t *L = ui_cur_level();
    L->parent = n;
    L->sel = L->top = 0;
    L->dyn = false;
    if (n->kind == K_SUB) {
        L->nodes = n->kids;
        L->count = n->count;
    } else if (n->kind == K_DYNAMIC && n->build) {
        /* Built ONCE, on entry -- never per keypress, or holding Down would
         * hammer the SD card. Rebuilt explicitly after an action changes them. */
        ui_dyn_clear(ui_S.dyn);
        n->build(ui_S.dyn);
        L->nodes = NULL;
        L->count = ui_S.dyn->n;
        L->dyn   = true;
    } else {
        ui_S.depth--;
        return;
    }
    ui_build_visible(L);
    if (L->dyn && ui_S.dyn->focus_set) {
        for (uint8_t i = 0; i < L->nvis; i++)
            if (ui_S.dyn->tag[L->vis[i]] == ui_S.dyn->focus_tag) { L->sel = i; break; }
        if (L->sel >= L->top + LY.body_rows) L->top = L->sel - LY.body_rows + 1;
    }
    ui_S.focus = FOCUS_LEFT;
    refresh_right_pane();
    ui_mark_dirty(D_SUB | D_PTITLE | D_LEFT | D_RIGHT | D_FOOT);
}

/*
 * close_at_root: what to do when there is no level left to back out of.
 * Esc closes the menu; Left only ever navigates, so it stops dead at the
 * root -- a stray Left on the root list must not throw the user out.
 */
static void leave_level(const bool close_at_root) {
    if (ui_S.depth == 0) { if (close_at_root) ui_S.quit = true; return; }
    ui_S.depth--;
    ui_S.focus = FOCUS_LEFT;
    ui_build_visible(ui_cur_level());
    refresh_right_pane();
    ui_mark_dirty(D_SUB | D_PTITLE | D_LEFT | D_RIGHT | D_FOOT);
}

/*
 * A page owns the whole screen while it runs, so the fullscreen has to be
 * repainted afterwards -- including the separator and the window border,
 * which ui_flush_dirty never touches.
 */
static void run_modal(void (*fn)(void)) {
    if (!fn) return;
    fn();
    ui_draw_frame_once();
    ui_mark_dirty(D_ALL);
}

static void dyn_invoke(const uint8_t key) {
    ui_level_t *L = ui_cur_level();
    if (!L->nvis) return;
    const uint8_t r = L->vis[L->sel];
    if (ui_S.dyn->dim[r]) return;           /* dimmed rows are informational */
    const ui_node_t *o = ui_dyn_owner();
    if (!o || !o->rowkey) return;
    o->rowkey(ui_S.dyn->tag[r], key);
    if (ui_S.quit) return;
    /* The action may have changed what the level lists (a mount, an eject). */
    if (o->build) {
        const uint8_t keep = L->sel;
        ui_dyn_clear(ui_S.dyn);
        o->build(ui_S.dyn);
        L->count = ui_S.dyn->n;
        L->sel = keep;
        ui_build_visible(L);
    }
    ui_draw_frame_once();
    ui_mark_dirty(D_ALL);
}

static void activate(void) {
    if (ui_cur_level()->dyn) { dyn_invoke(0); return; }
    const ui_node_t *n = ui_cur_node();
    if (!n || !ui_node_enabled(n)) return;      /* greyed rows are inert */
    switch (n->kind) {
        case K_SUB:
        case K_DYNAMIC:
            enter_level(n);
            break;
        case K_RADIO:
        case K_BOOL:
            if (ui_S.focus == FOCUS_LEFT) {     /* Right/Enter: step into the pane */
                ui_S.focus = FOCUS_RIGHT;
                ui_mark_left_row(ui_cur_level()->sel - ui_cur_level()->top);
                ui_mark_dirty(D_RIGHT | D_FOOT);
            } else {                            /* commit the highlighted option */
                uint8_t cnt;
                const ui_option_t *o = ui_node_options(n, &cnt);
                if (ui_S.rsel >= cnt) break;
                ui_node_set_value(n, o[ui_S.rsel].value);
                /*
                 * A value can gate other rows: turning the Beta Disk on
                 * reveals the disk slot. Rebuild the visible set so that
                 * happens now rather than on the next entry into the level.
                 */
                rebuild_keeping_selection();
                ui_S.rcount = (uint8_t)ui_right_row_count(n);
                if (ui_S.rsel >= ui_S.rcount)
                    ui_S.rsel = ui_S.rcount ? (uint8_t)(ui_S.rcount - 1) : 0;
                if (ui_S.rtop > ui_S.rsel) ui_S.rtop = ui_S.rsel;
                ui_mark_dirty(D_LEFT | D_RIGHT | D_FOOT | D_SUB);
            }
            break;
        case K_INT:
            /* Enter steps into the slider and back out of it again. */
            ui_S.focus = (ui_S.focus == FOCUS_LEFT) ? FOCUS_RIGHT : FOCUS_LEFT;
            ui_mark_left_row(ui_cur_level()->sel - ui_cur_level()->top);
            ui_mark_dirty(D_RIGHT | D_FOOT);
            break;
        case K_ACTION:
        case K_PAGE:
            run_modal(n->fn);
            /* An action can change anything, including what other rows say. */
            rebuild_keeping_selection();
            refresh_right_pane();
            break;
        default:
            break;
    }
}

/*
 * Adjust the focused K_INT by mult steps, clamped to [lo, hi]. Redraws the
 * slider and the left row's value label.
 */
static void step_int(const int mult) {
    const ui_node_t *n = ui_cur_node();
    if (!is_int_node(n) || n->step == 0) return;
    const int32_t old = ui_node_value(n);
    int32_t v = old + (int32_t)n->step * mult;
    if (v < n->lo) v = n->lo;
    if (v > n->hi) v = n->hi;
    if (v == old) return;
    ui_node_set_value(n, v);
    ui_mark_right_row(0);
    ui_mark_left_row(ui_cur_level()->sel - ui_cur_level()->top);
    ui_mark_dirty(D_FOOT);
}

/* ── first-letter jump ───────────────────────────────────────────────── */
/*
 * An ordinary letter or digit moves the cursor to the next row whose name
 * starts with it, wrapping, so a long list is one keypress away instead of
 * a dozen Downs. Only the list the user is LOOKING at is searched. The
 * search starts one past the cursor, so repeating the key cycles through
 * every row sharing that initial.
 */
static char first_alnum(const char *s) {
    if (!s) return 0;
    for (; *s; s++) if (isalnum((uint8_t)*s)) return (char)toupper((uint8_t)*s);
    return 0;
}

static const char *left_row_label(const ui_level_t *L, const int i) {
    if (i < 0 || i >= L->nvis) return NULL;
    const uint8_t r = L->vis[i];
    return L->dyn ? ui_S.dyn->label[r] : L->nodes[r].label;
}

static bool jump_to_letter(const char c) {
    const char want = (char)toupper((uint8_t)c);
    if (ui_S.focus == FOCUS_RIGHT) {
        const ui_node_t *n = ui_cur_level()->dyn ? NULL : ui_cur_node();
        if (!has_value_pane(n) || !ui_S.rcount) return false;
        uint8_t cnt;
        const ui_option_t *o = ui_node_options(n, &cnt);
        if (cnt > ui_S.rcount) cnt = ui_S.rcount;
        if (!cnt) return false;
        for (int i = 1; i <= cnt; i++) {
            const int v = (ui_S.rsel + i) % cnt;
            if (first_alnum(o[v].label) == want) {
                move_right(v - ui_S.rsel, false);
                return true;
            }
        }
        return false;
    }
    ui_level_t *L = ui_cur_level();
    if (!L->nvis) return false;
    for (int i = 1; i <= L->nvis; i++) {
        const int v = (L->sel + i) % L->nvis;
        if (first_alnum(left_row_label(L, v)) == want) {
            move_left(v - L->sel, false);
            return true;
        }
    }
    return false;
}

/* ── keys ────────────────────────────────────────────────────────────── */

typedef enum {
    NK_NONE = 0, NK_UP, NK_DOWN, NK_LEFT, NK_RIGHT,
    NK_ENTER, NK_ESC, NK_PGUP, NK_PGDN, NK_HOME, NK_END, NK_CLOSE, NK_CHAR
} ui_key_t;

static bool handle_key(const ui_key_t k, const char ch) {
    /* In the right pane of a K_INT row, the vertical keys adjust the value. */
    const bool int_pane = (ui_S.focus == FOCUS_RIGHT) && !ui_cur_level()->dyn
                          && is_int_node(ui_cur_node());
    if (int_pane) {
        switch (k) {
            case NK_UP:   step_int(+1); return ui_S.quit;
            case NK_DOWN: step_int(-1); return ui_S.quit;
            case NK_PGUP: step_int(+4); return ui_S.quit;
            case NK_PGDN: step_int(-4); return ui_S.quit;
            case NK_HOME: step_int(+32767); return ui_S.quit;
            case NK_END:  step_int(-32767); return ui_S.quit;
            default: break;                  /* Enter/Left/Esc fall through below */
        }
    }
    switch (k) {
        case NK_UP:
            (ui_S.focus == FOCUS_LEFT) ? move_left(-1, true) : move_right(-1, true);
            break;
        case NK_DOWN:
            (ui_S.focus == FOCUS_LEFT) ? move_left(+1, true) : move_right(+1, true);
            break;
        case NK_PGUP:
            (ui_S.focus == FOCUS_LEFT) ? move_left(-LY.body_rows, false)
                                       : move_right(-LY.body_rows, false);
            break;
        case NK_PGDN:
            (ui_S.focus == FOCUS_LEFT) ? move_left(+LY.body_rows, false)
                                       : move_right(+LY.body_rows, false);
            break;
        case NK_HOME:
            (ui_S.focus == FOCUS_LEFT) ? move_left(-UI_MAX_ROWS, false)
                                       : move_right(-UI_MAX_ROWS, false);
            break;
        case NK_END:
            (ui_S.focus == FOCUS_LEFT) ? move_left(+UI_MAX_ROWS, false)
                                       : move_right(+UI_MAX_ROWS, false);
            break;
        case NK_RIGHT:
        case NK_ENTER:
            activate();
            break;
        case NK_LEFT:
            if (ui_S.focus == FOCUS_RIGHT) {
                ui_S.focus = FOCUS_LEFT;
                ui_mark_left_row(ui_cur_level()->sel - ui_cur_level()->top);
                ui_mark_dirty(D_RIGHT | D_FOOT);
            } else {
                leave_level(false);
            }
            break;
        case NK_ESC:
            if (ui_S.focus == FOCUS_RIGHT) {
                ui_S.focus = FOCUS_LEFT;
                ui_mark_left_row(ui_cur_level()->sel - ui_cur_level()->top);
                ui_mark_dirty(D_RIGHT | D_FOOT);
            } else {
                leave_level(true);
            }
            break;
        case NK_CHAR:
            jump_to_letter(ch);
            break;
        case NK_CLOSE:
            ui_S.quit = true;
            break;
        default:
            break;
    }
    return ui_S.quit;
}

/*
 * One key, as this board delivers them.
 *
 * The console and the USB keyboard both arrive as a byte stream with
 * ANSI escape sequences for the cursor keys, which is what menu.c's
 * queue already carries -- so this is a decoder for that, not fabgl's
 * VirtualKey table.
 *
 * The continuation of an escape sequence is read with a timeout, because
 * Esc on its own is a key this menu uses constantly and waiting for ever
 * to find out whether a '[' is coming would hang on every press of it.
 * Twenty milliseconds is far longer than a terminal takes to send the
 * rest of a sequence and far shorter than a person takes to press
 * another key.
 */
#define ESC_CONT_MS 20

static ui_key_t next_key(char *ch) {
    for (;;) {
        const int c = menu_wait_key();
        if (c < 0) continue;

        if (c == 0x1B) {
            const int b = menu_wait_key_timeout(ESC_CONT_MS);
            if (b != '[' && b != 'O') return NK_ESC;     /* a bare Esc */
            const int d = menu_wait_key_timeout(ESC_CONT_MS);
            switch (d) {
                case 'A': return NK_UP;
                case 'B': return NK_DOWN;
                case 'C': return NK_RIGHT;
                case 'D': return NK_LEFT;
                case 'H': return NK_HOME;
                case 'F': return NK_END;
                /* "5~" and "6~": swallow the tilde so it is not read as a jump. */
                case '5': menu_wait_key_timeout(ESC_CONT_MS); return NK_PGUP;
                case '6': menu_wait_key_timeout(ESC_CONT_MS); return NK_PGDN;
                default:  continue;
            }
        }

        switch (c) {
            case '\r':
            case '\n': return NK_ENTER;
            /* F11/Tab and F12/backslash both close: whichever key opened
             * this, pressing it again is the way back out. */
            case '\t':
            case '\\': return NK_CLOSE;
            default:
                if (c >= ' ' && c < 0x7F) { *ch = (char)c; return NK_CHAR; }
                continue;
        }
    }
}

/* ── the session ─────────────────────────────────────────────────────── */

bool ui_menu_available(void) { return true; }

void ui_menu_run(void) {
    if (!ui_menu_available()) return;

    ui_compute_layout();
    if (!ui_layout_fits()) return;

    memset(&ui_S, 0, sizeof ui_S);
    ui_S.dyn = &dyn_pool;
    ui_dyn_clear(ui_S.dyn);
    ui_S.depth = 0;
    ui_S.focus = FOCUS_LEFT;

    ui_level_t *L = ui_cur_level();
    L->nodes  = ui_root_nodes();
    L->count  = ui_root_count();
    L->parent = NULL;
    ui_build_visible(L);
    refresh_right_pane();

    ui_status("");
    ui_draw_frame_once();
    ui_mark_dirty(D_ALL);
    ui_flush_dirty();

    const vga_mode_t was = vga_get_mode();
    vga_set_mode(VGA_MODE_UI);

    while (!ui_S.quit) {
        char ch = 0;
        const ui_key_t k = next_key(&ch);
        handle_key(k, ch);
        ui_flush_dirty();
    }

    vga_set_mode(was);
}
