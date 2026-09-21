/*
 * frank-dino — an RP2350B acting as the whole machine around a real Z80
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Ported from pico-speccy src/ui/UiNav.h and the portable half of
 * UiNav.cpp (GPL-3.0-or-later): the level stack, the two-pane focus, the
 * cursor arithmetic and the key handling. What did not come across is
 * their session plumbing -- the heap-allocated row pool, the staging
 * commit dialog, the WiFi and profile session hooks -- none of which has
 * anything on this side to talk to.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "ui_model.h"

typedef enum { FOCUS_LEFT = 0, FOCUS_RIGHT = 1 } ui_focus_t;

#define UI_MAX_DEPTH 5
#define UI_MAX_ROWS  64

/*
 * One menu level. `vis[]` maps a visible row index to a node index, so a
 * hidden row (visible() == false) never shifts the identity of the rows
 * around it.
 */
typedef struct {
    const ui_node_t *nodes;
    const ui_node_t *parent;    /* NULL at the root */
    uint8_t          count;
    uint8_t          sel;       /* selected visible row */
    uint8_t          top;       /* first visible row on screen */
    uint8_t          nvis;
    uint8_t          vis[UI_MAX_ROWS];
    bool             dyn;       /* rows come from S.dyn, not from nodes[] */
} ui_level_t;

typedef struct {
    ui_level_t    lv[UI_MAX_DEPTH];
    uint8_t       depth;                /* 0 = root */
    uint8_t       focus;                /* FOCUS_LEFT / FOCUS_RIGHT */
    uint8_t       rsel, rtop, rcount;   /* right pane selection / scroll / rows */
    bool          quit;
    ui_dynrows_t *dyn;                  /* the single dynamic-row pool */
} ui_state_t;

extern ui_state_t ui_S;

static inline ui_level_t *ui_cur_level(void) { return &ui_S.lv[ui_S.depth]; }

/* NULL on a dynamic level: its rows are pool entries, not nodes. */
static inline const ui_node_t *ui_cur_node(void) {
    const ui_level_t *L = ui_cur_level();
    return (!L->dyn && L->nvis) ? &L->nodes[L->vis[L->sel]] : NULL;
}
static inline const ui_node_t *ui_dyn_owner(void) {
    const ui_level_t *L = ui_cur_level();
    return L->dyn ? L->parent : NULL;
}

void             ui_build_visible(ui_level_t *L);
const ui_node_t *ui_sub_visible_child(const ui_node_t *n, int i);
int              ui_right_row_count(const ui_node_t *n);
const char      *ui_right_title(const ui_node_t *n);
const char      *ui_dyn_hint(int i);

/* An action that restarted the machine asks the menu to close. */
void ui_request_close(void);

/*
 * Open the menu and run it until the user leaves.
 *
 * Owns the screen and the key loop for as long as it is up, and puts the
 * video mode back the way it found it. The machine must already be
 * paused: this reads the card and draws into the PSRAM, neither of which
 * is safe with the Z80 clocked.
 */
void ui_menu_run(void);

/* False when there is no PSRAM, and so nowhere for the surface to live. */
bool ui_menu_available(void);
