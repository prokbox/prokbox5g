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
 * iPod UI shell: split-screen preview panels (#38 contracts)
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
#ifndef _IPODUI_PREVIEW_H_
#define _IPODUI_PREVIEW_H_

#include <stdbool.h>
#include <stddef.h>
#include "lcd.h"
#include "ipodui/nav_core.h"
#include "ipodui/screen_stack.h"

/* The panel of a split screen (spec main-menu.md "split screen", settings.md):
 * x 160-319, y 0-239, next to the 160 px list and its header. It shows a
 * preview of the highlighted item, which arrives by a ~200 ms cross-fade
 * once the selection has rested (0.4 s glyphs, 1.1 s covers); while the
 * wheel moves, the old panel stays (owner, Rev 6).
 *
 * Who does what:
 *  - providers (struct ui_preview_ops) draw one picture. Agent B's are in
 *    screens/panels.h, agent A's drift (ui_preview_drift) is below.
 *  - menus (menu.c) and split leaf screens report the highlighted item
 *    with ui_panel_select(); the scheduler (preview.c, agent A) waits for
 *    the rest, draws the provider into the strip and cross-fades it in,
 *    then runs the drift and any animated provider.
 */

/* -- the panel ---------------------------------------------------------- */

#define UI_PANEL_X  160             /* == ui_theme->split_x */
#define UI_PANEL_W  NAV_PANEL_W     /* 160 */
#define UI_PANEL_H  NAV_PANEL_H     /* 240 */

/* What a provider draws into. *vp is 160 x 240 with 0,0 at the panel's
 * top-left: either the LCD framebuffer (vp->x = 160, buffer NULL) or an
 * offscreen 160 x 240 RGB565 buffer in the strip (vp->x = 0, buffer set,
 * 'offscreen' true) that the scheduler then cross-fades in. Providers
 * never need to know which. vp->font is the theme's small font, drawmode
 * DRMODE_SOLID, fg/bg the theme's panel colours. The viewport belongs to
 * the scheduler; a provider may change its font, colours and drawmode
 * while it draws, never its geometry or buffer. */
struct ui_panel {
    struct viewport *vp;
    bool offscreen;
};

/* -- providers ---------------------------------------------------------- */

/* v1 arrives by cross-fade only (owner, Rev 6); SLIDE stays reserved. */
enum ui_panel_arrive {
    UI_PANEL_XFADE = 0,
    UI_PANEL_SLIDE,                 /* reserved, unused in v1 */
};

struct ui_preview_ops {
    const char *name;               /* trace and tests */
    /* Draws the whole panel for 'item_arg' (the item's preview_arg).
     * Rules (checked by the end review):
     *  - lcd_set_viewport(p->vp) first; draw only inside 0..159 x 0..239
     *    and cover every pixel (the cross-fade target must be complete);
     *  - no lcd_update*, no ui_lcd_push: the scheduler pushes;
     *  - no allocation: scratch comes from ui_panel_scratch();
     *  - no storage access while audio plays unless storage is awake
     *    (only the drift reads art; icon providers draw from RAM);
     *  - it may yield (a glyph load): the strip stays pinned around it;
     *  - 'phase' is ui_panel_phase() (lit ticks) for animated panels.
     * Returns ticks until it wants to draw again (an animated panel such
     * as the Extras clock: then drawn straight to the LCD, no fade, and
     * pushed as the 160 px panel band), or TIMEOUT_BLOCK. */
    int  (*draw)(const struct ui_panel *p, const void *item_arg, long phase);
    int  rest_delay_ticks;          /* NAV_REST_ICON or NAV_REST_COVER */
    enum ui_panel_arrive arrive;    /* UI_PANEL_XFADE */
};

/* Agent A's provider: the drifting album art of the panel set (Music, the
 * Music menu, Shuffle Songs, Now Playing; preview_arg NULL). Its draw()
 * shows the current cover at the current framing; the pan steps and
 * cover changes run in ui_panel_tick(). NAV_REST_COVER. */
extern const struct ui_preview_ops ui_preview_drift;
/* The same drift over the book covers (#58, owner): the Audiobooks rows
 * of the Main and Music menus. One drift slot for both: moving between a
 * music row and an Audiobooks row loads the other set's cover (one
 * 180,000 B read, only when storage may be read; plain until then, never
 * the other set's cover). */
extern const struct ui_preview_ops ui_preview_drift_books;
/* The main menu's Now Playing row (owner 2026-10-20): the playing
 * track's cover, still (no pan), from the panel set; the plain panel when
 * it has none there. A track change while the row is highlighted
 * redraws it once (ui_panel_tick()). NAV_REST_COVER. */
extern const struct ui_preview_ops ui_preview_nowplaying;
#ifdef HAVE_BCMFW_VIDEO
/* Phase 14a: the Videos rows drift over the posters (ARTCACHE_PSET_VIDEOS);
 * with none readable, the film strip and the count (ui_panel_draw_videos) */
extern const struct ui_preview_ops ui_preview_videos;
#endif

/* -- panel scratch ------------------------------------------------------ */

/* One block reserved at shell start (ui_preview_reserve()), for providers
 * that need a work buffer while they draw. 8b-3: no provider needs one
 * (panels.c composes in place), so the size is 0 and nothing is
 * reserved: ui_panel_scratch() returns NULL and every
 * UI_PANEL_SCRATCH_CHECK() of a non-zero need fails to compile. To bring
 * it back, set UI_PANEL_SCRATCH_SIZE (16384 before 8b-3). Movable; valid only inside draw() (pinned by the
 * scheduler around the call). Can't share with the strip: the strip holds
 * the panel being drawn. Can't share with the drift slot: it holds the
 * current cover, which keeps drifting.
 *
 * Size declaration pattern: each provider header defines its need,
 *     #define UI_PANEL_SCRATCH_<NAME>  <bytes>
 * and its .c checks it once at file scope:
 *     UI_PANEL_SCRATCH_CHECK(UI_PANEL_SCRATCH_<NAME>);
 * Raising UI_PANEL_SCRATCH_SIZE is agent A's call (a budget line). */
#define UI_PANEL_SCRATCH_SIZE   0       /* 16384 before 8b-3 */
#define UI_PANEL_CAT_(a, b) a##b
#define UI_PANEL_CAT(a, b)  UI_PANEL_CAT_(a, b)
#define UI_PANEL_SCRATCH_CHECK(n) \
    typedef char UI_PANEL_CAT(ui_panel_scratch_check_, __LINE__)[ \
        (n) <= UI_PANEL_SCRATCH_SIZE ? 1 : -1] __attribute__((unused))

/* The scratch and its size (>= UI_PANEL_SCRATCH_SIZE), or NULL if there
 * is none (size 0) or the reservation failed (draw something plain then).
 * Inside draw() only. */
void *ui_panel_scratch(size_t *size);

/* -- the scheduler (preview.c, agent A) --------------------------------- */

/* Shell start, before any playback (ui_main.c, next to ui_strip_init()):
 * the drift slot (one 300 x 300 RGB565 image, 180,000 B) and the scratch.
 * False if memory was short: panels then stay plain panel_bg. */
bool ui_preview_reserve(void);

/* A split screen's highlighted item, after every selection move and on
 * every push or pop that lands on a split screen. 'ops' NULL: no preview,
 * the old panel stays. The rest delay restarts at each call
 * (nav_sched_select()). */
void ui_panel_select(const struct ui_preview_ops *ops, const void *item_arg);
/* The top screen is a split leaf with draw_into_panel (screen_stack.h):
 * key the panel on it (rest NAV_REST_ICON). */
void ui_panel_select_screen(struct ui_screen *s);
/* Same item, new content (a leaf changed its value): cross-fades again
 * after the rest delay. */
void ui_panel_invalidate(void);
/* The top screen is not split (full-width list, WPS): the scheduler
 * idles, the phase keeps counting lit time. */
void ui_panel_hide(void);
/* Full repaint of a split screen (boot, WPS return, a push without the
 * strip): draws the wanted panel straight into the framebuffer, no fade;
 * the caller pushes it with the rest of the screen. */
void ui_panel_draw_now(void);
/* After a full draw: whether the panel ui_panel_draw_now() drew differs
 * from what the LCD shows (another row's panel, new content, a fade cut
 * short, or nothing valid there). A caller that pushed the whole screen
 * passes y = LCD_HEIGHT (nothing more to push); a slide that moves only
 * the list column passes its rect's top: the panel's rows from y down
 * are pushed once if they changed (the header push covers those above;
 * #58: else the LCD kept the old panel below the header). */
void ui_panel_sync(int y);
/* P19 screen truth (phase19-brief.md 3.6): before a CPU read or push of
 * the framebuffer rect (x, y, w, h) as "what is on screen".  If the BCM
 * holds the drift panel's pixels there, the CPU's own framing is written
 * back and pushed; otherwise it returns at once. */
void ui_fb_settle(int x, int y, int w, int h);
/* Main loop, every pass while a split screen is on top and no slide
 * runs: the due arrival cross-fade (boosted, NAV_PANEL_XFADE_STEPS
 * frames), drift steps (unboosted, lit only) and animated providers. It
 * pushes its own rects. Returns UI_TICK_ANIM while a highlight fade
 * runs, else ticks until its next work, or TIMEOUT_BLOCK. */
int  ui_panel_tick(void);
/* p19-battery: a drift step drawn and pushed by the CPU within the last
 * second (the lit clock policy's stream: clockpol.c); false while the
 * BCM's jobs draw it, or for the still cover */
bool ui_panel_drift_cpu(void);

/* The panel phase: lit-time ticks since shell start (nav_phase_update()),
 * never reset by a push, Cover Flow or the WPS. */
long ui_panel_phase(void);
/* Main loop, every pass (any top screen): brings the phase up to date
 * with the backlight state, so dark stretches never count. Cheap. */
void ui_panel_phase_poll(void);
#ifdef SIMULATOR
/* C's drift_phase script command: pins the phase (goldens, videos). */
void ui_panel_phase_set(long ticks);
/* Scripted runs start with the phase frozen (no drift steps, no cycle
 * changes, so main-menu goldens never depend on timing); "# drift: live"
 * or drift_phase un-freeze it, "drift_phase <ticks> frozen" re-freezes. */
void ui_panel_phase_freeze(bool frozen);
#endif

/* -- strip borrowing (Rev 5 R1; enforced by preview.c, tested by the pure
 *    nav_drift_plan() in tests/nav) ------------------------------------
 *
 * The strip (anim.h) is one block with a "last claimer" owner. preview.c
 * uses two owner ids and never allocates:
 *  UI_STRIP_OWNER_PANEL  a highlight arrival: claimed (pinned) around the
 *     provider's draw() into the offscreen panel, then once per fade step;
 *     if a step finds it not 'kept' (a glide or slide used the strip in
 *     between), the panel is drawn straight to the LCD and pushed (the
 *     fallback; logged), never left half-faded.
 *  UI_STRIP_OWNER_DRIFT  the next cover, prefetched while
 *     nav_drift_plan() says NAV_DRIFT_PREFETCH: audio playing, storage
 *     already awake, the panel at rest (no glide, slide or highlight
 *     fade), >= NAV_DRIFT_PREFETCH_FROM into the dwell. One
 *     artcache_panel_get() straight into the strip; the strip is pinned
 *     across that one 180,000 B read (the UI thread is the strip's only
 *     user, so nothing else can wait on it). Unpinned afterwards and
 *     between all ticks. The other pins held across I/O, on the same
 *     terms (UI thread only, pinned across one draw's reads, never
 *     across a tick): the Now Playing row's np_art_draw() (the thumbnail
 *     read behind the offscreen panel) and np_panel_draw() (the panel
 *     window's reads, artcache_panel_window()), both inside the PANEL
 *     claim around the provider's draw().
 *     At the cycle end the drift claims it again: 'kept' means the next
 *     cover is intact -> NAV_DRIFT_FADE_STRIP (fade the LCD panel toward
 *     the strip window in NAV_DRIFT_FADE_STEPS, then memcpy the strip
 *     into the drift slot). Not kept (any glide, slide or highlight fade
 *     since the prefetch took it): NAV_DRIFT_FADE_DIRECT if storage may
 *     be read (stopped, or awake): read the next cover straight into the
 *     drift slot -- the old one is still on the LCD -- and fade toward
 *     it; else NAV_DRIFT_REDWELL: fade to the same cover at the new
 *     cycle's framing (new direction). Stopped, no prefetch is made at
 *     all: the cycle end reads directly.
 * A list glide or a slide may take the strip at any time; the panel and
 * drift users always re-check 'kept' and fall back as above.
 *
 * Exclusivity: the panel and drift code claim the strip ONLY while a
 * split screen is on top (after ui_panel_hide() they claim nothing, and
 * ui_panel_tick() is not called). While a full-screen screen is on top
 * -- Now Playing above all, which keeps its own data in the strip under
 * UI_STRIP_OWNER_NP at fixed offsets (Phase 5) -- neither PANEL nor
 * DRIFT is ever claimed; the drift phase keeps counting and the next
 * cycle change after the return is a fade (nav_drift_plan()). preview.c
 * traces each claim as "panel strip claim owner=<PANEL|DRIFT> top=<screen
 * name>"; C's trace rule fails any such line whose top is not a split
 * screen.
 * Fades blend the LCD panel in place toward the target
 * (nav_xfade_weight(), nav_xfade_chan()), so no copy of the old picture
 * is kept anywhere. */

#endif /* _IPODUI_PREVIEW_H_ */
