/*
 * frank-dino — an RP2350B acting as the whole machine around a real Z80
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Ported from pico-speccy src/ui/UiModel.h (GPL-3.0-or-later).
 *
 * Their declarative menu tree: a static, const, flash-resident array of
 * nodes, no runtime construction and no heap. The rule that matters came
 * across with it -- option values are stored as explicit {label, value}
 * pairs, NEVER positionally, so display order and stored value are
 * independent by construction and a row can be reordered or renamed
 * without silently changing the meaning of a saved setting. So did the
 * second one: a conditional row uses a visible() predicate rather than
 * rebuilding the row list, which removes the whole class of index
 * arithmetic a hand-built menu needs.
 *
 * Two things are different here, both forced by the firmware underneath.
 *
 * Their `setting` is a SettingId into UiStage, an 88 KB registry that
 * stages every edit and applies the lot when the menu closes -- because
 * changing a machine in ESPectrum needs a reset to take effect. This
 * firmware decided the opposite in settings.h ("a setting that has to be
 * applied before it takes effect is a setting that can be out of step
 * with what the menu says"), so a node points straight at its byte in
 * `settings` and carries the hook to run after it changes. The staging
 * overlay, the pending counter and the red header band go with it.
 *
 * K_TEXT and K_PICK did not come across: there is no text field and no
 * profile list on this board to need them.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

typedef enum {
    K_SUB = 0,    /* static child list (kids/count) */
    K_DYNAMIC,    /* child list produced by build() when the level is entered */
    K_RADIO,      /* one-of-N over opts[]; stored value is ui_option_t::value */
    K_BOOL,       /* Off/On; sugar over a 2-entry opts[] */
    K_INT,        /* integer range, Left/Right steps by `step` */
    K_ACTION,     /* runs fn() immediately (not a setting) */
    K_PAGE,       /* full-screen info page; fn() owns its own key loop */
} ui_kind_t;

typedef struct {
    const char *label;
    int32_t     value;      /* the value actually stored -- not the row index */
    /* Optional compact form for the left column ("48K" against the right
     * pane's "ZX Spectrum 48K"); NULL = label is used in both places. */
    const char *slabel;
} ui_option_t;

/*
 * Rows of a K_DYNAMIC level are built into this fixed pool. Labels are
 * COPIED, so a builder can format into a scratch buffer without leaving
 * a dangling pointer behind.
 */
#define UI_DYN_MAX_ROWS   64
#define UI_DYN_LABEL_LEN  48
#define UI_DYN_VALUE_LEN  22

typedef struct {
    uint8_t n;
    char    label[UI_DYN_MAX_ROWS][UI_DYN_LABEL_LEN];
    char    value[UI_DYN_MAX_ROWS][UI_DYN_VALUE_LEN];
    int32_t tag[UI_DYN_MAX_ROWS];       /* passed to the level's rowkey() */
    uint8_t dim[UI_DYN_MAX_ROWS];       /* 1 = greyed out and not selectable */
    int32_t focus_tag;                  /* land the cursor on the row with this tag */
    bool    focus_set;
} ui_dynrows_t;

void ui_dyn_clear(ui_dynrows_t *d);
void ui_dyn_add(ui_dynrows_t *d, const char *lbl, const char *val, int32_t tag, bool dimmed);
void ui_dyn_focus_tag(ui_dynrows_t *d, int32_t tag);

typedef struct ui_node {
    const char        *label;
    ui_kind_t          kind;
    uint8_t            count;   /* opts[] entries, or kids[] entries for K_SUB */
    uint8_t           *slot;    /* the byte in `settings`; NULL = not a setting */
    const ui_option_t *opts;
    const struct ui_node *kids;
    bool             (*visible)(void);          /* NULL = always visible */
    void             (*fn)(void);               /* K_ACTION / K_PAGE */
    void             (*build)(ui_dynrows_t *);  /* K_DYNAMIC: fill the row pool */
    /* K_DYNAMIC: a row was activated. `key` is 0 for Enter, else the function-key
     * number, so one callback covers "open" and the per-row verbs. */
    void             (*rowkey)(int32_t tag, uint8_t key);
    int16_t            lo, hi, step;            /* K_INT */
    /* Optional dynamic value label, drawn right-aligned like a stored value.
     * Lets an action row show live state ("no tape", "2 tracks") without
     * being a stored setting. */
    const char      *(*vlabel)(void);
    /* Optional enable gate, independent of visible(): a row whose gate returns
     * false stays VISIBLE but greyed out and inert -- the generic "these options
     * belong to a feature that is currently off" affordance. */
    bool             (*enabled)(void);
    /* Run after this node's value changes. Theirs lives in the commit stage. */
    void             (*changed)(void);
} ui_node_t;

/*
 * Counts are derived from the array, never written by hand: a hand-written
 * count that drifts from the array is the one bug this model cannot catch.
 */
#define UI_COUNT(a) (uint8_t)(sizeof(a) / sizeof((a)[0]))

#define UI_SUB(lbl, kids_arr, vis) \
    { lbl, K_SUB, UI_COUNT(kids_arr), NULL, NULL, kids_arr, vis, \
      NULL, NULL, NULL, 0, 0, 0, NULL, NULL, NULL }
#define UI_ACTION(lbl, f) \
    { lbl, K_ACTION, 0, NULL, NULL, NULL, NULL, f, NULL, NULL, 0, 0, 0, NULL, NULL, NULL }
/* Action whose row shows live state via `vl` ("Eject tape   no tape"). */
#define UI_ACTIONV(lbl, f, vl) \
    { lbl, K_ACTION, 0, NULL, NULL, NULL, NULL, f, NULL, NULL, 0, 0, 0, vl, NULL, NULL }
#define UI_ACTIONV_EN(lbl, f, vl, en) \
    { lbl, K_ACTION, 0, NULL, NULL, NULL, NULL, f, NULL, NULL, 0, 0, 0, vl, en, NULL }
#define UI_PAGE(lbl, f) \
    { lbl, K_PAGE, 0, NULL, NULL, NULL, NULL, f, NULL, NULL, 0, 0, 0, NULL, NULL, NULL }
#define UI_RADIO(lbl, field, opts_arr, ch) \
    { lbl, K_RADIO, UI_COUNT(opts_arr), field, opts_arr, NULL, NULL, \
      NULL, NULL, NULL, 0, 0, 0, NULL, NULL, ch }
#define UI_RADIO_EN(lbl, field, opts_arr, ch, en) \
    { lbl, K_RADIO, UI_COUNT(opts_arr), field, opts_arr, NULL, NULL, \
      NULL, NULL, NULL, 0, 0, 0, NULL, en, ch }
#define UI_BOOL(lbl, field, ch) \
    { lbl, K_BOOL, UI_COUNT(ui_opt_offon), field, ui_opt_offon, NULL, NULL, \
      NULL, NULL, NULL, 0, 0, 0, NULL, NULL, ch }
#define UI_BOOL_EN(lbl, field, ch, en) \
    { lbl, K_BOOL, UI_COUNT(ui_opt_offon), field, ui_opt_offon, NULL, NULL, \
      NULL, NULL, NULL, 0, 0, 0, NULL, en, ch }
#define UI_INT(lbl, field, l, h, s, ch) \
    { lbl, K_INT, 0, field, NULL, NULL, NULL, NULL, NULL, NULL, l, h, s, NULL, NULL, ch }
#define UI_DYN(lbl, bld, rk, vl) \
    { lbl, K_DYNAMIC, 0, NULL, NULL, NULL, NULL, NULL, bld, rk, 0, 0, 0, vl, NULL, NULL }
/* Dynamic level with its own right-pane verb list, whose labels are the hints. */
#define UI_DYNH(lbl, bld, rk, hints, vl) \
    { lbl, K_DYNAMIC, UI_COUNT(hints), NULL, hints, NULL, NULL, NULL, bld, rk, \
      0, 0, 0, vl, NULL, NULL }

extern const ui_option_t ui_opt_offon[2];

/* The tree root, defined in ui_tree.c. */
const ui_node_t *ui_root_nodes(void);
uint8_t          ui_root_count(void);
/* What the sub-header says on the right: "Machine: Pentagon 128". */
const char      *ui_machine_line(void);

/* True when the node's enable gate (if any) allows interaction. */
static inline bool ui_node_enabled(const ui_node_t *n) {
    return !n->enabled || n->enabled();
}

/* Option table of a K_RADIO/K_BOOL node. */
static inline const ui_option_t *ui_node_options(const ui_node_t *n, uint8_t *cnt) {
    *cnt = n->count;
    return n->opts;
}

/* Current value of a value-carrying node, and the label to show for it. */
int32_t     ui_node_value(const ui_node_t *n);
void        ui_node_set_value(const ui_node_t *n, int32_t v);
const char *ui_node_value_label(const ui_node_t *n);   /* "" when it carries none */
