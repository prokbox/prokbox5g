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
#ifndef _IPODUI_SCREEN_STACK_H_
#define _IPODUI_SCREEN_STACK_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "ipodui/input.h"
#include "ipodui/modload.h"     /* #106: module stand-in names */

#define UI_DIRTY_HEADER  0x1
#define UI_DIRTY_BODY    0x2
#define UI_DIRTY_ALL     (UI_DIRTY_HEADER|UI_DIRTY_BODY)
/* Only the band set with ui_invalidate_band() changed (e.g. a selection
 * move). draw() gets this flag and reads the bands with ui_dirty_bands(). */
#define UI_DIRTY_BAND    0x4

enum ui_result { UI_HANDLED = 0, UI_IGNORED, UI_POP };

struct ui_screen;
struct ui_panel;                /* preview.h */
struct ui_screen_ops {
    const char *name;                                    /* logs and tests */
    int  (*enter)(struct ui_screen *s, const void *arg); /* <0 aborts the push */
    void (*leave)(struct ui_screen *s);                  /* free all core_alloc */
    void (*draw)(struct ui_screen *s, unsigned dirty);   /* draw only; no lcd_update */
    enum ui_result (*input)(struct ui_screen *s, const struct ui_input *in);
    int  (*timeout)(struct ui_screen *s);                /* optional: ticks until
                                                            next wakeup, or
                                                            TIMEOUT_BLOCK */
    /* optional: time-based work of the top screen (list glide frames,
     * marquee, scrollbar timeout), called once per main-loop pass. It
     * draws and pushes its own rects (ui_lcd_push) and returns
     * UI_TICK_ANIM while animating, else ticks until its next timer or
     * TIMEOUT_BLOCK. Not called while the screen is covered or a slide
     * transition runs. */
    int  (*tick)(struct ui_screen *s);
    /* optional, split leaf screens only (Settings > Backlight, #38): the
     * screen's own panel (tile + current value). When the top screen has
     * it, the panel scheduler (preview.h) keys the panel on the screen
     * instead of an item preview and calls this after the rest delay;
     * the screen calls ui_panel_invalidate() when its value changes. The
     * same rules as struct ui_preview_ops.draw apply (draw only into
     * p->vp, every pixel, no lcd_update, no allocation). A reveal
     * (UI_T_REVEAL) shows this before expanding, so nothing jumps. */
    void (*draw_into_panel)(struct ui_screen *s, const struct ui_panel *p);
    /* optional, a headerless page pushed with UI_T_REVEAL (Extras >
     * Clocks, P12): its foreground (the opaque rows from y 0 that move;
     * the Extras backdrop under the rest stays put) drawn with its left
     * edge at x (0..LCD_WIDTH), clipped at the right edge, over what is
     * there; no lcd_update. Returns the height of those rows. x =
     * LCD_WIDTH draws nothing (the main loop's query after a full draw). */
    int  (*reveal)(struct ui_screen *s, int x);
};

struct ui_screen {
    const struct ui_screen_ops *ops;
    const char *title;          /* shown in header */
    int  header_w;              /* header width from x 0, 0 = full width */
    unsigned dirty;
    int  handle;                /* core_alloc handle for screen state, 0 if none */
    void *priv;                 /* small inline state */
    unsigned char transition;   /* enum ui_transition it was pushed with;
                                   its pop mirrors it */
    uint16_t arena_mark;        /* P11: the screen arena's top before this
                                   screen's enter() (ui_push_t); reset to
                                   it when the screen leaves */
};

/* Transitions (spec index.md "Transitions"; always a slide, never a cut).
 * The values are fixed: nav_core.c's nav_transition_ticks() takes them
 * as an int.
 *  UI_T_PUSH    slide; durations from nav_transition_ticks(): 190 ms
 *               ease-out, split -> full 250 ms (its pop 290 ms). Between
 *               two split screens the panel stays put.
 *  UI_T_REVEAL  (Extras > Clocks) the backdrop stays while the list,
 *               header and panel content leave left and the destination
 *               comes in from the right, 250 ms (nav_reveal_offsets(),
 *               the measured motion). Runs when the split screen's
 *               partner is a headerless page with a reveal() op; else
 *               (a module stand-in) as a UI_T_PUSH.
 *  UI_T_SPLIT   (Cover Flow, Phase 7) the screen parts at x=160, list and
 *               header left, panel right, ease-in 210 ms (exit 290 ms).
 *               STUB in v1: runs as a UI_T_PUSH. */
enum ui_transition {
    UI_T_PUSH   = 0,
    UI_T_REVEAL = 1,
    UI_T_SPLIT  = 2,
    /* a system screen (Charging): a plain slide even between a split and
     * a full screen, never the split <-> full parting (8b-4a) */
    UI_T_SYSTEM = 3,
};

#define UI_STACK_MAX 8          /* static: 8 * sizeof(struct ui_screen) < 256 B */
int  ui_push_t(const struct ui_screen_ops *ops, const void *arg,
               enum ui_transition t);
/* == ui_push_t(ops, arg, UI_T_PUSH) */
int  ui_push(const struct ui_screen_ops *ops, const void *arg);
void ui_pop(void);
/* The top screen became another page in place (the video screen's
 * failure prompt, from its tick): it slides in as a push would, over
 * what the framebuffer shows, and leaves with a normal pop. */
void ui_restage(void);
/* Hold MENU (spec index.md "Global button behaviour"): leaves every
 * screen above the root, top first, and records one pop (slide back,
 * UI_T_PUSH). Nothing if the root is on top. */
void ui_pop_to_root(void);
/* A screen asks to leave on its own (Now Playing once playback has
 * stopped), from its tick(). The main loop pops it right after the tick,
 * before it waits for input: the request is not a key event, so the
 * unlit face's wake swallowing can never eat it (Phase 8a fix M2).
 * ui_take_pop_request() returns true once if 's' is still on top. */
void ui_pop_request(struct ui_screen *s);
bool ui_take_pop_request(void);
struct ui_screen *ui_top(void);
int  ui_depth(void);

/* Screen lifetime (#38 fix round): a screen's slot is reused after it
 * leaves, so a pointer kept elsewhere (preview.c's leaf panel key) must
 * be dropped when its screen goes. The hook runs for every screen that
 * leaves (ui_pop(), ui_pop_to_root()), before its leave() frees its
 * data; one hook, NULL to clear. */
typedef void (*ui_leave_hook_fn)(struct ui_screen *s);
void ui_set_leave_hook(ui_leave_hook_fn fn);
/* P12: called after a push's enter() succeeded, with the screen now
 * covered and the pushed one's ops (the game SDK suspends a game that a
 * system screen covers). One hook; NULL clears it. */
typedef void (*ui_cover_hook_fn)(const struct ui_screen *under,
                                 const struct ui_screen_ops *by);
void ui_set_cover_hook(ui_cover_hook_fn fn);
/* True while 's' is on the stack (between its enter() and leave()). */
bool ui_screen_live(const struct ui_screen *s);

/* A split screen (list + panel, #38): its header covers only the list
 * (header_w 0 = full width). */
static inline bool ui_screen_is_split(const struct ui_screen *s)
{
    return s && s->header_w > 0;
}
void ui_invalidate(unsigned dirty);   /* mark top screen dirty */

/* Slide transitions: ui_push() records +1 (slide in from the right),
 * ui_pop() -1 (slide back). The main loop takes it before the new top
 * screen draws, while the framebuffer still shows the old one. */
int  ui_take_transition(void);
/* The kind of the transition ui_take_transition() last returned: a push
 * reports its own kind, a pop the kind the popped screen was pushed
 * with. */
enum ui_transition ui_transition_kind(void);

/* Partial redraws: mark a full-width y-band (LCD coordinates) of the top
 * screen dirty. Up to UI_BANDS_MAX disjoint bands are kept, so a jump of
 * the selection pushes only the old and the new row; overlapping or
 * touching bands merge, and a band beyond the limit merges into the
 * nearest one. */
#define UI_BANDS_MAX 2
void ui_invalidate_band(int y, int h);
/* The pending bands, sorted by y; returns how many (0 if none). The
 * arrays hold UI_BANDS_MAX entries. Cleared by ui_clear_dirty(). */
int ui_dirty_bands(int *y, int *h);
void ui_clear_dirty(void);

/* -- P11: the screen arena (docs/phase11-brief.md section 4.1) ---------- */

/* Screen states without core_alloc after shell start. One block of
 * UI_ARENA_SIZE, core_alloc'd and pinned in ui_main.c at shell start,
 * before anything can start playback (ui_arena_init()). Shares with
 * nothing (permanent; a budget line; can't share because it lives as
 * long as the stack).
 *
 * MARK-BASED (Rev 1 M-1). ui_push_t() records the arena top in
 * s->arena_mark BEFORE calling ops->enter(). After ops->leave() returns
 * (ui_pop, ui_pop_to_root, each level) AND after an enter() that fails,
 * screen_stack.c resets the top to that mark. So whatever a screen took
 * (one block or several) is released with it, a failed enter() leaks
 * nothing, and screens never free arena memory themselves. A pointer
 * from ui_arena_alloc() is valid until its screen leaves (the block is
 * pinned: it never moves).
 *
 * Fallback: a screen that needs more than ui_arena_free() may core_alloc
 * ONLY when audio_status() == 0, re-checked with no yield before the
 * allocation. Its leave() core_free()s the block and then calls
 * audio_buffer_regrow() (playback.h), so the next playback start sizes
 * the audio buffer anew.
 *
 * Trace (UPDATE_TRACE form, sim.log; docs/phase11-brief.md section 4.10):
 *   arena push s=<name> n=<bytes taken by enter()> free=<bytes left>
 *   arena full need=<bytes> free=<bytes>   (ui_arena_alloc() returned NULL;
 *                                           the screen does not log it again)
 */
#ifndef SIMULATOR
#define UI_ARENA_SIZE   9216        /* P12 W3-K: 8 KB + 1 KB, so a paged
                                       Notes list (8 KB) keeps the 1 KB the
                                       screens over it take (the charging
                                       screen, the lock) */
#else
/* #126: the simulator's mixer frames are 4x the device's (1,024 frames),
 * so the visualizer's tap ring is 16,431 B there against 4,143 on the
 * device: the simulator's arena carries the difference, so it takes the
 * device's path (the ring in the arena), never the fallback */
#define UI_ARENA_SIZE   (9216 + 12288)
#endif
#define UI_ARENA_ALIGN  8
/* Shell start (ui_main.c): the arena's memory, 'size' bytes at 'base'
 * (pinned; base is aligned up to UI_ARENA_ALIGN here, so pass
 * UI_ARENA_SIZE + UI_ARENA_ALIGN - 1 bytes for a full UI_ARENA_SIZE).
 * Without it every ui_arena_alloc() fails. A second call replaces the
 * arena (host tests); the stack must then hold no arena state. */
void   ui_arena_init(void *base, size_t size);
void  *ui_arena_alloc(size_t bytes);    /* UI_ARENA_ALIGN-aligned, zeroed,
                                           from the top; NULL if it doesn't
                                           fit (no fallback here) */
size_t ui_arena_free(void);             /* bytes an alloc could still get */
/* #126: give back the newest block of the top screen (p, bytes as
 * allocated: it must end at the arena's top, else logged and ignored):
 * a screen whose part comes and goes while it shows (the visualizer's
 * view in Now Playing) */
void     ui_arena_pop(void *p, size_t bytes);
/* screen_stack.c only (and host tests): */
uint16_t ui_arena_mark(void);
void     ui_arena_reset(uint16_t mark); /* mark <= top, else logged and
                                           ignored */

/* Per-state bounds (assert <= in each owner's file:
 * _Static_assert(sizeof(struct X) <= UI_ARENA_B_X, ...)). Measured at the
 * contract commit, target (ARM) / sim (64-bit): playlists 2,324 / 2,400,
 * browse 216 / 320, book page 148 / 224, choice 148 / 224. tests/arena
 * sums the deepest chains of section 4.1 from these bounds. */
#define UI_ARENA_B_PLAYLISTS    2408    /* screens/playlists.c (U1); +8 sim:
                                           #69 list has_mark */
#define UI_ARENA_B_BROWSE       512     /* screens/browse.c (U1) */
#define UI_ARENA_B_BOOK_PAGE    320     /* screens/audiobooks.c (U1) */
#define UI_ARENA_B_CHOICE       384     /* screens/settings.c choice (U2a) */
#define UI_ARENA_B_OTG          320     /* screens/otglist.c (U1) */
#define UI_ARENA_B_HOLD         264     /* screens/holdmenu.c (U1); +8 sim:
                                           the list's is_off (#132) */
#define UI_ARENA_B_SEARCH       2560    /* screens/search.c (U1) */
#define UI_ARENA_B_SETTING      384     /* checklist, roller screens, About,
                                           Date & Time lists, Brightness,
                                           EQ, Language, Reset (U2a/U2b) */
#define UI_ARENA_B_LEGAL        2048    /* screens/legal.c (U2b) */
#define UI_ARENA_B_CHARGING     64      /* sysscreen.c (U2b) */
#define UI_ARENA_B_VIZ          4640    /* viz.c (#126): the view's state
                                           (480 B ARM) and the tap's ring
                                           (viz_tap_bytes(512): 4,143 B);
                                           only while the view shows, given
                                           back before the style list */

#endif /* _IPODUI_SCREEN_STACK_H_ */
