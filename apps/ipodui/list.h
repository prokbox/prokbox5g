/***************************************************************************
 *              __________                   __    __________
 *   Open       \______   \_______    ____  |  | __\______   \  ____  ___  ___
 *   Source      |     ___/\_  __ \  /  _ \ |  |/ / |    |  _/ /  _ \ \  \/  /
 *   Player      |    |     |  | \/ (  <_> )|    <  |    |   \(  <_> ) >    <
 *   Firmware    |____|     |__|     \____/ |__|_ \ |______  / \____/ /__/\_ \
 *                                               \/        \/               \/
 *
 * Custom firmware for the iPod Video, 5th generation (5G and 5.5G).
 * Not affiliated with or endorsed by Apple Inc.
 *
 * iPod UI shell
 *
 * Copyright (C) 2026 prokopto-dev
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 ****************************************************************************/
#ifndef _IPODUI_LIST_H_
#define _IPODUI_LIST_H_

#include <stdbool.h>
#include <stddef.h>
#include "lcd.h"
#include "ipodui/scroll.h"

/* The 7G list: the selection follows the wheel at once, the view glides
 * (ui_scroll) to keep it visible. At rest the view is row-snapped and
 * static draws go straight to the framebuffer; while gliding, rows are
 * rendered once into the shared offscreen strip (a ring of row slots)
 * and each frame blits the visible window. A scrollbar shows while
 * moving through a list longer than the view; a selected label wider
 * than its space marquee-scrolls after a pause; other labels that don't
 * fit end in "...".
 *
 * Geometry (docs/design/7g-spec.md "List geometry", the owner's photos):
 * the rows FILL the view exactly. A style says how many whole rows the
 * view shows; content row i starts at i * view height / rows (rounded
 * down), so a 220 px view of 9 rows has rows of 24 and 25 px, and any 9
 * consecutive rows span exactly 220 px. A row's height belongs to its
 * index, so it never changes while scrolling. Two-line styles add a
 * smaller grey second line, an optional square art box at the left (the
 * selection bar starts right of it) and a separator line at the bottom
 * of every row.
 *
 * All y coordinates in this API are LCD coordinates, so a band can go
 * straight to lcd_update_rect(). The struct may live in movable
 * (core_alloc) memory: nothing keeps a pointer to it between calls. */

#define UI_ROW_TWO_LINE 0x01    /* title + second line (get_sublabel) */
#define UI_ROW_SEP      0x02    /* separator on each row's last pixel row */

struct ui_row_style {
    unsigned char rows;         /* whole rows per view (pitch = h / rows) */
    unsigned char thumb;        /* art box: px square at x 0; 0 = none */
    unsigned char text_x;       /* label cell x in the row */
    unsigned char flags;        /* UI_ROW_* */
};

struct ui_list {
    struct viewport vp;         /* list area (header excluded) */
    int count, selected, top;   /* top = first visible row at rest */
    const char *(*get_label)(int idx, void *data, char *buf, size_t len);
    bool (*has_chevron)(int idx, void *data);   /* may be NULL */
    void *data;
    /* Phase 4 hooks (all optional, NULL/0 = off):
     * extra_w: px kept free right of every label (the text clips and
     *   marquees in less width), for draw_extra's content.
     * draw_extra: called after a row's label with the row's viewport set
     *   (row coordinates, 0,0 = row top-left, w x h = the row, drawmode
     *   solid); 'sel' = the row is selected (on the blue bar). Draws into
     *   the LCD or a strip slot alike, so it must only use lcd_* calls.
     * overlay: called after rows were drawn into the framebuffer (static
     *   band, glide frame, marquee step) with that band in LCD coordinates;
     *   draws whatever floats over the rows (the quick-scroll letter),
     *   clipped to the band, before the band is pushed. The default
     *   viewport is set. */
    int  extra_w;
    void (*draw_extra)(int idx, void *data, int w, int h, bool sel);
    void (*overlay)(void *data, int y, int h);
    /* Phase 4b (optional):
     * style: set with ui_list_set_style(); ui_list_init() sets the plain
     *   single-line style (ui_theme->list_rows rows per view).
     * get_sublabel: the second line of a two-line row (NULL or "" for
     *   none).
     * draw_thumb: draws the art box (style.thumb px square at the row's
     *   0,0; the row's viewport is set as for draw_extra). 'fetch' is
     *   false for rows rendered while the list glides: draw only what is
     *   already in RAM and touch no disk. Returns false if what it drew
     *   is a stand-in that a fetch could replace; such rows are drawn
     *   again at rest, after the glide (and its boost) ended. A fetch
     *   may yield: a list with draw_thumb must stay in place (pinned)
     *   through ui_list_draw() and ui_list_tick(). */
    struct ui_row_style style;
    const char *(*get_sublabel)(int idx, void *data, char *buf, size_t len);
    bool (*draw_thumb)(int idx, void *data, int size, bool fetch);
    /* #69 (optional): true if row idx carries the now-playing mark
     * (npmark.h). The row then draws the glyph at its right edge (left of
     * the chevron's place on a row with a chevron) and its label(s)
     * truncate UI_NPMARK_GAP px before it. Called for every row drawn:
     * RAM compares only. */
    bool (*has_mark)(int idx, void *data);
    /* (optional) true if row idx can't be chosen now (a setting another
     * one turns off): its label and value draw in list_fg_off. The list
     * still moves onto it; the screen skips such rows (ui_list_skip())
     * and ignores SELECT on them. RAM compares only. */
    bool (*is_off)(int idx, void *data);
    /* motion (private) */
    struct ui_scroll scroll;    /* px; at rest pos == target == row y of top */
    int  strip_id;              /* owner id for the strip's row slots */
    long last_frame;            /* tick of the last scroll frame */
    long sbar_hide;             /* tick when the scrollbar hides */
    bool sbar;                  /* scrollbar on screen */
    bool sbar_new;              /* shown at rest, its column not pushed yet */
    bool fill_due;              /* rows got stand-in art during a glide */
    bool jump;                  /* fast spin: whole-row jumps (UI_LIST_JUMP_V) */
    int  mq_w;                  /* selected label width if it overflows, else 0 */
    int  mq_x;                  /* marquee offset (px scrolled left) */
    int  mq_tw;                 /* its text box, less an inline value */
    long mq_next;               /* tick of the next marquee step */
    bool mq_dark;               /* parked while the backlight is off */
};

/* Fast-scroll row jumps (#58, owner device feedback): the 5.5G's LCD
 * responds slowly, so a smooth glide at a high wheel speed smears and
 * ghosts the glyphs. Above UI_LIST_JUMP_V clicks/s (input.c's wheel
 * speed: one click = one wheel event, 15 degrees) the list moves in
 * whole-row jumps: each frame lands the view on the selection's row
 * grid with no intermediate glide positions, at most one frame per
 * UI_LIST_JUMP_TICKS (positions in between are dropped, not queued),
 * as the 7G does when spun fast. The quick-scroll letter shows as
 * before. It stays in jump mode until the speed falls below
 * UI_LIST_JUMP_V_EXIT (hysteresis, so a spin at the edge doesn't flip
 * between the two every click); the art of rows drawn with a stand-in
 * is read once the wheel has rested UI_LIST_JUMP_SETTLE ticks. Normal
 * speeds glide exactly as before. UI_LIST_JUMP_V 0 turns it off.
 * Device check (P10, docs/ROADMAP.md): tune all four on the 5.5G. */
#ifndef UI_LIST_JUMP_V
#define UI_LIST_JUMP_V          24  /* clicks/s: ~1 turn/s, 2 rows a click */
#endif
#ifndef UI_LIST_JUMP_V_EXIT
#define UI_LIST_JUMP_V_EXIT     16
#endif
#ifndef UI_LIST_JUMP_TICKS
#define UI_LIST_JUMP_TICKS      (HZ/20)     /* <= 20 jump frames/s */
#endif
#ifndef UI_LIST_JUMP_SETTLE
#define UI_LIST_JUMP_SETTLE     (HZ/4)
#endif

/* Rows kept in view beyond the selection when it moves (owner #52,
 * 2026-10-20: on the 7G, moving down keeps one row visible below the
 * selection before the list scrolls; moving up, one above). The view
 * glides to keep the margin, except at the list's ends, where the
 * selection reaches the first or last row. Capped at (rows - 1) / 2.
 * Positions set without a move (a pop, a restored selection, a new
 * count) only keep the selection visible. 0 = scroll only when the
 * selection would leave the view (Phase 2-8 behaviour). */
#ifndef UI_LIST_EDGE_ROWS
#define UI_LIST_EDGE_ROWS       0   /* owner to confirm the 7G rule (2026-10-20): 1 keeps a row visible below a moving selection */
#endif

void ui_list_init(struct ui_list *l, const struct viewport *vp, int count,
                  const char *(*get_label)(int, void *, char *, size_t),
                  void *data);
/* Moves selection (clamped, no wrap -- 7G doesn't wrap). Fills up to two
 * y-bands (LCD coordinates) that need a static redraw -- the old and the
 * new row, or one band for adjacent rows -- and returns how many. Returns
 * 0 if nothing changed, or if the view has to move: then the list glides
 * on its own from ui_list_tick() (without a strip it snaps and returns
 * the whole view as one band, like Phase 2). */
int ui_list_move(struct ui_list *l, int delta, int band_y[2], int band_h[2]);
/* Static draw of the rows in a band, at rest. A draw covering the whole
 * view also ends any glide (snaps) and restarts the marquee delay. */
void ui_list_draw(struct ui_list *l, int y, int h);

/* Changes the item count, keeping the selection in range and visible. */
/* The top a ui_list_move(l, delta) would leave (nothing moves): a paged
 * list asks which rows a move would show before it makes it */
int ui_list_top_after(const struct ui_list *l, int delta);
/* The first row the view shows now (a glide's frame may lag the top) */
int ui_list_shown_top(const struct ui_list *l);
void ui_list_set_count(struct ui_list *l, int count);
/* The move that lands the selection 'delta' rows away counting only the
 * rows is_off() allows (clamped; 0 if no such row lies that way): pass
 * it to ui_list_move(). Without is_off it returns delta. */
int ui_list_skip(const struct ui_list *l, int delta);

/* Sets the row style and resettles the view (follow with a full draw). */
void ui_list_set_style(struct ui_list *l, const struct ui_row_style *st);
/* Changes the view's height and its rows per view (the rows then fill
 * the new height), e.g. to make room for a status line under the list.
 * Follow with a full draw. */
void ui_list_set_height(struct ui_list *l, int height, int rows);

/* Geometry at rest, LCD coordinates. ui_list_row_band(): the band of
 * row 'idx' if it is on screen at rest (false otherwise).
 * ui_list_rows_band(): widens [*y, *y + *h) to whole rows on screen,
 * clipped to the view. */
bool ui_list_row_band(const struct ui_list *l, int idx, int *y, int *h);
void ui_list_rows_band(const struct ui_list *l, int *y, int *h);

/* Time-based work: a glide frame (when due), the marquee, the scrollbar
 * timeout and, once a glide has ended, the art of the rows it drew with
 * a stand-in. Draws and pushes its own LCD rects. Returns UI_TICK_ANIM
 * while gliding, else ticks until it wants to be called again, or
 * TIMEOUT_BLOCK. */
int ui_list_tick(struct ui_list *l);

#ifdef SIMULATOR
/* "list_force_bar on|off" (sim_cmd_fit3a.c): every long list's scrollbar
 * held on, or hidden at once; any thread */
void ui_list_force_bar(bool on);
#endif

/* True while the view glides: every frame then redraws the whole view
 * (and the overlay), so nothing needs to be invalidated meanwhile. */
static inline bool ui_list_gliding(const struct ui_list *l)
{
    return l->scroll.pos != l->scroll.target;
}

#endif /* _IPODUI_LIST_H_ */
