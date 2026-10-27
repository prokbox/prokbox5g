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
 * iPod UI shell: navigation model, pure logic (#38 contracts)
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
#ifndef _IPODUI_NAV_CORE_H_
#define _IPODUI_NAV_CORE_H_

#include <stdbool.h>

/* The pure parts of the #38 navigation model: panel rest scheduling, the
 * lit-time panel phase, the drift framing and its strip plan, cross-fade
 * weights and transition timing. No Rockbox headers, no globals: host
 * tested by tests/nav (docs/phase-nav-brief.md, "Contracts (committed)").
 * preview.c and ui_main.c (agent A) wrap these with the LCD, the strip
 * and the art cache. All times are kernel ticks at NAV_HZ (preview.c
 * asserts NAV_HZ == HZ). */

#define NAV_HZ          100
#define NAV_NEVER       (-1L)       /* == TIMEOUT_BLOCK: nothing due */

/* -- panel rest scheduling ---------------------------------------------- */

/* Rest delays (spec main-menu.md, VIDEO-2.md §C): the new panel arrives
 * only after the selection has rested this long; while the wheel moves
 * the old panel stays (owner, Rev 6). */
#define NAV_REST_ICON   (NAV_HZ * 4 / 10)   /* 0.4 s: glyph/tile panels */
#define NAV_REST_COVER  (NAV_HZ * 11 / 10)  /* 1.1 s: cover (drift) panels */

/* Highlight arrival: a ~200 ms cross-fade (spec), boosted like any
 * response animation, NAV_PANEL_XFADE_STEPS frames. */
#define NAV_PANEL_XFADE_TICKS  (NAV_HZ / 5)
#define NAV_PANEL_XFADE_STEPS  6
#define NAV_XFADE_STEPS_MAX    8            /* Rev 1: CPU cap per fade */

/* A panel is identified by its key: the provider (or a leaf screen's ops)
 * and its argument. Same key = same picture, nothing to do (moving
 * between two Music items keeps the drift running untouched). A NULL
 * 'ops' key means "no preview": the old panel stays. */
struct nav_panel_key {
    const void *ops;
    const void *arg;
};

struct nav_panel_sched {
    struct nav_panel_key shown;     /* on screen (ops NULL: blank) */
    struct nav_panel_key want;      /* the highlighted item's */
    long moved;                     /* tick of the last select/invalidate */
    int  rest;                      /* rest delay of 'want' */
    bool pending;                   /* want must arrive once rested */
};

void nav_sched_init(struct nav_panel_sched *s);
/* The highlighted item changed, or a split screen was pushed or popped
 * to: the rest delay restarts from 'now' (after a push the panel shows
 * nothing new until it has passed, Rev 1). A NULL key ops keeps the
 * shown panel. */
void nav_sched_select(struct nav_panel_sched *s, const void *ops,
                      const void *arg, int rest_ticks, long now);
/* Same key, new content (a leaf changed its value): arrives again after
 * the rest delay, by cross-fade. */
void nav_sched_invalidate(struct nav_panel_sched *s, long now);
/* Ticks until the pending arrival is due: 0 = now, NAV_NEVER = none. */
long nav_sched_due(const struct nav_panel_sched *s, long now);
/* The arrival ran (or was drawn in full): shown := want. */
void nav_sched_arrived(struct nav_panel_sched *s);
/* Full repaint (boot, WPS return): nothing on screen any more, so the
 * wanted key is drawn at once without a fade; the caller then calls
 * nav_sched_arrived(). */
void nav_sched_forget(struct nav_panel_sched *s);

/* -- panel phase: lit-time ticks ---------------------------------------- */

/* One global clock for every animated panel (drift, Extras clock),
 * counted only while the backlight is lit, never reset by a push,
 * Cover Flow or the WPS (spec: "the cycle is not reset"). */
struct nav_phase {
    long ticks;                     /* lit ticks since shell start */
    long last;                      /* tick of the last update */
    bool lit;                       /* lit at 'last' */
};

void nav_phase_init(struct nav_phase *ph, long now, bool lit);
/* Adds now - last if the backlight was lit since 'last' (the state the
 * previous call reported); 'lit' is the state from now on. Returns the
 * phase. Call from the UI thread at each panel tick and at every
 * backlight change. */
long nav_phase_update(struct nav_phase *ph, long now, bool lit);
/* SIMULATOR drift_phase command (tests pin the phase for goldens). */
void nav_phase_set(struct nav_phase *ph, long ticks, long now);

/* -- drift (pan only, owner 2026-10-07) --------------------------------- */

/* Each cover dwells NAV_DRIFT_CYCLE ticks (7 s). A cycle starts with the
 * NAV_DRIFT_FADE (400 ms) cross-fade from the previous framing, then pans
 * 1 px every NAV_DRIFT_STEP ticks (6.7 px/s) in the cycle's direction:
 * 44 px over the dwell. Each pixel is shown in two half steps (8b, owner:
 * smoother panning at the same speed): the whole-pixel framing, then,
 * NAV_DRIFT_HALF ticks later, the half-way blend toward the next one. The 160x240 window moves inside the 300x300
 * ARTCACHE_PANEL image, centred on the cover's middle. */
#define NAV_DRIFT_CYCLE     (NAV_HZ * 7)
#define NAV_DRIFT_FADE      (NAV_HZ * 4 / 10)
#define NAV_DRIFT_FADE_STEPS 8
#define NAV_DRIFT_STEP      15
#define NAV_DRIFT_HALF      ((NAV_DRIFT_STEP + 1) / 2)  /* 8: 80 + 70 ms */
#define NAV_DRIFT_SRC       300         /* ARTCACHE_PANEL side */
#define NAV_PANEL_W         160
#define NAV_PANEL_H         240
#define NAV_DRIFT_PAN_MAX   ((NAV_DRIFT_CYCLE - NAV_DRIFT_FADE) / NAV_DRIFT_STEP)
/* Strip prefetch of the next cover (Rev 5 R1) starts this far into a
 * dwell, so a push or a glide right after an arrival has settled. */
#define NAV_DRIFT_PREFETCH_FROM (NAV_HZ * 2)

struct nav_drift_pos {
    long cycle;                     /* phase / NAV_DRIFT_CYCLE */
    int  t;                         /* ticks into the cycle */
    int  fade_step;                 /* 1..NAV_DRIFT_FADE_STEPS while fading,
                                       0 once panning */
    int  pan;                       /* px panned so far, 0..PAN_MAX */
    int  half;                      /* 1: half-way to pan + 1 (< PAN_MAX) */
    int  x, y;                      /* window's top-left in the source */
    int  dx, dy;                    /* the cycle's direction, -1..1 each */
};
/* The pan position in half steps: what nav_drift_plan() compares */
#define NAV_DRIFT_SUB(p)    ((p)->pan * 2 + (p)->half)

void nav_drift_pos(long phase, struct nav_drift_pos *out);
/* Ticks until the drift picture next changes (next fade step, pan half
 * step or cycle start), > 0. */
int nav_drift_wake(long phase);

/* What the drift does at a tick (preview.c acts on it). The "next" cover
 * lives in the list strip (Rev 5 R1; owner id UI_STRIP_OWNER_DRIFT), the
 * current one in the drift slot. */
enum nav_drift_act {
    NAV_DRIFT_IDLE = 0,     /* nothing visible changes */
    NAV_DRIFT_PAN,          /* blit the slot window at the new offset */
    NAV_DRIFT_PREFETCH,     /* read the next cover into the strip */
    NAV_DRIFT_FADE_STRIP,   /* fade fb -> strip (next cover), then copy
                               the strip into the slot; cover + 1 */
    NAV_DRIFT_FADE_DIRECT,  /* no prefetch kept: read the next cover
                               straight into the slot (the old one is
                               still on screen), fade fb -> slot; cover + 1 */
    NAV_DRIFT_REDWELL,      /* storage asleep or one cover: fade fb ->
                               the same cover at the new cycle's framing */
};

struct nav_drift_in {
    long cycle;                     /* nav_drift_pos().cycle now */
    long shown_cycle;               /* cycle whose framing is on screen */
    int  pan, shown_pan;            /* pan due / on screen, half steps
                                       (NAV_DRIFT_SUB()) */
    int  t;                         /* ticks into the cycle */
    bool at_rest;                   /* drift panel shown on the top screen;
                                       no glide, slide or highlight fade */
    bool storage_ok;                /* stopped, or storage already awake */
    bool playing;                   /* audio playing or paused */
    bool next_kept;                 /* the strip still holds the prefetch
                                       (last ui_strip_claim(DRIFT) kept) */
    int  panel_count;               /* artcache_panel_count() */
};

enum nav_drift_act nav_drift_plan(const struct nav_drift_in *in);

/* -- cross-fades -------------------------------------------------------- */

/* Fades blend the framebuffer's panel (the old picture) toward the target
 * in place: step k of n moves each channel by weight/256 of the way left,
 * so no copy of the old picture is kept and the last step lands exactly
 * on the target. Returns weight in 1/256 (k in 1..n). */
int nav_xfade_weight(int k, int n);
/* One channel step, rounded to nearest (the reference for preview.c's
 * RGB565 loop): never overshoots, the steps stay within 1.25 levels of a
 * linear fade (tests/nav), weight 256 lands exactly. A step that misses
 * a frame still converges: the next weight moves the right share of
 * what is left. */
static inline int nav_xfade_chan(int cur, int target, int weight)
{
    int d = (target - cur) * weight;
    return d >= 0 ? cur + ((d + 128) >> 8) : cur - ((-d + 128) >> 8);
}

/* -- transitions -------------------------------------------------------- */

/* Durations (spec index.md "Transitions"): slides 190 ms ease-out;
 * split -> full-screen pushes 250 ms, their pops 290 ms; reveal 250 ms
 * both ways; split (Cover Flow) 210 ms in, 290 ms out. dir: +1 push,
 * -1 pop. 'kind' is enum ui_transition (screen_stack.h) as an int. */
int nav_transition_ticks(int kind, int dir, bool from_split, bool to_split);

/* The Extras > Clocks reveal's motion (P12 W1-C; measured, owner video
 * VIDEO-2 A3; wired into ui_main.c by W2-W). The world map is a fixed
 * backdrop. Into Clocks (dir +1), at elapsed ticks of total: the old
 * foreground (header, list, the panel's clock and date) has moved *out_dx
 * px left, faster than the page (it clears 320 px at 80 % of the time:
 * the measured parallax), and the Clocks page's bands stand with their
 * left edge at *in_x (320 -> 0, x = 320 (1 - t)(1 - 0.4 t): the measured
 * 245/183/125/81/22/0 px at 40/80/130/170/210/250 ms, within 12 px). Back (dir -1) is
 * the exact mirror: the same pair at total - elapsed. Both clamp to the
 * ends outside 0..total. */
void nav_reveal_offsets(int elapsed, int total, int dir, int *out_dx,
                        int *in_x);

#endif /* _IPODUI_NAV_CORE_H_ */
