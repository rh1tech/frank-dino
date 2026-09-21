/*
 * frank-dino — an RP2350B acting as the whole machine around a real Z80
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-dino
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Modal primitives, ported from pico-speccy src/ui/UiDialog.cpp.
 *
 * They draw with ui_gfx over whatever is on screen and never save what
 * was underneath -- the caller owns the repaint afterwards, exactly as
 * run_modal() in ui_nav.c already does for actions.
 *
 * Their file also carries an inline line editor, a pick list, a busy
 * box, a paused badge and a countdown confirm. Those came with features
 * this firmware does not have (renaming files over FTP, choosing a tape
 * block, a post-reboot video-mode revert), so they are not here yet.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/*
 * Yes/No question. `text` may contain '\n'. Returns true on Yes.
 *
 * Keys: the cursor keys move, Enter accepts, Esc is No, Y and N answer
 * directly. `default_yes` puts the initial highlight on Yes -- only for
 * questions whose answer is safe to take on a bare Enter; a destructive
 * one keeps the default on No, which is the whole point of asking.
 */
bool ui_confirm(const char *text, const char *title, bool default_yes);

/*
 * N buttons in a row under the text. Returns the chosen index; Esc
 * returns `esc_result`.
 */
int ui_choice(const char *text, const char *const *btns, int n, int initial,
              int esc_result);

/*
 * Centred message. `ms` > 0 shows it for that long and any key dismisses
 * it early; `ms` == 0 waits for a key. `warn` colours the border.
 */
void ui_toast(const char *msg, bool warn, int ms);

/*
 * A full-screen scrolling page of text, in the window everything else
 * uses: header with the rainbow and the title, body, footer with the
 * scroll hint and a position counter once there is more than fits.
 *
 * `body` is newline-separated and stays put while the page is up. A
 * 0x02 byte switches ink for the rest of the line, as their markup does
 * -- UI_MK_* are the codes, and anything else is drawn as text.
 *
 * Owns the screen and the key loop, and leaves the surface painted: the
 * caller repaints, exactly as it does after a dialog.
 */
#define UI_MK_DIM    "\x02" "d"
#define UI_MK_TEXT   "\x02" "t"
#define UI_MK_WHITE  "\x02" "w"
#define UI_MK_OK     "\x02" "g"
#define UI_MK_BAD    "\x02" "r"
#define UI_MK_HEAD   "\x02" "h"

void ui_text_page(const char *title, const char *body, const char *hdr_right);

/*
 * A modal scrolling list. Returns the chosen index, or -1 on Esc.
 *
 * Rows are fetched on demand rather than handed over as an array of
 * pointers, which is their uiPickListCb: it is what lets a caller format
 * a row out of a struct it already has instead of building a second list
 * of strings beside it.
 */
typedef void (*ui_row_cb)(int idx, char *out, unsigned outsz);

int ui_pick_list(const char *title, int n, ui_row_cb cb, int initial, int wchars,
                 const char *footer);
