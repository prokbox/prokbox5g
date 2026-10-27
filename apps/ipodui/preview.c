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
 * iPod UI shell: split-screen preview panels, the scheduler and the drift
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

/* The panel of a split screen (preview.h; docs/phase-nav-brief.md,
 * "Contracts (committed)", D1-D5, D9, D15). The pure parts (rest timing,
 * phase, drift framing and plan, fade weights) are nav_core.c; this file
 * wraps them with the LCD, the strip, the drift slot and the art cache.
 *
 * Memory: two movable blocks reserved once at shell start, before any
 * playback (ui_preview_reserve()): the drift slot (the current cover,
 * 300 x 300 RGB565, 180,000 B) and the panel scratch (UI_PANEL_SCRATCH_SIZE,
 * 0 since 8b-3: nothing reserved); 8b-4a's second slot, and #92's
 * banks (PV_COVERS - 1 more covers per set, 360,448 B per set at 3
 * covers; none on 32 MB). The
 * next cover and a highlight fade's new panel borrow the strip (anim.h).
 * Nothing allocates afterwards. Statics: one small state struct, the
 * panel viewport and its frame buffer descriptor (< 200 B).
 *
 * CPU: a highlight fade is boosted (a response to input, lean rule 6);
 * the drift never boosts: a pan step is a 160 x 240 blit (whole pixel)
 * or half-pixel blend (half_window()) and a push every 75 ms, a cover
 * fade 8 in-place blends 50 ms apart; all of it only while lit and at
 * rest. */

#include "config.h"
#include <string.h>
#include <limits.h>
#ifdef SIMULATOR
#include <stdlib.h>
#endif
#include "system.h"
#include "kernel.h"
#include "lcd.h"
#include "audio.h"
#include "storage.h"
#include "backlight.h"
#include "core_alloc.h"
#include "logf.h"
#include "panic.h"
#include "timefuncs.h"
#include "ipodui/paint.h"
#include "ipodui/theme.h"
#include "ipodui/anim.h"
#include "ipodui/ptrace.h"
#include "ipodui/artcache.h"
#include "ipodui/preview.h"
#include "ipodui/library.h"
#include "ipodui/libfile.h"
#include "ipodui/drift_pick.h"
#include "ipodui/screens/panels.h"
#include "ipodui/uilog.h"
#include "ipodui/material.h"
#include "ipodui/np_playback.h"
#include "ipodui/drift_jobs.h"
#include "ipodui/art_bcm.h"
#include "ipodui/litmiss.h"
#include "ipodui/mq_jobs.h"
#ifdef HAVE_DRIFT_JOBS
#include "bcmjob_pp.h"
#endif

#define SLOT_SIZE   (NAV_DRIFT_SRC * NAV_DRIFT_SRC * sizeof(fb_data))
#define PANEL_SIZE  (UI_PANEL_W * UI_PANEL_H * sizeof(fb_data))
#define DF_STEP     (NAV_DRIFT_FADE / NAV_DRIFT_FADE_STEPS)  /* 5 ticks */
/* #52: the first cover stands still this long (boot settles under a
 * static cover) before the drift starts; ~ owner "after ~1-2 s" */
#define DRIFT_HOLD  (HZ * 3 / 2)
/* #73 follow-up (owner, device): a set's first cover (a set change) holds
 * still through its fade and this much more while the next cover is
 * prefetched, so the pan starts from its beginning and never catches up;
 * HOLD_CAP bounds any hold */
#define HOLD_STILL  (HZ / 2)
#define HOLD_CAP    (HZ * 2)

/* #92 (Q5): covers per set kept resident: the slot (or the second one)
 * plus a bank of PV_COVERS - 1 more per set (music, books), taken at
 * shell start only. 64 MB: 3. 32 MB: 1 (the bank off, 8b's two slots):
 * the 32 MB library-commit margin cannot spare 360 KB (Q5's rule). */
#ifndef PV_COVERS
#if MEMORYSIZE >= 64
#define PV_COVERS   3
#else
#define PV_COVERS   1
#endif
#endif
#define PV_BANK     (PV_COVERS - 1)     /* covers in a set's bank */
#define PV_BANK_SETS 2                  /* music, books */
#define PV_BANK_CAP (PV_BANK > 0 ? PV_BANK : 1)

#define PV_ASSERT(name, c) \
    typedef char pv_assert_##name[(c) ? 1 : -1] __attribute__((unused))
PV_ASSERT(hz, NAV_HZ == HZ);
PV_ASSERT(slot_in_strip, SLOT_SIZE <= UI_STRIP_SIZE);    /* the prefetch */
PV_ASSERT(panel_in_strip, PANEL_SIZE <= UI_STRIP_SIZE);  /* a highlight */
/* the Now Playing row's fallback thumbnail: read behind the panel */
PV_ASSERT(np_thumb_in_strip, PANEL_SIZE + 55 * 55 * 2 <= UI_STRIP_SIZE);
PV_ASSERT(panel_x, UI_PANEL_X + UI_PANEL_W == LCD_WIDTH);
PV_ASSERT(src, NAV_DRIFT_SRC >= UI_PANEL_H);
/* a banked cover's pixels are a slot's, at the slot stride of panels.pak */
PV_ASSERT(bank_stride, SLOT_SIZE + 16 <= ARTCACHE_PANEL_SLOT_BYTES
                       && SLOT_SIZE % 4 == 0);
PV_ASSERT(bank_run, PV_BANK <= ARTCACHE_PANEL_RUN_MAX);
PV_ASSERT(bank_sets, ARTCACHE_PSET_MUSIC == 0 && ARTCACHE_PSET_BOOKS == 1);
/* half_window(): word-aligned rows, and an odd x never at the last one */
PV_ASSERT(half_words, NAV_DRIFT_SRC % 2 == 0 && UI_PANEL_W % 2 == 0
                      && UI_PANEL_X % 2 == 0 && LCD_WIDTH % 2 == 0
                      && (NAV_DRIFT_SRC - UI_PANEL_W) % 2 == 0);

static struct {
    struct nav_panel_sched sched;
    struct nav_phase phase;
    bool want_leaf, shown_leaf;     /* key is a split leaf screen */
    bool visible;                   /* a split screen is on top */
    bool fb_valid;                  /* the LCD panel shows 'shown' */
    bool lcd_stale;                 /* draw_now changed it, not pushed */
    signed char ps_kind;            /* the arrival slides (8b-4a): +1 the
                                       slideshow out to the right, -1 in
                                       from the right, 0 a cross-fade */
    short ps_done;                  /* columns moved so far, 0..160 */
    long ps_start;                  /* tick of its first frame */
    signed char hl_k;               /* highlight fade: steps done, 0 idle,
                                       -1 started, no frame yet */
    bool anim;                      /* the shown provider redraws itself */
    long anim_due;                  /* ... at this tick */
    int  slot;                      /* drift slot handle, 0 = none */
    /* 8b-4a (owner-approved hybrid): a second slot holding the last
     * cover of the set not shown, so a Music <-> Audiobooks switch is a
     * handle swap (no I/O, no wake). 0 = none (memory short): one slot,
     * as before. slot_swap() exchanges it with the fields below. */
    int  slot2;
    signed char set2;               /* its cover's set, -1 none */
    short cover2;                   /* its panel index, -1 none */
    int  count2;                    /* its set's count, last read */
    int  seq;                       /* artcache_panel_seq() when the
                                       slot's cover was read */
    int  seq2;                      /* ... the second slot's */
    uint64_t cover_key2;
#ifdef HAVE_BCMFW_VIDEO
    short cw2;
#endif
    /* #92: each set's bank, PV_BANK more covers read in one read at a
     * natural disk wake (artcache_panel_run()); while storage sleeps the
     * drift rotates them through the slot (an in-place swap, no I/O).
     * Valid only while panels.pak's sequence is the one it was read
     * under: a stale cover is never swapped in. 0 = none (the 8b
     * drift: one cover per set while storage sleeps). */
    int  bank[PV_BANK_SETS];        /* handles */
    uint64_t bank_key[PV_BANK_SETS][PV_BANK_CAP];
    /* each banked cover's index in its set, exact (artcache_panel_run()
     * reports it) and valid while the bank is (the set's order changes
     * only with a panels.pak sequence bump) */
    short bank_idx[PV_BANK_SETS][PV_BANK_CAP];
#ifdef HAVE_BCMFW_VIDEO
    short bank_cw[PV_BANK_SETS][PV_BANK_CAP];
#endif
    int  bank_seq[PV_BANK_SETS];    /* artcache_panel_seq() at the read */
    signed char bank_n[PV_BANK_SETS];   /* covers held, 0 none */
    signed char bank_rot[PV_BANK_SETS]; /* where the next search starts */
    signed char bank_used[PV_BANK_SETS];/* -1: the read under bank_seq
                                           found none (not again) */
    unsigned char bank_seen[PV_BANK_SETS]; /* covers shown since the read
                                           (bit j: bank entry j) */
    int  scratch;                   /* panel scratch handle, 0 = none */
    int  count;                     /* the set's panel count, last read */
    signed char set;                /* panel set of the slot's cover, -1 */
    signed char next_set;           /* ... of the prefetch */
    signed char woke_set;           /* set read with storage asleep (#73) */
    bool holding;                   /* no other cover: the held pan (#58) */
    bool half_off;                  /* half steps dropped: too slow here */
    signed char half_slow;          /* slow half steps in a row */
    short cover;                    /* panel index in the slot, -1 none */
    short next;                     /* panel index prefetched, -1 none */
    uint64_t cover_key;             /* the slot's cover (0: none yet) */
    uint64_t next_key;              /* the prefetch's */
    int next_seq;                   /* artcache_panel_seq() at its read */
    long shown_cycle;               /* drift cycle on screen */
    short shown_pan;                /* its pan on screen, half steps
                                       (NAV_DRIFT_SUB()), -1 none */
    short fx, fy;                   /* framing on screen (or faded to) */
    short hx, hy;                   /* held pan: its origin ... */
    signed char hdx, hdy;           /* ... and direction */
    signed char df_k;               /* drift fade: steps done, 0 idle */
    long df_start;                  /* tick the drift fade began */
    long prefetch_failed;           /* cycle whose prefetch failed */
    uint32_t np_track;              /* np_pb_track() the Now Playing
                                       row's cover was drawn for */
    uint64_t np_woke;               /* its key, read with storage asleep */
    bool np_user;                   /* the row was just highlighted: one
                                       wake read allowed (8b-4a M1) */
    bool np_deferred;               /* drawn plain: storage was asleep */
    bool np_sharp_wait;             /* np-sharp: the stand-in is up, its
                                       panel queued (artcache_npcover_queue()) */
    int np_sharp_seq;               /* artcache_npcover_seq() then */
    long hold_until;                /* first cover still until, 0 none */
    long hold_start;                /* ... since */
    bool hold_prep;                 /* ... the next cover not tried yet */
#ifdef HAVE_DRIFT_JOBS
    /* P19 19-2: the last frame sent as jobs, for drift_settle(): a half
     * step's second framing (sx1, sy1), the whole-pixel one is fx, fy */
    bool shalf;
    short sx1, sy1;
    /* P19 19-6 (the end state): the Now Playing row's panel is the BCM's
     * resident slot (np_bcm: the glass shows it, the framebuffer holds
     * nothing of it), arriving by its fade (np_arrive: that album) */
    bool np_bcm;
    bool np_settling;               /* np_bcm_settle()'s own redraw */
    uint64_t np_arrive;
    uint64_t np_bcm_key;            /* the album np_bcm shows */
    uint64_t np_res_asked;          /* the re-decode queued for this album */
    uint32_t np_res_ep;             /* ... in this epoch */
#endif
#ifdef HAVE_BCMFW_VIDEO
    short cw;                       /* the slot's picture width, 0 = all
                                       (artcache_panel_get_cw()) */
    short next_cw;                  /* ... the prefetch's */
#endif
} pv = { .cover = -1, .next = -1, .set = -1, .next_set = -1,
         .set2 = -1, .cover2 = -1,
         .woke_set = -1,
         .prefetch_failed = -1 };

/* the panel's viewport: the LCD keeps a pointer to the current one */
static struct viewport pvp;
static struct frame_buffer_t pfb;

/* -- helpers ------------------------------------------------------------ */

static inline const char *top_name(void)
{
    struct ui_screen *s = ui_top();
    return s ? s->ops->name : "-";
}

/* #73: every read, deferral and skip of the drift slot, and every plain
 * (coverless) drift draw: check_trace.py's "panelset" rule */
#define SET_TRACE(set, why) \
    UPDATE_TRACE("panel set read set=%d why=%s", (int)(set), (why))

/* Every claim by this file is traced (D15): C fails one whose top screen
 * is not split. Only called while pv.visible. */
static fb_data *claim(int owner, bool *kept)
{
    UPDATE_TRACE("panel strip claim owner=%s top=%s",
                 owner == UI_STRIP_OWNER_DRIFT ? "DRIFT" : "PANEL",
                 top_name());
    return ui_strip_claim(owner, kept);
}

/* Storage may be read: stopped, or already spinning/awake (artcache.md
 * §7). Paused counts as playing: the buffer is full, the disk asleep.
 * The simulator (hostfs) has no disk state: the art cache's model of it
 * (artcache.c, the "disk_active <ms>" script command) stands in. */
#ifdef SIMULATOR
extern long sim_disk_active_until;
static bool disk_awake(void)
{
    return sim_disk_active_until
        && TIME_BEFORE(current_tick, sim_disk_active_until);
}
#else
#define disk_awake() storage_disk_is_active()
#endif

static bool storage_ok(void)
{
    return !audio_status() || disk_awake();
}

static void push_panel(const char *kind)
{
    ui_lcd_push(kind, UI_PANEL_X, 0, UI_PANEL_W, UI_PANEL_H);
    pv.lcd_stale = false;
}

/* In-place cross-fade step: each fb pixel moves 'weight'/256 of the way
 * toward src (nav_xfade_weight()). Packed 0x07e0f81f form with the weight
 * in 1/32 (5 bits: the packed fields have room for x32), rounded; weight
 * 256 is an exact copy. Per pixel: 2 loads, 1 compare, ~10 ALU, 1 store;
 * equal pixels (flat areas, a mostly unchanged picture) are skipped. */
static void __attribute__((noinline))
xfade_rect(fb_data *dst, int dstride, const fb_data *src, int sstride,
           int w, int h, int weight)
{
    unsigned w32 = (weight + 4) >> 3, iw = 32 - w32;
    int x;

    if (weight >= 256)
    {
        for (; h > 0; h--, dst += dstride, src += sstride)
            memcpy(dst, src, w * sizeof(fb_data));
        return;
    }
    if (w32 == 0)
        w32 = 1, iw = 31;
    for (; h > 0; h--, dst += dstride, src += sstride)
    {
        for (x = 0; x < w; x++)
        {
            unsigned d = dst[x], s = src[x], r;
            if (d == s)
                continue;
            d = (d | (d << 16)) & 0x07e0f81f;
            s = (s | (s << 16)) & 0x07e0f81f;
            r = ((d * iw + s * w32 + 0x02008010) >> 5) & 0x07e0f81f;
            dst[x] = r | (r >> 16);
        }
    }
}

/* -- drawing a key ------------------------------------------------------ */

/* No art yet (a fresh library has no panels.pak until the art cache is
 * built, #52): the panel gradient, never an empty dark panel */
static void plain(void)
{
    ui_paint_panel_bg(0, UI_PANEL_H);
}

/* The key of a split leaf that has left (screen_gone()): its data is
 * freed and its slot may be reused, so it is never called again. What it
 * drew is still on the LCD, and stays there (a pop between split screens
 * keeps the panel until the parent's item has rested, D1); drawn
 * offscreen (a fade source) it is plain. */
static int gone_draw(const struct ui_panel *p, const void *arg, long phase)
{
    (void)arg;
    (void)phase;
    if (p->offscreen)
        plain();
    return TIMEOUT_BLOCK;
}

static const struct ui_preview_ops gone_ops = {
    .name = "left", .draw = gone_draw,
    .rest_delay_ticks = NAV_REST_ICON, .arrive = UI_PANEL_XFADE,
};

/* Draws key 'k' into the LCD panel ('off' NULL) or the 160 x 240 buffer
 * 'off' (the strip, pinned by the caller). Returns the provider's wish
 * (ticks until it redraws, or TIMEOUT_BLOCK). */
static int draw_key(const struct nav_panel_key *k, bool leaf, fb_data *off)
{
    const struct ui_theme *t = ui_theme;
    struct ui_panel p;
    int ret = TIMEOUT_BLOCK;

    memset(&pvp, 0, sizeof(pvp));
    pvp.x = off ? 0 : UI_PANEL_X;
    pvp.width = UI_PANEL_W;
    pvp.height = UI_PANEL_H;
    pvp.font = t->font_small;
    pvp.drawmode = DRMODE_SOLID;
    pvp.fg_pattern = t->list_fg;
    pvp.bg_pattern = t->panel_bg;
    if (off)
    {
        memset(&pfb, 0, sizeof(pfb));
        pfb.fb_ptr = off;
        pfb.stride = STRIDE_MAIN(UI_PANEL_W, UI_PANEL_H);
        pfb.elems = LCD_NBELEMS(UI_PANEL_W, UI_PANEL_H);
        pvp.buffer = &pfb;
    }
    p.vp = &pvp;
    p.offscreen = off != NULL;
    lcd_set_viewport(&pvp);

    if (!k->ops)
        plain();
    else
    {
#ifdef SIMULATOR
        /* the D9/D15 draw rules, traced for check_trace.py's "paneldraw"
         * rule: during playback a panel draw (provider or leaf) never
         * allocates and never opens a library view */
        int opens = lib_open_calls();
        unsigned long allocs;

        core_alloc_watch(thread_self());
        allocs = core_alloc_watched();
#endif
        if (pv.scratch > 0)
            core_pin(pv.scratch);
        if (leaf)
        {
            struct ui_screen *s = (struct ui_screen *)k->arg;
            if (!ui_screen_live(s))
            {
                /* screen_gone() should have replaced the key: a call
                 * into a popped screen is a bug (freed data) */
#ifdef SIMULATOR
                panicf("ipodui: panel key %p is a popped screen", s);
#endif
                logf("ipodui: panel key is a popped screen");
                plain();
            }
            else if (s->ops->draw_into_panel)
                s->ops->draw_into_panel(s, &p);
            else
                plain();
        }
        else
        {
            const struct ui_preview_ops *ops = k->ops;
            ret = ops->draw(&p, k->arg, ui_panel_phase());
        }
        if (pv.scratch > 0)
            core_unpin(pv.scratch);
#ifdef SIMULATOR
        core_alloc_watch(0);
        if (audio_status())
            UPDATE_TRACE("panel draw %s playing allocs=%lu opens=%d",
                         leaf ? ((struct ui_screen *)k->arg)->ops->name
                              : ((const struct ui_preview_ops *)
                                 k->ops)->name,
                         core_alloc_watched() - allocs,
                         lib_open_calls() - opens);
#endif
    }
    lcd_set_viewport(NULL);
    return ret;
}

static void set_anim(int ret)
{
    pv.anim = ret != TIMEOUT_BLOCK && ret >= 0;
    if (pv.anim)
        pv.anim_due = current_tick + MAX(ret, 1);
}

/* -- the split shadow (8b-3; brief 3.1, spec main-menu.md "Split-screen
 *    depth") ------------------------------------------------------------
 *
 * The list casts a soft shadow onto the panel: columns 0..UI_SHADOW_W-1
 * of every panel image are darkened by ui_shadow_a[] exactly once. Every
 * writer of the panel rectangle goes through one of these, so the seam
 * never doubles, fades out or snaps back:
 *  - ui_panel_compose(): a key drawn into the LCD or the strip, then
 *    shadowed (replaces every bare draw_key());
 *  - ui_panel_fade(): the in-place fade toward a composed (shadowed)
 *    image, or toward a raw one (the drift's cover slot) whose shadow
 *    columns it darkens on the way (replaces every xfade_rect() call);
 *  - the drift's whole-pixel step (pan_to()) and half step
 *    (half_window()), which shadow what they wrote.
 * No ICODE (IRAM is for codecs). Cost: 1,440 blends per image, ~20k
 * instructions. */
static fb_data *lcd_panel(void)
{
    lcd_set_viewport(NULL);         /* FBADDR uses the current viewport */
    /* not ownable: preview.c is the panel's owner; every entry that draws settles first (drift_settle()) */
    return FBADDR(UI_PANEL_X, 0);
}

static void shadow_lcd(void)
{
    /* not ownable: preview.c is the panel's owner; every entry that draws settles first (drift_settle()) */
    ui_shadow_apply(lcd_panel(), LCD_WIDTH, UI_PANEL_H);
}

/* Draws key k into the LCD panel ('off' NULL) or the 160 x 240 strip
 * image 'off', shadowed. A gone key drawn into the LCD draws nothing
 * (what is there stays, already shadowed). */
static int ui_panel_compose(const struct nav_panel_key *k, bool leaf,
                            fb_data *off)
{
    int ret = draw_key(k, leaf, off);

    if (off)
        ui_shadow_apply(off, UI_PANEL_W, UI_PANEL_H);
    else if (leaf || k->ops != &gone_ops)
        shadow_lcd();
    return ret;
}

/* One in-place fade step of the LCD panel toward src (weight /256,
 * nav_xfade_weight()). 'raw': src is unshadowed (the drift slot), so its
 * shadow columns are darkened first and the seam stays as it was. */
static void ui_panel_fade(const fb_data *src, int sstride, int weight,
                          bool raw)
{
    /* not ownable: preview.c is the panel's owner; every entry that draws settles first (drift_settle()) */
    fb_data *dst = lcd_panel();
    fb_data t[UI_SHADOW_W];
    int y;

    if (!raw)
    {
        xfade_rect(dst, LCD_WIDTH, src, sstride, UI_PANEL_W, UI_PANEL_H,
                   weight);
        return;
    }
    xfade_rect(dst + UI_SHADOW_W, LCD_WIDTH, src + UI_SHADOW_W, sstride,
               UI_PANEL_W - UI_SHADOW_W, UI_PANEL_H, weight);
    for (y = 0; y < UI_PANEL_H; y++, dst += LCD_WIDTH, src += sstride)
    {
        memcpy(t, src, sizeof(t));
        ui_shadow_apply(t, UI_SHADOW_W, 1);
        xfade_rect(dst, LCD_WIDTH, t, UI_SHADOW_W, UI_SHADOW_W, 1, weight);
    }
}

void *ui_panel_scratch(size_t *size)
{
    if (pv.scratch <= 0)
    {
        *size = 0;
        return NULL;
    }
    *size = UI_PANEL_SCRATCH_SIZE;
    return core_get_data(pv.scratch);
}

/* -- the drift (ui_preview_drift, ui_preview_drift_books) ---------------- */

/* Covers are identified by key (owner rule, 2026-10-09: never the same
 * cover twice in a row, at the wrap or after panels.pak changed order;
 * drift_pick.h). The slot holds one cover of one set (music, or the
 * books of the Audiobooks rows, #58); a cover of the other set is never
 * shown. */

/* The cover now in the slot is on screen (a fade toward it began, or it
 * was drawn): check_trace.py's "driftrepeat" rule reads these lines (two
 * consecutive ones never name the same key). */
static void cover_shown(const char *how)
{
    UPDATE_TRACE("panel cover key=%08lx%08lx set=%d how=%s",
                 (unsigned long)(pv.cover_key >> 32),
                 (unsigned long)(pv.cover_key & 0xffffffffu), pv.set, how);
    (void)how;
    pv.holding = false;
}

/* Reads panel 'i' of 'set' into the drift slot (one 180,000 B read; may
 * yield). On a failure the slot may hold a torn picture: pv.cover is -1
 * then (pv.cover_key stays: it is still what the LCD shows). */
static bool slot_read(int set, int i)
{
    struct bitmap bm;
    uint64_t key = 0;
    int r;

    ui_anim_idle();                 /* never boosted across I/O */
    pv.seq = artcache_panel_seq();  /* before: a change meanwhile shows */
    core_pin(pv.slot);
#ifdef HAVE_BCMFW_VIDEO
    {
        int cw = 0;

        r = artcache_panel_get_cw(set, i, &bm, core_get_data(pv.slot),
                                  SLOT_SIZE, &key, &cw);
        pv.cw = cw;
    }
#else
    r = artcache_panel_get(set, i, &bm, core_get_data(pv.slot), SLOT_SIZE,
                           &key);
#endif
    core_unpin(pv.slot);
    if (r != ARTCACHE_OK || bm.width != NAV_DRIFT_SRC
        || bm.height != NAV_DRIFT_SRC)
    {
        logf("ipodui: drift: panel %d not read", i);
        SET_TRACE(set, "read-failed");
        pv.cover = -1;
        return false;
    }
    pv.set = set;
    pv.cover = i;
    pv.cover_key = key;
    return true;
}

/* The set's keys, in pv.count too. Only when storage may be read (the
 * 1 KB header at most). */
static int set_keys(int set, uint64_t *keys)
{
    pv.count = artcache_panel_keys(set, keys, LIBFILE_PANELS_MAX);
    return pv.count;
}

static int key_index(const uint64_t *keys, int n, uint64_t key)
{
    int i;

    for (i = 0; key && i < n; i++)
        if (keys[i] == key)
            return i;
    return -1;
}

#ifdef SIMULATOR
static bool phase_frozen(void);     /* below, with the panel phase */
#else
#define phase_frozen() false
#endif

/* Which cover the drift starts on (#52: a random one at boot). A
 * scripted simulator run takes the first, for deterministic shots. */
static int first_index(int count)
{
    unsigned long seed = current_tick;

    if (phase_frozen())
        return 0;
#if CONFIG_RTC
    seed ^= (unsigned long)mktime(get_time());
#endif
    return (seed * 2654435761u >> 8) % count;
}

static int shown_set(void)
{
#ifdef HAVE_BCMFW_VIDEO
    if (pv.sched.shown.ops == &ui_preview_videos)
        return ARTCACHE_PSET_VIDEOS;
#endif
    return pv.sched.shown.ops == &ui_preview_drift_books
           ? ARTCACHE_PSET_BOOKS : ARTCACHE_PSET_MUSIC;
}

/* A set's first cover went into the slot (LF_NEW): it holds still, the
 * phase frozen, for its fade and HOLD_STILL (DRIFT_HOLD at boot, #52)
 * while drift_hold_tick() prefetches the next cover; a live phase moves
 * on to the next cycle's start first, so the pan then starts from its
 * beginning (a frozen phase, scripted shots, stays put). */
static void hold_begin_first(int set, bool boot)
{
    long now = current_tick;

    (void)set;                  /* trace only: compiled out on the target */

    if (!phase_frozen())
    {
        long t = pv.phase.ticks % NAV_DRIFT_CYCLE;
        if (t)
            nav_phase_set(&pv.phase, pv.phase.ticks - t + NAV_DRIFT_CYCLE,
                          now);
    }
    pv.hold_start = now;
    pv.hold_until = now + (boot ? DRIFT_HOLD : NAV_DRIFT_FADE + HOLD_STILL);
    if (!pv.hold_until)
        pv.hold_until = 1;
    pv.hold_prep = true;
    UPDATE_TRACE("panel hold set=%d why=%s ms=%ld", set,
                 boot ? "begin-boot" : "begin-set", 0L);
}

/* Exchanges the two slots (handles and what they hold) */
static void slot_swap(void)
{
#define PV_SWAP(a, b) do { typeof(a) t_ = (a); (a) = (b); (b) = t_; } while (0)
    PV_SWAP(pv.slot, pv.slot2);
    PV_SWAP(pv.set, pv.set2);
    PV_SWAP(pv.cover, pv.cover2);
    PV_SWAP(pv.count, pv.count2);
    PV_SWAP(pv.cover_key, pv.cover_key2);
    PV_SWAP(pv.seq, pv.seq2);
#ifdef HAVE_BCMFW_VIDEO
    PV_SWAP(pv.cw, pv.cw2);
#endif
#undef PV_SWAP
}

/* Whether the second slot still holds a cover of 'set' that is in the
 * panel set (8b-4a M2): unchanged since it was parked, or -- once
 * panels.pak changed (a build, USB) -- found again by key while storage
 * may be read (the 1 KB header). Else it is dropped: no stale cover is
 * ever swapped in without I/O. */
static bool slot2_valid(int set)
{
    uint64_t keys[LIBFILE_PANELS_MAX];
    int n, i;

    if (pv.slot2 <= 0 || pv.set2 != set || pv.cover2 < 0)
        return false;
    if (pv.seq2 == artcache_panel_seq())
        return true;
    if (storage_ok())
    {
        ui_anim_idle();             /* never boosted across I/O */
        n = artcache_panel_keys(set, keys, LIBFILE_PANELS_MAX);
        i = key_index(keys, n, pv.cover_key2);
        if (i >= 0)
        {
            pv.cover2 = i;
            pv.count2 = n;
            pv.seq2 = artcache_panel_seq();
            return true;
        }
    }
    UPDATE_TRACE("panel slot2 dropped set=%d", set);
    pv.set2 = -1;
    pv.cover2 = -1;
    return false;
}

/* Exchanges the slots (each keeps the panels.pak sequence its cover was
 * read under) */
static void slot_park_swap(void)
{
    slot_swap();
}

/* With a second slot: the slot for 'set', swapped in if the second one
 * holds that set's last cover (no I/O). True if the slot now holds a
 * cover of 'set'. Nothing moves otherwise. */
static bool slot_for_set(int set)
{
    if (pv.set != set && slot2_valid(set))
    {
        slot_park_swap();
        UPDATE_TRACE("panel slot swap set=%d", set);
    }
    return pv.set == set && pv.cover >= 0;
}

/* slot_read() of another set's cover: with a second slot, the cover the
 * slot holds is parked there first (for the way back), and the read
 * overwrites what the second slot held. */
static bool slot_read_set(int set, int i)
{
    if (pv.slot2 > 0 && pv.set != set && pv.set >= 0 && pv.cover >= 0)
    {
        slot_park_swap();
        UPDATE_TRACE("panel slot park set=%d", (int)pv.set2);
    }
    return slot_read(set, i);
}

/* -- #92: the bank ------------------------------------------------------ */
static bool bank_valid(int set)
{
    return set >= 0 && set < PV_BANK_SETS && pv.bank[set] > 0
           && pv.bank_n[set] > 0
           && pv.bank_seq[set] == artcache_panel_seq();
}

/* Exchanges a and b, n bytes (word aligned, n % 4 == 0), in place */
static void swap_words(uint32_t *a, uint32_t *b, size_t n)
{
    for (n /= 4; n; n--, a++, b++)
    {
        uint32_t t = *a;
        *a = *b;
        *b = t;
    }
}

/* Banked cover j of 'set' into the slot, the slot's cover into its place
 * (an in-place swap, no I/O). The slot must hold a cover of 'set' read
 * under the bank's sequence (pv.seq). */
static void bank_swap(int set, int j)
{
    core_pin(pv.slot);
    core_pin(pv.bank[set]);
    swap_words(core_get_data(pv.slot),
               (uint32_t *)((char *)core_get_data(pv.bank[set])
                            + (size_t)j * ARTCACHE_PANEL_SLOT_BYTES),
               SLOT_SIZE);
    core_unpin(pv.bank[set]);
    core_unpin(pv.slot);
    {
        uint64_t k = pv.cover_key;
        short i = pv.cover;

        pv.cover_key = pv.bank_key[set][j];
        pv.cover = pv.bank_idx[set][j];
        pv.bank_key[set][j] = k;
        pv.bank_idx[set][j] = i;
    }
#ifdef HAVE_BCMFW_VIDEO
    {
        short c = pv.cw;
        pv.cw = pv.bank_cw[set][j];
        pv.bank_cw[set][j] = c;
    }
#endif
    pv.bank_seen[set] |= 1u << j;   /* now the slot's old cover: shown */
}

static bool bank_swappable(int set)
{
    return bank_valid(set) && pv.set == set && pv.cover >= 0
           && pv.seq == pv.bank_seq[set];
}

/* The next banked cover of 'set' to swap in, from bank_rot: one not
 * shown since the read first, else any (the drift keeps cycling through
 * the covers it holds while storage sleeps); never the slot's own cover.
 * Re-checked against the slot's key every time (review H1): a cover the
 * slot got another way (a prefetch, a direct read, Now Playing) counts
 * as shown, and the rotation steps past it. -1 if none. *fresh: an
 * unseen one was found (else the bank wants a read at the next wake). */
static int bank_next(int set, bool *fresh)
{
    int k, n = pv.bank_n[set], any = -1;

    for (k = 0; k < n; k++)
    {
        int j = (pv.bank_rot[set] + k) % n;

        if (pv.bank_key[set][j] == pv.cover_key)
        {
            pv.bank_seen[set] |= 1u << j;   /* on screen already */
            continue;
        }
        if (!(pv.bank_seen[set] & (1u << j)))
        {
            *fresh = true;
            return j;
        }
        if (any < 0)
            any = j;
    }
    *fresh = false;
    return any;
}

/* While storage sleeps, at a cycle end: the next banked cover of 'set'
 * into the slot (no I/O, bank_next()). True if the slot now holds
 * another cover of the set. */
static bool bank_rotate(int set)
{
    int j;
    bool fresh;

    if (!bank_swappable(set))
        return false;
    j = bank_next(set, &fresh);
    if (j < 0)
        return false;               /* only the slot's cover in it */
    bank_swap(set, j);
    pv.bank_rot[set] = (j + 1) % pv.bank_n[set];
    SET_TRACE(set, "bank");
    return true;
}

/* Now Playing (review, low): the album's cover from the bank, if it is
 * there (no disk spin-up). True if the slot now holds it. */
static bool bank_take(uint64_t key)
{
    int set, j;

    for (set = 0; set < PV_BANK_SETS; set++)
    {
        if (!bank_swappable(set))
            continue;
        for (j = 0; j < pv.bank_n[set]; j++)
            if (pv.bank_key[set][j] == key)
            {
                bank_swap(set, j);
                SET_TRACE(set, "bank-nowplaying");
                return true;
            }
    }
    return false;
}

/* At a natural disk wake while audio is paused or playing (never while
 * stopped, review: the bank serves only while storage sleeps, and on a
 * hard drive the read would spin it up): the set's bank read again when
 * it is stale or every banked cover was shown: the PV_BANK covers after
 * the slot's, in set order, one read on a grouped panels.pak. */
static void bank_refresh(int set)
{
    uint64_t keys[PV_BANK_CAP];
    int cw[PV_BANK_CAP], idx[PV_BANK_CAP];
    int got, j, count;

    if (set < 0 || set >= PV_BANK_SETS || pv.bank[set] <= 0
        || pv.set != set || pv.cover < 0)
        return;
    if (bank_valid(set))
    {
        bool fresh;

        if (bank_next(set, &fresh) >= 0 && fresh)
            return;                 /* still unseen covers */
    }
    if (pv.bank_seq[set] == artcache_panel_seq() && pv.bank_n[set] == 0
        && pv.bank_used[set] < 0)
        return;                     /* tried under this sequence: none */
    ui_anim_idle();                 /* never boosted across I/O */
    count = artcache_panel_count(set);
    pv.bank_seq[set] = artcache_panel_seq();
    pv.bank_n[set] = 0;
    pv.bank_rot[set] = 0;
    pv.bank_seen[set] = 0;
    pv.bank_used[set] = -1;         /* (a failed read: not again) */
    if (count < 2)
        return;
    core_pin(pv.bank[set]);
    got = artcache_panel_run(set, (pv.cover + 1) % count, PV_BANK,
                             pv.cover_key, core_get_data(pv.bank[set]),
                             (size_t)PV_BANK * ARTCACHE_PANEL_SLOT_BYTES,
                             keys, cw, idx);
    core_unpin(pv.bank[set]);
    if (got <= 0 || pv.bank_seq[set] != artcache_panel_seq())
        return;
    for (j = 0; j < got; j++)
    {
        pv.bank_key[set][j] = keys[j];
        pv.bank_idx[set][j] = idx[j];
#ifdef HAVE_BCMFW_VIDEO
        pv.bank_cw[set][j] = cw[j];
#endif
    }
    (void)cw;
    pv.bank_n[set] = got;
    pv.bank_used[set] = 0;
    UPDATE_TRACE("panel bank set=%d n=%d", set, got);
}

/* What load_first() did */
enum { LF_NONE = 0, LF_NEW, LF_SAME };

/* The set's first cover into the slot, once storage may be read: one raw
 * RGB565 read of the pre-scaled panel (no decode); the very first one is
 * shown still for DRIFT_HOLD. Never the cover the LCD shows (pv.cover_key, e.g. after a
 * torn read): when that one is the whole set it is read back (LF_SAME,
 * no fade). LF_NEW: another cover of 'set' is in the slot. */
static int load_first(int set)
{
    uint64_t keys[LIBFILE_PANELS_MAX];
    int i, n;
    bool boot;

    if (pv.slot <= 0 || (pv.set == set && pv.cover >= 0))
        return LF_NONE;
    if (slot_for_set(set))
    {
        /* the set's last cover, kept in the second slot: no I/O */
        SET_TRACE(set, "swap");
        hold_begin_first(set, false);
        return LF_NEW;
    }
    if (!storage_ok())
    {
        /* #73 (owner): the panel never goes empty when the set changes,
         * the same on flash and hard disk: the read happens now, waking
         * storage if it must (a spin-up on a hard disk: tuning only).
         * Once per set change: a failed read waits for an awake disk. */
        if (pv.woke_set == set)
        {
            SET_TRACE(set, "defer-asleep");
            return LF_NONE;
        }
        pv.woke_set = set;
        SET_TRACE(set, "wake-setchange");
#ifdef SIMULATOR
        /* the read wakes storage, as on the device: awake for the ATA
         * idle hold after it (ata.c: 1 s on flash, 5 s on a hard disk) */
#ifdef HAVE_FLASH_STORAGE
        sim_disk_active_until = current_tick + HZ;
#else
        sim_disk_active_until = current_tick + 5 * HZ;
#endif
#endif
    }
    /* every path that reads: a set read while awake must not leave an
     * older set's wake mark (8b-4a fix: Books (wake) -> Music (awake) ->
     * Books then deferred for ever while paused) */
    pv.woke_set = set;
    n = set_keys(set, keys);
    if (n <= 0)
    {
        SET_TRACE(set, "skip-nokeys");
        return LF_NONE;
    }
    i = first_index(n);
    if (keys[i] == pv.cover_key)
        i = drift_pick_next(keys, n, i, pv.cover_key);
    if (i < 0)
    {
        /* the set is just the cover on screen (the slot was torn) */
        SET_TRACE(set, "read-same");
        if (!slot_read_set(set, key_index(keys, n, pv.cover_key)))
            return LF_NONE;
        pv.holding = true;
        return LF_SAME;
    }
    pv.next = -1;
    boot = !pv.cover_key;
    SET_TRACE(set, boot ? "read-boot" : pv.set != set ? "read-setchange"
                                                     : "read-torn");
    if (!slot_read_set(set, i))
        return LF_NONE;
    hold_begin_first(set, boot);
    return LF_NEW;
}

static void hold_end(const char *why, long now)
{
    (void)why;                  /* trace only: compiled out on the target */
    (void)now;
    UPDATE_TRACE("panel hold set=%d why=%s ms=%ld", (int)pv.set, why,
                 (long)(now - pv.hold_start) * (1000 / HZ));
    pv.hold_until = 0;
    pv.hold_prep = false;
}

/* A set's first cover is held: no pan, the phase waits (HOLD_CAP at most,
 * whatever else happens) */
static bool drift_held(long now)
{
    if (pv.hold_until && !TIME_BEFORE(now, pv.hold_start + HOLD_CAP))
        hold_end("end-cap", now);
    return pv.hold_until != 0;
}

static void blit_window(int x, int y)
{
    const fb_data *src;

    core_pin(pv.slot);
    src = core_get_data(pv.slot);
    lcd_bitmap_part(src, x, y, STRIDE_MAIN(NAV_DRIFT_SRC, NAV_DRIFT_SRC),
                    0, 0, UI_PANEL_W, UI_PANEL_H);
    core_unpin(pv.slot);
}

/* 8b half steps (owner: panning not perfectly smooth; 1 px every 150 ms
 * judders): between two whole-pixel framings the panel shows the window
 * half-way, the average of the two, so the picture moves every 75 ms at
 * the same speed. Written straight into the LCD's panel rectangle (dst
 * stride LCD_WIDTH, word aligned) from the slot at (x0, y0) and (x1, y1),
 * one pixel apart on each axis at most. Two pixels per word: the floor
 * average of each RGB565 channel, (a & b) + ((a ^ b) & ~lsbs) / 2; a
 * window starting on an odd pixel is read as words one pixel back and
 * re-paired with a shift. No multiply; unrolled, 3.6 (aligned) to 4.5
 * (odd start) ARM instructions per pixel: 139k-175k per step (fw-ui
 * disassembly), against ~10k-60k for the whole-pixel blit's memcpy. */
#define AVG2(a, b) (((a) & (b)) + ((((a) ^ (b)) & 0xf7def7deu) >> 1))
/* Drop frames rather than lag: a half step whose compose takes longer
 * than this (target timer), HALF_SLOW_MAX times in a row, ends the half
 * steps until the next boot (ipodui.log says so). Expected: ~6-8 ms
 * unboosted at 30 MHz, memory stalls included. */
#define HALF_SLOW_US    25000
#define HALF_SLOW_MAX   4

/* pixels 2i and 2i + 1 of a row whose aligned words are w; 'odd': the
 * row starts one pixel into w (w is then one pixel before it) */
static inline __attribute__((always_inline)) uint32_t
pair_at(const uint32_t *w, int i, bool odd)
{
#ifdef ROCKBOX_LITTLE_ENDIAN
    return odd ? (w[i] >> 16) | (w[i + 1] << 16) : w[i];
#else
    return odd ? (w[i] << 16) | (w[i + 1] >> 16) : w[i];
#endif
}

static inline __attribute__((always_inline)) void
avg_rows(uint32_t *d, const fb_data *a, const fb_data *b, int h,
         bool aodd, bool bodd)
{
    const int n = UI_PANEL_W / 2;
    int i;

    for (; h > 0; h--, d += LCD_WIDTH / 2, a += NAV_DRIFT_SRC,
                       b += NAV_DRIFT_SRC)
    {
        const uint32_t *wa = (const uint32_t *)(a - aodd);
        const uint32_t *wb = (const uint32_t *)(b - bodd);

        for (i = 0; i < n; i++)
        {
            uint32_t pa = pair_at(wa, i, aodd), pb = pair_at(wb, i, bodd);
            d[i] = AVG2(pa, pb);
        }
    }
}

static void __attribute__((noinline, optimize("O2", "unroll-loops")))
half_window(int x0, int y0, int x1, int y1)
{
    const fb_data *src, *a, *b;
    /* not ownable: preview.c is the panel's owner; every entry that draws settles first (drift_settle()) */
    uint32_t *d = (uint32_t *)FBADDR(UI_PANEL_X, 0);

    core_pin(pv.slot);
    src = core_get_data(pv.slot);
    a = src + y0 * NAV_DRIFT_SRC + x0;
    b = src + y1 * NAV_DRIFT_SRC + x1;
    /* an odd start reads one pixel before the window and one after it:
     * x is then 1..NAV_DRIFT_SRC - UI_PANEL_W - 1, both inside the row */
    switch (((x0 & 1) << 1) | (x1 & 1))
    {
        case 0: avg_rows(d, a, b, UI_PANEL_H, false, false); break;
        case 1: avg_rows(d, a, b, UI_PANEL_H, false, true);  break;
        case 2: avg_rows(d, a, b, UI_PANEL_H, true, false);  break;
        default: avg_rows(d, a, b, UI_PANEL_H, true, true);  break;
    }
    core_unpin(pv.slot);
    /* the split shadow on what it wrote (raw slot pixels) */
    ui_shadow_apply((fb_data *)d, LCD_WIDTH, UI_PANEL_H);
}

#ifdef HAVE_DRIFT_JOBS
/* The whole-pixel framing (x, y) of the slot into the LCD panel, shadowed
 * (no push) */
static void blit_framing(int x, int y)
{
    struct viewport vp;

    memset(&vp, 0, sizeof(vp));
    vp.x = UI_PANEL_X;
    vp.width = UI_PANEL_W;
    vp.height = UI_PANEL_H;
    vp.drawmode = DRMODE_SOLID;
    lcd_set_viewport(&vp);
    blit_window(x, y);
    shadow_lcd();
    lcd_set_viewport(NULL);
}
#endif

/* P19 screen truth (phase19-brief.md 3.6): while the drift runs as jobs
 * the BCM holds the panel's pixels.  Before anything reads or writes the
 * panel on the CPU, the CPU writes its own framing of the last frame back
 * from pv.slot (no disk; a fade cut short shows its end) and pushes it,
 * so framebuffer and glass agree again.  No-op otherwise. */
#ifdef HAVE_DRIFT_JOBS
static void np_bcm_settle(void);
#endif
static void drift_settle(void)
{
#ifdef HAVE_DRIFT_JOBS
    if (!dj_owned())
        return;
    dj_release();
    if (pv.np_bcm)
        np_bcm_settle();            /* the row's own picture (no read-back) */
    else if (pv.slot > 0)
    {
        if (pv.shalf)
            half_window(pv.fx, pv.fy, pv.sx1, pv.sy1);
        else
            blit_framing(pv.fx, pv.fy);
    }
    /* through the ring, after anything still queued (M1); a BCM that
     * can't take it gets today's push */
    UPDATE_TRACE("update settle x=%d y=%d w=%d h=%d", UI_PANEL_X, 0,
                 UI_PANEL_W, UI_PANEL_H);
    if (!dj_resync())
        push_panel("settle");
    pv.lcd_stale = false;
#endif
}

#ifdef HAVE_DRIFT_JOBS
/* A full-screen lcd_update() while the panel is owned (bcmjob_owned_hook):
 * the caller has already drawn its frame, so the framebuffer is the new
 * truth: the drift's jobs retire and the panel is handed back, with no
 * redraw over the caller's pixels (a takeover that settles on entry, as
 * every shell one does, never gets here) */
static void drift_release_hook(void)
{
    if (dj_owned())
    {
        ui_log("panel", "drift released by a full push");
        dj_release();
    }
}
#endif

void ui_fb_settle(int x, int y, int w, int h)
{
    (void)y; (void)h;
#ifdef HAVE_MQ_JOBS
    mqj_settle(x, y, w, h);         /* 19-5: a marquee's PAN in the rect */
#endif
    if (x + w > UI_PANEL_X)         /* the panel is full height */
        drift_settle();
}

/* The held pan (#58; owner: one cover is held, never faded into itself).
 * With no other cover to show (one cover in the set, or during playback
 * with storage asleep and no prefetch kept), each new cycle pans the same
 * cover back the way the last cycle came, from where it is, at the
 * drift's speed: no fade, no jump. 'pos' is the new cycle's. */
static int sgn(int v)
{
    return v > 0 ? 1 : (v < 0 ? -1 : 0);
}

static void hold_begin(const struct nav_drift_pos *pos)
{
    if (!pv.holding)
    {
        /* the direction the cycle on screen panned in */
        struct nav_drift_pos a, b;

        nav_drift_pos(pv.shown_cycle * NAV_DRIFT_CYCLE, &a);
        nav_drift_pos(pv.shown_cycle * NAV_DRIFT_CYCLE + NAV_DRIFT_CYCLE - 1,
                      &b);
        pv.hdx = sgn(b.x - a.x);
        pv.hdy = sgn(b.y - a.y);
        if (!pv.hdx && !pv.hdy)
            pv.hdx = 1;
    }
    pv.hdx = -pv.hdx;
    pv.hdy = -pv.hdy;
    pv.hx = pv.fx;
    pv.hy = pv.fy;
    pv.holding = true;
    pv.shown_cycle = pos->cycle;
    pv.shown_pan = -1;              /* the next pan_to() always blits */
    UPDATE_TRACE("panel cover hold key=%08lx%08lx set=%d count=%d",
                 (unsigned long)(pv.cover_key >> 32),
                 (unsigned long)(pv.cover_key & 0xffffffffu), pv.set,
                 pv.count);
}

#ifdef HAVE_BCMFW_VIDEO
/* Phase 14a (docs/phase14a-brief.md 3.5): a portrait picture (pv.cw px
 * wide, centred in the slot) is panned inside itself. The drift's 140 px
 * of horizontal room are scaled to the picture's cw - 160 (40 px for a
 * 2:3 poster: +-20 around the centre), the vertical stays the cover's
 * 60 (+-30): the pans lean vertical. Scaling keeps the steps whole pixels
 * at most 1 px apart (half_window()) and the window inside the picture
 * (x odd only between its edges). */
static int content_x(int x)
{
    if (!pv.cw)
        return x;
    return (NAV_DRIFT_SRC - pv.cw) / 2
           + x * (pv.cw - UI_PANEL_W) / (NAV_DRIFT_SRC - UI_PANEL_W);
}
#endif

/* The framing 'pan' px into the cycle of 'pos': the cycle's own, or the
 * held pan's */
static void framing_at(const struct nav_drift_pos *pos, int pan, int *x,
                       int *y)
{
#ifdef HAVE_BCMFW_VIDEO
    const int x0 = pv.cw ? (NAV_DRIFT_SRC - pv.cw) / 2 : 0;
    const int x1 = pv.cw ? x0 + pv.cw - UI_PANEL_W
                         : NAV_DRIFT_SRC - UI_PANEL_W;

    if (!pv.holding)
    {
        *x = content_x(MAX(0, MIN(NAV_DRIFT_SRC - UI_PANEL_W,
                                  pos->x + pos->dx * (pan - pos->pan))));
        *y = pos->y + pos->dy * (pan - pos->pan);
    }
    else
    {
        *x = pv.hx + pv.hdx * pan;
        *y = pv.hy + pv.hdy * pan;
    }
    *x = MAX(x0, MIN(x1, *x));
#else
    if (!pv.holding)
    {
        *x = pos->x + pos->dx * (pan - pos->pan);
        *y = pos->y + pos->dy * (pan - pos->pan);
    }
    else
    {
        *x = pv.hx + pv.hdx * pan;
        *y = pv.hy + pv.hdy * pan;
    }
    *x = MAX(0, MIN(NAV_DRIFT_SRC - UI_PANEL_W, *x));
#endif
    *y = MAX(0, MIN(NAV_DRIFT_SRC - UI_PANEL_H, *y));
}

static void framing(const struct nav_drift_pos *pos, int *x, int *y)
{
    framing_at(pos, pos->pan, x, y);
}

/* draw(): the set's current cover at the phase's framing (no fade). A
 * cover of the other set is never shown: plain until this set's can be
 * read (storage asleep during playback: the drift tick retries). */
static int drift_draw_set(const struct ui_panel *p, long phase, int set)
{
    struct nav_drift_pos pos;
    int x, y;

    lcd_set_viewport(p->vp);
#ifdef HAVE_BCMFW_VIDEO
    /* the last video removed: the slot's poster is gone from the set
     * (artcache_panel_count() reads the 1 KB header only when the builder
     * changed it), so the film strip and "No Videos" show again */
    if (set == ARTCACHE_PSET_VIDEOS && pv.set == set && pv.cover >= 0
        && storage_ok() && artcache_panel_count(set) <= 0)
    {
        pv.cover = -1;
        pv.set = -1;
    }
#endif
    if (load_first(set) == LF_NEW)
    {
        cover_shown("draw");
        phase = pv.phase.ticks;     /* held: maybe a cycle start now */
    }
    lcd_set_viewport(p->vp);        /* a read may have run other code */
    if (pv.cover < 0 || pv.set != set)
    {
#ifdef HAVE_BCMFW_VIDEO
        /* the Videos panel without a poster to show (none, or not
         * readable now): the film strip and the count */
        if (set == ARTCACHE_PSET_VIDEOS)
        {
            ui_panel_draw_videos(p);
            return TIMEOUT_BLOCK;
        }
#endif
        SET_TRACE(set, "empty");    /* the panel shows no cover */
        plain();
        return TIMEOUT_BLOCK;
    }
    nav_drift_pos(phase, &pos);
    if (pv.holding && pos.cycle != pv.shown_cycle)
        hold_begin(&pos);
    framing(&pos, &x, &y);
    blit_window(x, y);
    pv.shown_cycle = pos.cycle;
    pv.shown_pan = pos.pan * 2;     /* the whole pixel; a due half step
                                       follows at the next drift tick */
    pv.fx = x;
    pv.fy = y;
    pv.df_k = 0;
    return TIMEOUT_BLOCK;
}

static int drift_draw(const struct ui_panel *p, const void *arg, long phase)
{
    (void)arg;
    return drift_draw_set(p, phase, ARTCACHE_PSET_MUSIC);
}

static int drift_draw_books(const struct ui_panel *p, const void *arg,
                            long phase)
{
    (void)arg;
    return drift_draw_set(p, phase, ARTCACHE_PSET_BOOKS);
}

const struct ui_preview_ops ui_preview_drift = {
    "drift", drift_draw, NAV_REST_COVER, UI_PANEL_XFADE };
const struct ui_preview_ops ui_preview_drift_books = {
    "drift-books", drift_draw_books, NAV_REST_COVER, UI_PANEL_XFADE };

#ifdef HAVE_BCMFW_VIDEO
/* Phase 14a: the main menu's Videos row. The posters drift like covers
 * (ARTCACHE_PSET_VIDEOS, built at sync time); with none (or none
 * readable yet) the film strip and "N Videos" / "No Videos" (spec
 * main-menu.md; owner photo mov-main-panel-videos.jpg), from which the
 * first poster fades in once it can be read. */
static int drift_draw_videos(const struct ui_panel *p, const void *arg,
                             long phase)
{
    (void)arg;
    return drift_draw_set(p, phase, ARTCACHE_PSET_VIDEOS);
}

const struct ui_preview_ops ui_preview_videos = {
    "videos", drift_draw_videos, NAV_REST_COVER, UI_PANEL_XFADE };
#endif

/* -- the Now Playing row (ui_preview_nowplaying; owner 2026-10-20) ------ */

/* The playing track's cover, still: no pan, no tick work.
 *  1. its panel-set image (300 x 300 ARTCACHE_PANEL, music or books),
 *     read into the drift slot like a drift cover -- or kept there (or
 *     in the second slot) when it already holds it: centred, filling;
 *  2. else its panel outside the shown set (panels.pak's default set,
 *     panelpool.pak): the same picture, its window read straight into
 *     the offscreen panel (np-art-quality);
 *  3. else (an album played since the last precompute is in no panel
 *     set) its 55 px list thumbnail scaled up, bilinear -- soft: the
 *     only flat image the art cache keeps for every album;
 *  4. else the plain panel (never empty, never another album's cover).
 * Storage (8b-4a M1): read when it may be (stopped, or awake). Asleep,
 * only a row the user just highlighted wakes it, once per cover; a
 * track change in the background draws plain (or keeps the right cover)
 * and reads at the next natural wake (ui_panel_tick()). */
static bool np_may_read(uint64_t key, bool user)
{
    if (storage_ok())
        return true;
    if (!user || pv.np_woke == key)
    {
        pv.np_deferred = true;
        return false;
    }
    return true;                    /* np_io() marks it once it reads */
}

/* Right before a real read for the row: unboosted (never boosted across
 * I/O), and with storage asleep this read is the user's one wake for
 * 'key' -- marked only now, so a draw that bails before any I/O (a
 * direct draw, a key in no panel set) leaves it for the follow-up. */
static void np_io(uint64_t key)
{
    ui_anim_idle();
    if (storage_ok())
        return;
    pv.np_woke = key;
#ifdef SIMULATOR
#ifdef HAVE_FLASH_STORAGE
    sim_disk_active_until = current_tick + HZ;
#else
    sim_disk_active_until = current_tick + 5 * HZ;
#endif
#endif
}

#ifdef SIMULATOR
/* test knob IPODUI_NP_NO_PANEL: "1" (ipodui-p8b-np-art), no album is in
 * a panel set, so the thumbnail fallback shows deterministically;
 * "shown" (ipodui-np-artq-*), only the shown set misses (the slot, the
 * bank, the set read), so a panel built outside it shows; "pool"
 * (ipodui-np-artq-pool), the shown set misses here and the default set
 * in artcache_panel_window(), so only the pool can answer; "built"
 * (ipodui-np-sharp-*), only npcover.pak can */
static int np_knob(void)
{
    const char *v = getenv("IPODUI_NP_NO_PANEL");

    if (!v)
        return 0;
    return strcmp(v, "shown") && strcmp(v, "pool") && strcmp(v, "built")
           ? 1 : 2;
}
#define NP_KNOB()   np_knob()
#else
#define NP_KNOB()   0
#endif

static bool np_cover_read(uint64_t key)
{
    uint64_t keys[LIBFILE_PANELS_MAX];
    int set, i, n;

    if (pv.slot <= 0 || NP_KNOB())
        return false;
    for (set = ARTCACHE_PSET_MUSIC; set <= ARTCACHE_PSET_BOOKS; set++)
    {
        /* not set_keys(): pv.count is the slot's set's, set below */
        ui_anim_idle();             /* the header may be read */
        n = artcache_panel_keys(set, keys, LIBFILE_PANELS_MAX);
        i = key_index(keys, n, key);
        if (i < 0)
            continue;
        SET_TRACE(set, "read-nowplaying");
        np_io(key);
        if (!slot_read_set(set, i))
            return false;
        pv.count = n;               /* drift_tick() plans with it */
        if (pv.next_key == key)
            pv.next = -1;           /* the drift never fades into it */
        cover_shown("nowplaying");
        return true;
    }
    return false;
}

/* 2: the album's panel where the shown set does not name it (an album
 * of panels.pak's default set rotated out of view, or of the pool,
 * #100): the same 300 x 300 picture as 1, its centre window read
 * straight into the offscreen panel (artcache_panel_window(): the rows
 * in runs through the strip behind the panel; the drift's slots and
 * bank untouched). Like 3, only into the offscreen panel (np_deferred
 * otherwise). False if none is built: then (np-sharp, owner 2026-10-24)
 * its render is queued, and the row reads again once it lands
 * (np_sharp_wait, ui_panel_tick()). */
static bool np_panel_draw(const struct ui_panel *p, uint64_t key)
{
    fb_data *dst;
    int r;

    if (!p->offscreen)
    {
        pv.np_deferred = true;
        return false;
    }
    if (NP_KNOB() == 1)
        return false;
    dst = p->vp->buffer->fb_ptr;
    np_io(key);
    r = artcache_panel_window(key, (NAV_DRIFT_SRC - UI_PANEL_W) / 2,
                              (NAV_DRIFT_SRC - UI_PANEL_H) / 2, UI_PANEL_W,
                              UI_PANEL_H, dst, UI_PANEL_W,
                              dst + UI_PANEL_W * UI_PANEL_H,
                              UI_STRIP_SIZE - PANEL_SIZE);
    lcd_set_viewport(p->vp);        /* the read may have run other code */
    if (r == ARTCACHE_PENDING)
        artcache_npcover_queue(key, ARTCACHE_NPR_ROW);
    if (r != ARTCACHE_OK)
    {
        pv.np_sharp_wait = true;    /* read again when a render lands */
        pv.np_sharp_seq = artcache_npcover_seq();
    }
    return r == ARTCACHE_OK;
}

/* 3: the album's flat cover from the art cache's only other per-album
 * flat image, its 55 x 55 list thumbnail (Now Playing's own art is the
 * composed card), scaled up to fill the panel the way a cover fills it
 * (240 x 240, the centre 160 columns), bilinear. Read into the strip
 * behind the offscreen panel (a highlight's arrival holds the strip)
 * and drawn once per arrival, never per frame: 38,400 px of RGB565
 * lerps (~1-2 M instructions, unboosted). Drawn straight to the LCD (a
 * full repaint, maybe inside a slide's capture) the strip is not ours:
 * plain now, and the next panel tick brings it by the arrival
 * (np_deferred). False if there is no thumbnail. */
#define NP_THUMB_FILL   UI_PANEL_H          /* the scaled side */
static bool np_art_draw(const struct ui_panel *p, uint64_t key)
{
    struct bitmap bm;
    fb_data *dst, *src;
    int r, x, y, sw, sh;

    if (!p->offscreen)
    {
        pv.np_deferred = true;
        return false;
    }
    dst = p->vp->buffer->fb_ptr;
    src = dst + UI_PANEL_W * UI_PANEL_H;    /* behind the panel */
    np_io(key);
    r = artcache_get(key, ARTCACHE_THUMB, &bm, src,
                     UI_STRIP_SIZE - PANEL_SIZE);
    lcd_set_viewport(p->vp);        /* the read may have run other code */
    if (r != ARTCACHE_OK || bm.width < 2 || bm.height < 2)
        return false;
    src = (fb_data *)bm.data;
    sw = bm.width;
    sh = bm.height;
    for (y = 0; y < UI_PANEL_H; y++)
    {
        /* 16.16 source position of the pixel centre */
        long fy = ((2 * y + 1) * (long)sh << 15) / NP_THUMB_FILL - 32768;
        int y0 = fy < 0 ? 0 : fy >> 16, wy = fy < 0 ? 0 : (fy >> 11) & 31;
        const fb_data *r0 = src + MIN(y0, sh - 1) * sw;
        const fb_data *r1 = src + MIN(y0 + 1, sh - 1) * sw;
        fb_data *d = dst + y * UI_PANEL_W;

        for (x = 0; x < UI_PANEL_W; x++)
        {
            int xf = x + (NP_THUMB_FILL - UI_PANEL_W) / 2;
            long fx = ((2 * xf + 1) * (long)sw << 15) / NP_THUMB_FILL
                      - 32768;
            int x0 = fx < 0 ? 0 : fx >> 16;
            int wx = fx < 0 ? 0 : (fx >> 11) & 31;
            int x1 = MIN(x0 + 1, sw - 1);
            /* packed 0x07e0f81f lerps, weights in 1/32 */
            uint32_t a = r0[x0], b = r0[x1], c = r1[x0], e = r1[x1];
            uint32_t t, u;

            a = (a | a << 16) & 0x07e0f81f;
            b = (b | b << 16) & 0x07e0f81f;
            c = (c | c << 16) & 0x07e0f81f;
            e = (e | e << 16) & 0x07e0f81f;
            t = ((a * (32 - wx) + b * wx) >> 5) & 0x07e0f81f;
            u = ((c * (32 - wx) + e * wx) >> 5) & 0x07e0f81f;
            t = ((t * (32 - wy) + u * wy) >> 5) & 0x07e0f81f;
            d[x] = t | t >> 16;
        }
    }
    return true;
}

static int np_cover_draw(const struct ui_panel *p, const void *arg,
                         long phase)
{
#ifdef HAVE_DRIFT_JOBS
    /* np_bcm_settle(): the album the glass shows, whatever plays now */
    uint64_t key = pv.np_settling ? pv.np_bcm_key : np_pb_cover_key();
#else
    uint64_t key = np_pb_cover_key();
#endif
    bool user = pv.np_user, have, may = false;

    (void)arg;
    (void)phase;
    pv.np_user = false;
    pv.np_deferred = false;
    pv.np_sharp_wait = false;
    pv.np_track = np_pb_track();
#ifdef HAVE_DRIFT_JOBS
    /* P19 19-6 (album-render.md section 14, the end state): the album's
     * panel is resident on the BCM: the row arrives by the BCM's fade to
     * that slot and stays its BLIT.  Nothing is read here, neither the
     * file nor the slot: the strip's picture is unused (plain).  The
     * CPU's own picture is drawn only if the CPU must take the panel
     * back (np_bcm_settle()) */
    pv.np_arrive = 0;
    if (key && p->offscreen && !pv.np_settling && !pv.ps_kind
        && dj_resident(key) && dj_path() == DJ_PATH_JOBS)
    {
        /* (a slide's arrival is the CPU's: its picture is drawn below,
         * then the row swaps to the slot once it rests) */
        pv.np_arrive = key;
        lcd_set_viewport(p->vp);
        plain();
        UPDATE_TRACE("panel nowplaying resident key=%08lx%08lx how=%s",
                     (unsigned long)(key >> 32),
                     (unsigned long)(key & 0xffffffffu), "arrive");
        return TIMEOUT_BLOCK;
    }
#endif
    if (key && !NP_KNOB() && pv.cover_key2 == key && pv.set2 >= 0
        && pv.set != pv.set2 && slot2_valid(pv.set2))
        slot_park_swap();           /* the other set's last cover: no I/O */
    have = key && !NP_KNOB() && pv.cover >= 0 && pv.cover_key == key;
    if (key && !have && !NP_KNOB() && bank_take(key))
    {
        have = true;                /* #92: banked, no disk spin-up */
        if (pv.next_key == key)
            pv.next = -1;
        cover_shown("nowplaying");
    }
    if (key && !have && (may = np_may_read(key, user)))
        have = np_cover_read(key);
    lcd_set_viewport(p->vp);        /* a read may have run other code */
    if (have)
    {
        /* the centre of the cover: the middle of every drift's range */
        blit_window((NAV_DRIFT_SRC - UI_PANEL_W) / 2,
                    (NAV_DRIFT_SRC - UI_PANEL_H) / 2);
        UPDATE_TRACE("panel nowplaying cover key=%08lx%08lx",
                     (unsigned long)(key >> 32),
                     (unsigned long)(key & 0xffffffffu));
    }
    else if (may && np_panel_draw(p, key))
        UPDATE_TRACE("panel nowplaying window key=%08lx%08lx",
                     (unsigned long)(key >> 32),
                     (unsigned long)(key & 0xffffffffu));
    else if (may && np_art_draw(p, key))
        UPDATE_TRACE("panel nowplaying art key=%08lx%08lx",
                     (unsigned long)(key >> 32),
                     (unsigned long)(key & 0xffffffffu));
    else
    {
        lcd_set_viewport(p->vp);
        UPDATE_TRACE("panel nowplaying plain key=%08lx deferred=%d",
                     (unsigned long)key, (int)pv.np_deferred);
        plain();
        if (!p->offscreen && pv.np_deferred)
            pv.np_user = user;      /* the follow-up keeps the user's say */
    }
    return TIMEOUT_BLOCK;
}

#ifdef HAVE_DRIFT_JOBS
/* The row's resident panel as the BCM shows it: the cover's centre, as
 * np_cover_draw()'s blit_window() frames it */
#define NP_RES_X    ((NAV_DRIFT_SRC - UI_PANEL_W) / 2)
#define NP_RES_Y    ((NAV_DRIFT_SRC - UI_PANEL_H) / 2)

/* drift_settle() for the row: the BCM's frames are done (dj_release());
 * the framebuffer gets the row's CPU picture (drawn again, through the
 * strip when it is free: the thumbnail and window reads need an offscreen
 * panel), never a read-back of the glass */
static void np_bcm_settle(void)
{
    fb_data *strip;
    bool kept;
    /* the redraw is the glass's album again, not a new arrival: the row's
     * own bookkeeping (a track change still due, a deferral) stays */
    uint32_t track = pv.np_track;
    bool user = pv.np_user, deferred = pv.np_deferred;
    bool sharp_wait = pv.np_sharp_wait;
    int sharp_seq = pv.np_sharp_seq;

    pv.np_bcm = false;
    pv.np_settling = true;
    UPDATE_TRACE("panel nowplaying resident settle key=%08lx%08lx",
                 (unsigned long)(pv.np_bcm_key >> 32),
                 (unsigned long)(pv.np_bcm_key & 0xffffffffu));
    strip = pv.visible ? claim(UI_STRIP_OWNER_PANEL, &kept) : NULL;
    if (strip)
    {
        ui_panel_compose(&pv.sched.shown, pv.shown_leaf, strip);
        ui_panel_fade(strip, UI_PANEL_W, 256, false);   /* exact */
        ui_strip_put();
    }
    else
        ui_panel_compose(&pv.sched.shown, pv.shown_leaf, NULL);
    pv.np_settling = false;
    pv.np_track = track;
    pv.np_user = user;
    pv.np_deferred = deferred;
    pv.np_sharp_wait = sharp_wait;
    pv.np_sharp_seq = sharp_seq;
}

/* The row shows its resident panel on the BCM once it rests: after a
 * slide or a direct draw (the CPU's picture first), or when the decode
 * lands while it shows (the instant swap).  Queues the re-decode when
 * the BCM lost it (an epoch). */
static void np_bcm_tick(bool lit)
{
    uint64_t key;
    bool shown = pv.visible && pv.fb_valid && !pv.shown_leaf
                 && pv.sched.shown.ops == &ui_preview_nowplaying;

    art_bcm_np_shown(shown && lit);
    if (!shown || !lit || pv.hl_k || pv.sched.pending)
        return;
    if (pv.np_bcm && !dj_resident(pv.np_bcm_key))
    {
        /* the BCM lost the slot (a cut, a restart: a new epoch) under
         * the still frame: the CPU's picture back on the glass */
        ui_log("panel", "nowplaying resident lost epoch=%lu",
               (unsigned long)dj_epoch());
        drift_settle();
        pv.np_bcm = false;
    }
    key = np_pb_cover_key();
    if (!key)
        return;
    if (!dj_resident(key))
    {
        uint32_t ep = dj_epoch();

        /* asked once an epoch, once it can be done (DECODE offered) */
        if ((pv.np_res_asked != key || pv.np_res_ep != ep)
            && art_bcm_resident_wanted(key))
        {
            pv.np_res_asked = key;
            pv.np_res_ep = ep;
            artcache_npcover_queue(key, ARTCACHE_NPR_RESIDENT);
        }
        return;
    }
    if (pv.np_bcm || dj_path() != DJ_PATH_JOBS)
        return;
    if (dj_resident_show(key, NP_RES_X, NP_RES_Y, 256))
    {
        pv.np_bcm = true;
        pv.np_bcm_key = key;
        UPDATE_TRACE("panel nowplaying resident key=%08lx%08lx how=%s",
                     (unsigned long)(key >> 32),
                     (unsigned long)(key & 0xffffffffu), "swap");
    }
}
#endif

const struct ui_preview_ops ui_preview_nowplaying = {
    "nowplaying", np_cover_draw, NAV_REST_COVER, UI_PANEL_XFADE };

static bool drift_shown(void)
{
#ifdef HAVE_BCMFW_VIDEO
    if (!pv.shown_leaf && pv.sched.shown.ops == &ui_preview_videos)
        return true;
#endif
    return !pv.shown_leaf && (pv.sched.shown.ops == &ui_preview_drift
                              || pv.sched.shown.ops == &ui_preview_drift_books);
}

/* One drift fade step, if due. Returns ticks until the next one. */
static int drift_fade_step(long now)
{
    int k = 1 + (now - pv.df_start) / DF_STEP;
    fb_data *src;
#ifdef HAVE_BCM_JOBS_MODEL
    bool jobs_frame = false;
#endif

    if (k > NAV_DRIFT_FADE_STEPS)
        k = NAV_DRIFT_FADE_STEPS;
    if (k > pv.df_k)
    {
        PT_GAPVARS(gap);
#ifdef HAVE_DRIFT_JOBS
        enum dj_path path = dj_path();

        pv.df_k = k;
        if (path == DJ_PATH_JOBS
            && dj_fade(pv.slot, pv.cover_key, pv.fx, pv.fy,
                       nav_xfade_weight(k, NAV_DRIFT_FADE_STEPS)))
        {
            pv.shalf = false;
            PT_TRACE("panel cost kind=fade step=%d jobs=1", k);
#ifdef HAVE_BCM_JOBS_MODEL
            jobs_frame = dj_parity_mode();  /* B3: the CPU frame too */
            if (!jobs_frame)
                goto stepped;
        }
        else
        {
#else
            goto stepped;
        }
#endif
        drift_settle();             /* a frame the jobs did not take */
        if (dj_path() == DJ_PATH_STILL)
        {
            /* D-1: the new cover at its framing, no fade */
            blit_framing(pv.fx, pv.fy);
            push_panel("drift");
            goto stepped;
        }
#ifdef HAVE_BCM_JOBS_MODEL
        }
#endif
#endif
        pv.df_k = k;
        lcd_set_viewport(NULL);
        PT_VAR(t_xf);
        core_pin(pv.slot);
        src = (fb_data *)core_get_data(pv.slot)
              + pv.fy * NAV_DRIFT_SRC + pv.fx;
        ui_panel_fade(src, NAV_DRIFT_SRC,
                      nav_xfade_weight(k, NAV_DRIFT_FADE_STEPS), true);
        core_unpin(pv.slot);
        PT_LAP(t_xf);
        PT_VAR(t_push);
        push_panel("drift");
        PT_LAP(t_push);
        PT_GAP(gap);
        PT_TRACE("panel cost kind=fade step=%d blit_us=%lu push_us=%lu "
                 "gap_us=%lu px=%d", k, t_xf, t_push, gap,
                 UI_PANEL_W * UI_PANEL_H);
#ifdef HAVE_BCM_JOBS_MODEL
        if (jobs_frame)
            dj_parity("fade");
#endif
    }
#ifdef HAVE_DRIFT_JOBS
stepped:
#endif
    if (pv.df_k >= NAV_DRIFT_FADE_STEPS)
    {
        pv.df_k = 0;
        return nav_drift_wake(ui_panel_phase());
    }
    return pv.df_start + pv.df_k * DF_STEP - now;
}

/* A new cover in the slot: fade toward it at the cycle's framing. */
static int drift_begin_fade(const struct nav_drift_pos *pos, long now,
                            const char *how)
{
    cover_shown(how);
    pv.shown_cycle = pos->cycle;
    pv.shown_pan = pos->pan * 2;
#ifdef HAVE_BCMFW_VIDEO
    {
        int x, y;

        framing(pos, &x, &y);       /* inside a portrait picture */
        pv.fx = x;
        pv.fy = y;
    }
#else
    pv.fx = pos->x;
    pv.fy = pos->y;
#endif
    pv.df_k = 0;
    pv.df_start = now;
    return drift_fade_step(now);
}

/* Draws the slot at the framing due now: a whole-pixel pan step (a
 * blit), a half step (half_window(), the blend toward the next pixel),
 * the held pan, or the same cover read back after a torn read (no
 * visible change). pv.fx/fy stay the whole-pixel framing: a fade or a
 * held pan starts from there. */
static void pan_to(const struct nav_drift_pos *pos)
{
    struct viewport vp;
    int x, y, x1 = 0, y1 = 0;
    bool half = false;
#ifdef HAVE_BCM_JOBS_MODEL
    bool jobs_frame = false;
#endif

    PT_GAPVARS(gap);
#ifdef HAVE_IPOD_UI_DEVMODE
    if (pv.shown_pan >= 0 && pos->cycle == pv.shown_cycle
        && NAV_DRIFT_SUB(pos) > pv.shown_pan)
    {
        /* bench lit24ui: the half steps since the last frame (1: on
         * time) and how far into this one's slot the frame is posted */
        int r = (pos->t - NAV_DRIFT_FADE) % NAV_DRIFT_STEP;

        LITMISS_DRIFT(NAV_DRIFT_SUB(pos) - pv.shown_pan,
                      r >= NAV_DRIFT_HALF ? r - NAV_DRIFT_HALF : r);
    }
#endif
    framing(pos, &x, &y);
    if (pos->half && !pv.half_off)
    {
        framing_at(pos, pos->pan + 1, &x1, &y1);
        half = x1 != x || y1 != y;  /* else clamped: the pixel itself */
    }
    lcd_set_viewport(NULL);
#ifdef HAVE_DRIFT_JOBS
    switch (dj_path())
    {
        case DJ_PATH_JOBS:
            if (half ? dj_half(pv.slot, pv.cover_key, x, y, x1, y1)
                     : dj_pan(pv.slot, pv.cover_key, x, y))
            {
                pv.shalf = half;
                pv.sx1 = x1;
                pv.sy1 = y1;
                pv.shown_pan = NAV_DRIFT_SUB(pos);
                pv.fx = x;
                pv.fy = y;
                PT_TRACE("panel cost kind=pan step=%d jobs=1 half=%d",
                         pv.shown_pan, half ? 1 : 0);
#ifdef HAVE_BCM_JOBS_MODEL
                jobs_frame = dj_parity_mode();  /* B3: the CPU too */
                if (jobs_frame)
                    break;
#endif
                return;
            }
            drift_settle();         /* this frame on the CPU */
            if (dj_path() != DJ_PATH_STILL)
                break;
            /* fall through */
        case DJ_PATH_STILL:
            /* D-1: the cover stands still where it is (settled, or drawn
             * by its fade); the plan moves on */
            drift_settle();
            pv.shown_pan = NAV_DRIFT_SUB(pos);
            return;
        default:
            drift_settle();         /* the row moved to "off" */
            break;
    }
#endif
    PT_VAR(t_blit);
    if (half)
    {
#ifndef SIMULATOR
        unsigned long t0 = USEC_TIMER, dt;
#endif
        half_window(x, y, x1, y1);
#ifndef SIMULATOR
        dt = USEC_TIMER - t0;
        pv.half_slow = dt > HALF_SLOW_US ? pv.half_slow + 1 : 0;
        if (pv.half_slow >= HALF_SLOW_MAX)
        {
            pv.half_off = true;
            ui_log("panel", "half steps off us=%lu", dt);
        }
#endif
    }
    else
    {
        memset(&vp, 0, sizeof(vp));
        vp.x = UI_PANEL_X;
        vp.width = UI_PANEL_W;
        vp.height = UI_PANEL_H;
        vp.drawmode = DRMODE_SOLID;
        lcd_set_viewport(&vp);
        blit_window(x, y);
        shadow_lcd();
    }
    PT_LAP(t_blit);
    pv.shown_pan = NAV_DRIFT_SUB(pos);
    pv.fx = x;
    pv.fy = y;
    UPDATE_TRACE("panel drift step=%d boost=%d lit=%d", pv.shown_pan,
                 ui_anim_active() ? 1 : 0, is_backlight_on(true) ? 1 : 0);
    PT_VAR(t_push);
    push_panel("drift");
    PT_LAP(t_push);
    PT_GAP(gap);
    /* step in half steps; half=1: half_window() was the compose */
    PT_TRACE("panel cost kind=pan step=%d blit_us=%lu push_us=%lu "
             "gap_us=%lu px=%d half=%d", pv.shown_pan, t_blit, t_push, gap,
             UI_PANEL_W * UI_PANEL_H, half ? 1 : 0);
#ifdef HAVE_BCM_JOBS_MODEL
    if (jobs_frame)
        dj_parity(half ? "half" : "pan");
#endif
}

/* The next cover (index 'nxt' of 'set') into the strip: the drift's
 * prefetch, one 180,000 B read. Sets pv.next, or pv.prefetch_failed. */
static void prefetch(int set, int nxt, long cycle)
{
    fb_data *strip;
    bool kept;

    strip = claim(UI_STRIP_OWNER_DRIFT, &kept);
    SET_TRACE(set, "read-prefetch");
    pv.next_seq = artcache_panel_seq();     /* the read's sequence */
    if (strip)
    {
        struct bitmap bm;
        uint64_t key = 0;
#ifdef HAVE_BCMFW_VIDEO
        int cw = 0;
        /* the one pin held across I/O (preview.h) */
        if (artcache_panel_get_cw(set, nxt, &bm, strip,
                                  UI_STRIP_SIZE, &key, &cw)
            == ARTCACHE_OK && key != pv.cover_key)
        {
            pv.next = nxt;
            pv.next_set = set;
            pv.next_key = key;
            pv.next_cw = cw;
        }
#else
        /* the one pin held across I/O (preview.h) */
        if (artcache_panel_get(set, nxt, &bm, strip,
                               UI_STRIP_SIZE, &key)
            == ARTCACHE_OK && key != pv.cover_key)
        {
            pv.next = nxt;
            pv.next_set = set;
            pv.next_key = key;
        }
#endif
        else
        {
            pv.next = -1;
            pv.prefetch_failed = cycle;
        }
    }
    ui_strip_put();
}

/* p19-battery: the last drift step the CPU drew and pushed itself (not
 * the BCM's jobs, not the still cover): the lit clock's stream signal */
static long cpu_drift_tick;
static bool cpu_drift_seen;

bool ui_panel_drift_cpu(void)
{
    return cpu_drift_seen && TIME_BEFORE(current_tick, cpu_drift_tick + HZ);
}

static int drift_tick(long phase, long now)
{
    uint64_t keys[LIBFILE_PANELS_MAX];
    struct nav_drift_pos pos;
    struct nav_drift_in in;
    enum nav_drift_act act;
    const int set = shown_set();
    fb_data *strip;
    bool kept;
    int nxt = -1, n = 0;

    if (pv.slot <= 0)
        return TIMEOUT_BLOCK;
#ifdef HAVE_DRIFT_JOBS
    if (dj_path() == DJ_PATH_CPU)
#endif
    {
        cpu_drift_tick = now;
        cpu_drift_seen = true;
    }
    nav_drift_pos(phase, &pos);
    if (pv.cover < 0 || pv.set != set)
    {
        /* plain so far, the other set's cover in the slot, or a torn
         * read: the set's first cover fades in once it can be read */
        switch (load_first(set))
        {
            case LF_NEW:
                /* held (hold_begin_first()): the phase may have moved on
                 * to a cycle start */
                nav_drift_pos(pv.phase.ticks, &pos);
                return drift_begin_fade(&pos, now, "first");
            case LF_SAME:
                break;              /* the slot is whole again */
            default:
                return TIMEOUT_BLOCK;   /* retried at the next pass */
        }
    }
    if (pv.df_k)
        return drift_fade_step(now);

    in.cycle = pos.cycle;
    in.shown_cycle = pv.shown_cycle;
    in.pan = NAV_DRIFT_SUB(&pos);
    in.shown_pan = pv.shown_pan;
    in.t = pos.t;
    in.at_rest = true;              /* called only at rest (ui_main.c) */
    in.storage_ok = storage_ok();
    in.playing = audio_status() != 0;
    in.next_kept = pv.next >= 0 && pv.next_set == set
                   && pv.next_key != pv.cover_key
                   && ui_strip_owned_by(UI_STRIP_OWNER_DRIFT);
    /* #92: storage asleep at a cycle end: the bank's next cover (no I/O);
     * awake mid-dwell (a natural wake, paused or playing): the bank read again
     * when every banked cover was shown or panels.pak changed */
    if (!in.storage_ok && !in.next_kept && pos.cycle != pv.shown_cycle
        && bank_rotate(set))
        return drift_begin_fade(&pos, now, "bank");
    if (in.storage_ok && in.playing && pos.cycle == pv.shown_cycle)
        bank_refresh(set);          /* a natural wake; never stopped */
    /* the next cover by key (drift_pick.h), whenever it may be read: at a
     * cycle end, and for a prefetch */
    if (in.storage_ok
        && (pos.cycle != pv.shown_cycle
            || (in.playing && !in.next_kept
                && pos.t >= NAV_DRIFT_PREFETCH_FROM)))
    {
        n = set_keys(set, keys);
        nxt = drift_pick_next(keys, n, pv.cover, pv.cover_key);
    }
    /* no other cover in the set: the plan's single-cover case (held) */
    in.panel_count = in.storage_ok && nxt < 0 ? 1 : pv.count;
    act = nav_drift_plan(&in);

    switch (act)
    {
        case NAV_DRIFT_PREFETCH:
            if (pv.prefetch_failed != pos.cycle && nxt >= 0)
            {
                prefetch(set, nxt, pos.cycle);
                break;
            }
            /* tried this dwell already: just pan */
            if (NAV_DRIFT_SUB(&pos) == pv.shown_pan)
                break;
            /* fall through */
        case NAV_DRIFT_PAN:
            pan_to(&pos);
            break;

        case NAV_DRIFT_FADE_STRIP:
            strip = claim(UI_STRIP_OWNER_DRIFT, &kept);
            pv.next = -1;
            if (strip && kept)
            {
                /* the old cover stays on the LCD: the fade blends from
                 * there, so the slot can take the new one first */
                core_pin(pv.slot);
                memcpy(core_get_data(pv.slot), strip, SLOT_SIZE);
                core_unpin(pv.slot);
                pv.seq = pv.next_seq;   /* as of the prefetch's read */
                pv.set = pv.next_set;
                pv.cover = -1;      /* its index in this set: unknown */
                pv.cover_key = pv.next_key;
#ifdef HAVE_BCMFW_VIDEO
                pv.cw = pv.next_cw;
#endif
                if (n > 0)
                    pv.cover = key_index(keys, n, pv.cover_key);
                if (pv.cover < 0)
                    pv.cover = 0;   /* only a starting point for the pick */
                ui_strip_put();
                return drift_begin_fade(&pos, now, "strip");
            }
            ui_strip_put();
            hold_begin(&pos);       /* lost after all: hold this cycle */
            pan_to(&pos);
            break;

        case NAV_DRIFT_FADE_DIRECT:
            pv.next = -1;
            if (nxt >= 0)
                SET_TRACE(set, "read-direct");
            if (nxt >= 0 && slot_read(set, nxt))
                return drift_begin_fade(&pos, now, "direct");
            /* not read (the slot may be torn): the cover on screen back
             * into it, if it is still in the set, and held; else the
             * next pass starts over (load_first() never picks the cover
             * on screen; the LCD keeps it meanwhile) */
            if (nxt >= 0 && !slot_read(set, key_index(keys, n, pv.cover_key)))
                return TIMEOUT_BLOCK;
            /* fall through */
        case NAV_DRIFT_REDWELL:
            if (!in.storage_ok)
                SET_TRACE(set, "defer-hold");   /* same cover, held pan */
            hold_begin(&pos);
            pan_to(&pos);
            break;

        case NAV_DRIFT_IDLE:
            break;
    }
    return nav_drift_wake(phase);
}

/* A held first cover (hold_begin_first()): its fade runs, then the next
 * cover is prefetched (once, when storage may be read, as the drift's own
 * prefetch), and the hold ends at hold_until once that was tried: the
 * pan then starts from its beginning, the next read already done. */
static int drift_hold_tick(long now)
{
    const int set = shown_set();

    if (pv.df_k)
    {
        int w = drift_fade_step(now);
        if (pv.df_k)
            return w;
    }
    if (pv.hold_prep)
    {
        uint64_t keys[LIBFILE_PANELS_MAX];
        long cycle = pv.phase.ticks / NAV_DRIFT_CYCLE;
        int n, nxt;

        pv.hold_prep = false;
        if (pv.cover >= 0 && pv.set == set && storage_ok()
            && !(pv.next >= 0 && pv.next_set == set
                 && pv.next_key != pv.cover_key
                 && ui_strip_owned_by(UI_STRIP_OWNER_DRIFT)))
        {
            n = set_keys(set, keys);
            nxt = drift_pick_next(keys, n, pv.cover, pv.cover_key);
            if (nxt >= 0 && pv.prefetch_failed != cycle)
                prefetch(set, nxt, cycle);
        }
        now = current_tick;         /* the read took its time */
    }
    if (TIME_BEFORE(now, pv.hold_until))
        return pv.hold_until - now;
    hold_end(pv.next >= 0 && pv.next_set == set ? "end-ready"
             : storage_ok() ? "end-none" : "end-asleep", now);
    return 1;                       /* the drift resumes next pass */
}

/* -- highlight arrivals ------------------------------------------------- */

/* The panel-sync trace (P11, owner rule: the panel never shows a row
 * other than the selected one): "panel want" at every selection report,
 * "panel show" whenever a draw of the wanted key starts (fade, direct,
 * reveal). check_trace.py: every show equals the last want. */
static const char *key_name(const void *ops) __attribute__((unused));
static const char *key_name(const void *ops)
{
    return ops ? *(const char *const *)ops : "none";
}

static void trace_show(const char *how) __attribute__((unused));
static void trace_show(const char *how)
{
    (void)how;
    UPDATE_TRACE("panel show key=%s arg=%p how=%s",
                 key_name(pv.sched.shown.ops), pv.sched.shown.arg, how);
}

static void direct_draw(void)
{
    trace_show("direct");
    set_anim(ui_panel_compose(&pv.sched.shown, pv.shown_leaf, NULL));
    push_panel("panel");
    pv.fb_valid = true;
}

/* One fade frame (boosted, paced by the 30 fps cap). Returns
 * UI_TICK_ANIM while it runs. hl_k -1 is "started, no frame yet"
 * (hl_start()): ui_panel_tick() keeps calling here until the frame slot
 * comes round, so a first frame put off because a list scroll had just
 * taken the slot is resumed, never lost (8b-3: with hl_k 0 it read as "no
 * fade" and Settings rows below the ninth kept the previous row's
 * panel), and never forced out of turn either (a fast spin keeps its
 * glide frames). */
/* -- panel arrivals between a slideshow and a fixed panel (owner,
 *    2026-10-20; spec main-menu.md "Panel transitions") -----------------
 *
 * Inside a split screen (the list stays), when the highlighted row's
 * panel changes:
 *  - slideshow (a drift: music, books, posters) -> fixed panel: the
 *    slideshow slides out to the right, uncovering the fixed panel in
 *    place (composed underneath first, in the strip);
 *  - fixed -> slideshow: the slideshow slides in from the right over the
 *    fixed panel, which stays put;
 *  - slideshow -> another slideshow group: the cross-fade (the target's
 *    cover from the second slot when it holds it), then its drift goes on;
 *  - fixed -> fixed: the cross-fade; rows of the same slideshow: nothing.
 * The moving image carries its split shadow (baked into its first
 * columns, as the parting's panel does); the panel uncovered or left
 * behind keeps its own at the seam. Frames: the highlight fade's pacing
 * (hl_k), a fast spin retargets to the newest row as the fade does.
 * Memory: the strip's offscreen panel (76,800 B) only. Per frame: one
 * memmove + memcpy per panel row (<= 160 px), one 160 x 240 push.
 * UI_PANEL_SLIDE 0 restores the cross-fade everywhere (A/B). */
#ifndef UI_PANEL_SLIDE
#define UI_PANEL_SLIDE  1
#endif
#define UI_PANEL_SLIDE_TICKS (HZ * 20 / 100)    /* device tuning (8b-4) */

/* 0: a fixed panel; else the slideshow group (its panel set + 1) */
static int panel_group(const struct nav_panel_key *k, bool leaf)
{
    if (leaf || !k->ops)
        return 0;
    if (k->ops == &ui_preview_drift)
        return 1 + ARTCACHE_PSET_MUSIC;
    if (k->ops == &ui_preview_drift_books)
        return 1 + ARTCACHE_PSET_BOOKS;
#ifdef HAVE_BCMFW_VIDEO
    if (k->ops == &ui_preview_videos)
        return 1 + ARTCACHE_PSET_VIDEOS;
#endif
    return 0;
}

/* One slide frame from the strip (the arriving panel, composed). */
static int ps_step(fb_data *strip)
{
    long el, inv;
    int d, y;

    if (pv.hl_k < 0)
    {
        pv.hl_k = 0;                /* the first frame already moves */
        pv.ps_start = current_tick - 1;
        pv.ps_done = 0;
    }
    pv.hl_k = MIN(pv.hl_k + 1, 100);
    el = current_tick - pv.ps_start;
    /* quadratic ease-out; the last frame that fits lands it */
    if (el + UI_ANIM_FRAME_TICKS > UI_PANEL_SLIDE_TICKS)
        d = UI_PANEL_W;
    else
    {
        inv = 1024 - el * 1024 / UI_PANEL_SLIDE_TICKS;
        d = UI_PANEL_W - (int)(UI_PANEL_W * (inv * inv / 1024) / 1024);
    }
    if (d > pv.ps_done)
    {
        lcd_set_viewport(NULL);
        if (d >= UI_PANEL_W)
            ui_panel_fade(strip, UI_PANEL_W, 256, false);  /* exact */
        else
            for (y = 0; y < UI_PANEL_H; y++)
            {
                /* not ownable: preview.c is the panel's owner; every entry that draws settles first (drift_settle()) */
                fb_data *row = FBADDR(UI_PANEL_X, y);
                const fb_data *srow = strip + y * UI_PANEL_W;

                if (pv.ps_kind > 0)
                {
                    /* the slideshow moves right; the fixed panel shows
                     * where it was */
                    memmove(row + d, row + pv.ps_done,
                            (UI_PANEL_W - d) * sizeof(fb_data));
                    memcpy(row + pv.ps_done, srow + pv.ps_done,
                           (d - pv.ps_done) * sizeof(fb_data));
                }
                else            /* its first d columns at the right edge */
                    memcpy(row + UI_PANEL_W - d, srow, d * sizeof(fb_data));
            }
        pv.ps_done = d;
    }
    ui_strip_put();
    push_panel("anim:pslide");
    if (d >= UI_PANEL_W)
    {
        pv.hl_k = 0;
        pv.ps_kind = 0;
        return TIMEOUT_BLOCK;
    }
    return UI_TICK_ANIM;
}

static int hl_step(void)
{
    fb_data *strip;
    bool kept;
    int k;

    if (!ui_anim_frame_due())
        return UI_TICK_ANIM;
    ui_anim_frame();
#ifdef HAVE_DRIFT_JOBS
    if (pv.np_arrive && !pv.ps_kind)
    {
        /* P19 19-6: the Now Playing row's arrival on the BCM, a FADE step
         * from the front frame to the resident slot (the CPU's
         * ui_panel_fade(raw) toward the same picture, B3) */
        if (pv.hl_k < 0)
            pv.hl_k = 0;
        k = pv.hl_k + 1;
        if (dj_resident_show(pv.np_arrive, NP_RES_X, NP_RES_Y,
                             k >= NAV_PANEL_XFADE_STEPS ? 256
                             : nav_xfade_weight(k, NAV_PANEL_XFADE_STEPS)))
        {
            pv.hl_k = k;
            pv.np_bcm = true;
            pv.np_bcm_key = pv.np_arrive;
            UPDATE_TRACE("panel nowplaying resident key=%08lx%08lx how=%s "
                         "step=%d", (unsigned long)(pv.np_arrive >> 32),
                         (unsigned long)(pv.np_arrive & 0xffffffffu),
                         "fade", k);
            if (k < NAV_PANEL_XFADE_STEPS)
                return UI_TICK_ANIM;
            pv.hl_k = 0;
            pv.np_arrive = 0;
            return TIMEOUT_BLOCK;
        }
        /* refused (a decode began, the BCM went): the CPU's fade from
         * here, toward the CPU's picture (the BCM's steps settled first:
         * it ends at its target either way), drawn into the strip now */
        pv.np_arrive = 0;
        if (pv.np_bcm)
            drift_settle();         /* composes it into the strip too */
        else
        {
            strip = claim(UI_STRIP_OWNER_PANEL, &kept);
            if (strip)
            {
                pv.np_settling = true;
                pv.np_bcm_key = np_pb_cover_key();
                ui_panel_compose(&pv.sched.shown, pv.shown_leaf, strip);
                pv.np_settling = false;
                ui_strip_put();
            }
        }
    }
#endif
    strip = claim(UI_STRIP_OWNER_PANEL, &kept);
    if (!strip || !kept)
    {
        /* a glide or slide took the strip: never leave it half-faded */
        ui_strip_put();
        logf("ipodui: panel fade lost the strip, drawn directly");
        pv.hl_k = 0;
        pv.ps_kind = 0;
        direct_draw();
        return TIMEOUT_BLOCK;
    }
    if (pv.ps_kind)
        return ps_step(strip);
    if (pv.hl_k < 0)
        pv.hl_k = 0;                /* the first frame */
    k = ++pv.hl_k;
    ui_panel_fade(strip, UI_PANEL_W,
                  nav_xfade_weight(k, NAV_PANEL_XFADE_STEPS), false);
    ui_strip_put();
    push_panel("anim:xfade");
    if (k >= NAV_PANEL_XFADE_STEPS)
    {
        pv.hl_k = 0;
        return TIMEOUT_BLOCK;
    }
    return UI_TICK_ANIM;
}

/* The wanted panel is due: draw it into the strip and fade it in. */
static int hl_start(void)
{
    fb_data *strip;
    bool kept;
    int ret;

    UPDATE_TRACE("panel arrive owner=%s wait=%ld",
                 pv.want_leaf
                 ? ((const struct ui_screen *)pv.sched.want.arg)->ops->name
                 : ((const struct ui_preview_ops *)pv.sched.want.ops)->name,
                 current_tick - pv.sched.moved);
    {
        /* what the LCD shows (if valid) against what arrives */
        int from = pv.fb_valid ? panel_group(&pv.sched.shown, pv.shown_leaf)
                               : -1;
        int to = panel_group(&pv.sched.want, pv.want_leaf);

        pv.ps_kind = 0;
#if UI_PANEL_SLIDE
        if (from > 0 && to == 0)
            pv.ps_kind = 1;
        else if (from == 0 && to > 0)
            pv.ps_kind = -1;
#endif
        (void)from;
        (void)to;
    }
    nav_sched_arrived(&pv.sched);
    pv.shown_leaf = pv.want_leaf;
    trace_show(pv.ps_kind > 0 ? "slide-out" : pv.ps_kind < 0 ? "slide-in"
                                                              : "fade");
    strip = claim(UI_STRIP_OWNER_PANEL, &kept);
    if (!strip)
    {
        direct_draw();
        return TIMEOUT_BLOCK;
    }
    /* pinned across the provider's draw (it may load a glyph) */
    ret = ui_panel_compose(&pv.sched.shown, pv.shown_leaf, strip);
    ui_strip_put();
    set_anim(ret);
    pv.hl_k = -1;                   /* started, no frame yet */
    if (pv.anim)
    {
        /* an animated panel draws straight to the LCD from now on, but
         * it arrives by the same fade */
        pv.anim_due = current_tick + NAV_PANEL_XFADE_TICKS;
    }
    return hl_step();
}

/* -- the scheduler API -------------------------------------------------- */

/* The panel phase, brought up to date. In a scripted simulator run it
 * is frozen by default (deterministic goldens: the drift framing and
 * cycle never depend on how long the sim took to get there); a script
 * opts in with "# drift: live" (shots.sh exports IPODUI_SIM_DRIFT=live)
 * or "drift_phase <ticks>" (live from that phase, sim_cmd_nav.c). */
#ifdef SIMULATOR
extern const char *sim_script_file;
static signed char phase_live = -1;     /* -1: the default, see above */

static bool phase_frozen(void)
{
    if (phase_live < 0)
    {
        const char *e = getenv("IPODUI_SIM_DRIFT");
        phase_live = !sim_script_file || (e && !strcmp(e, "live"));
    }
    return !phase_live;
}
#endif

static long phase_update(long now, bool lit)
{
    if (phase_frozen() || drift_held(now))
    {
        nav_phase_set(&pv.phase, pv.phase.ticks, now);
        pv.phase.lit = lit;
        return pv.phase.ticks;
    }
    return nav_phase_update(&pv.phase, now, lit);
}

/* A screen leaves (screen_stack.c's hook, before its leave()): a leaf
 * key on it becomes gone_ops, so nothing calls into it again. */
static void screen_gone(struct ui_screen *s)
{
    struct nav_panel_sched *sc = &pv.sched;

    if (pv.shown_leaf && sc->shown.arg == s && pv.hl_k)
    {
        /* popped during its own arrival fade (#38 end review): never
         * leave a half-faded panel. The strip still holds the leaf's
         * whole panel unless a glide took it: then finish the fade at
         * once (the exact last step); else a plain panel. Either way
         * the parent's item arrives after its rest delay, as usual. */
        fb_data *strip;
        bool kept;

        strip = claim(UI_STRIP_OWNER_PANEL, &kept);
        if (strip && kept)
            ui_panel_fade(strip, UI_PANEL_W,
                          nav_xfade_weight(NAV_PANEL_XFADE_STEPS,
                                           NAV_PANEL_XFADE_STEPS), false);
        else
        {
            struct viewport vp;

            memset(&vp, 0, sizeof(vp));
            vp.x = UI_PANEL_X;
            vp.width = UI_PANEL_W;
            vp.height = UI_PANEL_H;
            vp.drawmode = DRMODE_SOLID;
            lcd_set_viewport(&vp);
            plain();
            shadow_lcd();
        }
        if (strip)
            ui_strip_put();
        UPDATE_TRACE("panel fade snapped: %s popped at step %d/%d",
                     s->ops->name, pv.hl_k, NAV_PANEL_XFADE_STEPS);
        pv.hl_k = 0;
        push_panel("panel");
    }
    if (pv.shown_leaf && sc->shown.arg == s)
    {
        sc->shown.ops = &gone_ops;
        sc->shown.arg = NULL;
        pv.shown_leaf = false;
        pv.anim = false;
    }
    if (pv.want_leaf && sc->want.arg == s)
    {
        /* nothing else wanted yet: the panel on screen stays (the
         * parent selects its own item when it draws) */
        sc->want = sc->shown;
        sc->pending = false;
        pv.want_leaf = pv.shown_leaf;
    }
}

bool ui_preview_reserve(void)
{
#ifdef HAVE_DRIFT_JOBS
    /* a full-screen lcd_update() releases the drift's panel first (H3) */
    bcmjob_owned_hook(drift_release_hook);
#endif
    ui_set_leave_hook(screen_gone);
    nav_sched_init(&pv.sched);
    nav_phase_init(&pv.phase, current_tick, true);
    if (pv.slot <= 0)
    {
        pv.slot = core_alloc(SLOT_SIZE);
        if (pv.slot <= 0)
        {
            pv.slot = 0;
            logf("ipodui: no memory for the drift slot");
        }
    }
    /* the second slot, at shell start only (before the audio buffer;
     * never later: an allocation while paused restarts the queue).
     * Without it the drift works as before with one slot. */
    if (pv.slot > 0 && pv.slot2 <= 0
#ifdef SIMULATOR
        /* test knob: the one-slot fallback (32 MB, memory short) */
        && !getenv("IPODUI_NO_SLOT2")
#endif
       )
    {
        pv.slot2 = core_alloc(SLOT_SIZE);
        if (pv.slot2 <= 0)
        {
            pv.slot2 = 0;
            logf("ipodui: no memory for the second drift slot");
        }
        UPDATE_TRACE("panel slot2 %s", pv.slot2 > 0 ? "yes" : "no");
    }
#if PV_BANK > 0
    /* #92: the banks, at shell start only, after the second slot (the
     * same rule); without them the drift is 8b's */
    if (pv.slot2 > 0
#ifdef SIMULATOR
        && !getenv("IPODUI_NO_BANK")
#endif
       )
    {
        int set;

        for (set = 0; set < PV_BANK_SETS; set++)
            if (pv.bank[set] <= 0)
            {
                pv.bank[set] = core_alloc((size_t)PV_BANK
                                          * ARTCACHE_PANEL_SLOT_BYTES);
                if (pv.bank[set] <= 0)
                {
                    pv.bank[set] = 0;
                    logf("ipodui: no memory for a drift bank");
                }
            }
        UPDATE_TRACE("panel bank covers=%d music=%d books=%d", PV_COVERS,
                     pv.bank[0] > 0, pv.bank[1] > 0);
    }
#endif
#if UI_PANEL_SCRATCH_SIZE > 0
    if (pv.scratch <= 0)
    {
        pv.scratch = core_alloc(UI_PANEL_SCRATCH_SIZE);
        if (pv.scratch <= 0)
        {
            pv.scratch = 0;
            logf("ipodui: no memory for the panel scratch");
        }
    }
    return pv.slot > 0 && pv.scratch > 0;
#else
    return pv.slot > 0;             /* no scratch (preview.h, 8b-3) */
#endif
}

/* Owner rule (P11): a selection change starts the cross-fade on the same
 * frame, from what is on screen to the new row's panel, so the rest delay
 * is 0 for every panel (ops->rest_delay_ticks and NAV_REST_* stay for
 * the pure scheduler tests). Drift panels draw the cover that is already
 * in RAM; the art builder's placeholder is that row's own panel. */
#define PANEL_REST 0

void ui_panel_select(const struct ui_preview_ops *ops, const void *item_arg)
{
    UPDATE_TRACE("panel want key=%s arg=%p", key_name(ops), item_arg);
    if (ops == &ui_preview_nowplaying)
    {
        pv.np_user = true;          /* highlighted by the user: one wake */
        pv.np_woke = 0;             /* per highlight, not per cover */
    }
    mod_panel_key(ops);             /* #106: a module's preview stays */
    nav_sched_select(&pv.sched, ops, item_arg, PANEL_REST, current_tick);
    pv.want_leaf = ops ? false : pv.shown_leaf;
    pv.visible = true;
}

void ui_panel_select_screen(struct ui_screen *s)
{
    UPDATE_TRACE("panel want key=%s arg=%p", key_name(s->ops), (void *)s);
    mod_panel_key(s->ops);          /* #106: a module screen's panel */
    nav_sched_select(&pv.sched, s->ops, s, PANEL_REST, current_tick);
    pv.want_leaf = true;
    pv.visible = true;
}

void ui_panel_invalidate(void)
{
    nav_sched_invalidate(&pv.sched, current_tick);
}

void ui_panel_hide(void)
{
    if (!pv.visible)
        return;
    drift_settle();
#ifdef HAVE_DRIFT_JOBS
    art_bcm_np_shown(false);
    pv.np_arrive = 0;
#endif
    pv.visible = false;
    pv.fb_valid = false;
    pv.lcd_stale = false;
    pv.hl_k = 0;
    pv.df_k = 0;
    pv.anim = false;
}

void ui_panel_draw_now(void)
{
    drift_settle();
    /* what the LCD shows is not what is drawn next: ui_panel_sync() */
    if (!pv.fb_valid || pv.sched.pending || pv.hl_k)
        pv.lcd_stale = true;
    pv.visible = true;
    pv.hl_k = 0;
    /* a reveal, boot, push or pop: the CURRENT row's panel in the first
     * frame, no fade; never the last-drawn one (owner rule: album art
     * behind a Settings row after a pop from Now Playing) */
    if (pv.sched.want.ops || !pv.fb_valid)
    {
        nav_sched_forget(&pv.sched);
        nav_sched_arrived(&pv.sched);
        pv.shown_leaf = pv.want_leaf;
    }
    trace_show("now");
    set_anim(ui_panel_compose(&pv.sched.shown, pv.shown_leaf, NULL));
    pv.fb_valid = true;
}

void ui_panel_sync(int y)
{
    drift_settle();
    if (pv.lcd_stale && y < UI_PANEL_H)
        ui_lcd_push("panel", UI_PANEL_X, y, UI_PANEL_W, UI_PANEL_H - y);
    pv.lcd_stale = false;
}

int ui_panel_tick(void)
{
    long now = current_tick;
    bool lit = is_backlight_on(true);
    long phase = phase_update(now, lit);
    long due;
    int wake = TIMEOUT_BLOCK;

    if (!pv.visible || !pv.fb_valid)
        return TIMEOUT_BLOCK;
    due = nav_sched_due(&pv.sched, now);
#ifdef HAVE_DRIFT_JOBS
    /* (an arrival on the BCM reads nothing of the framebuffer) */
    if ((pv.hl_k && !pv.np_arrive) || due == 0)
#else
    if (pv.hl_k || due == 0)
#endif
        drift_settle();             /* the highlight fade reads the panel */
    if (pv.hl_k && due != 0)
        return hl_step();
    if (pv.hl_k)
        pv.hl_k = 0;                /* retarget: the newest row, no queue */
    if (due == 0)
        return hl_start();
    if (due != NAV_NEVER)
        wake = due;

    /* the Now Playing row: the track changed under it, one redraw (its
     * cross-fade) -- one volatile read a pass */
    if (pv.sched.want.ops == &ui_preview_nowplaying && !pv.sched.pending
        && (np_pb_track() != pv.np_track
            || (pv.np_deferred && (storage_ok() || pv.np_user))
            || (pv.np_sharp_wait && storage_ok()
                && artcache_npcover_seq() != pv.np_sharp_seq)))
    {
        /* a track change, or a deferred read once the disk woke on its
         * own (refill), or (np-sharp) the queued cover landed with the
         * disk still awake: no wake from here (8b-4a M1) */
        pv.np_track = np_pb_track();
        pv.np_deferred = false;
        pv.np_sharp_wait = false;
        ui_panel_invalidate();
        wake = MIN(wake == TIMEOUT_BLOCK ? INT_MAX : wake, MAX(PANEL_REST, 1));
    }
#ifdef HAVE_DRIFT_JOBS
    np_bcm_tick(lit);
#endif
    if (!lit)
    {
        /* dark: nothing of the drift runs, so the panel goes back to
         * the framebuffer (the unlit face settles in its own enter(),
         * which draws before any tick) */
        drift_settle();
        return wake;                /* drift and clocks idle while dark */
    }

    if (pv.anim)
    {
        if (!TIME_BEFORE(now, pv.anim_due))
        {
            drift_settle();
            set_anim(ui_panel_compose(&pv.sched.shown, pv.shown_leaf, NULL));
            push_panel("panel");
        }
        if (pv.anim)
            wake = MIN(wake == TIMEOUT_BLOCK ? INT_MAX : wake,
                       pv.anim_due - now);
    }
    else if (drift_shown())
    {
        int dw;
        ui_anim_idle();             /* the drift never runs boosted */
        dw = drift_held(now) ? drift_hold_tick(now) : drift_tick(phase, now);
        if (dw != TIMEOUT_BLOCK)
            wake = wake == TIMEOUT_BLOCK ? dw : MIN(wake, dw);
    }
    return wake;
}

void ui_panel_phase_poll(void)
{
    phase_update(current_tick, is_backlight_on(true));
}

long ui_panel_phase(void)
{
    return pv.phase.ticks;
}

#ifdef SIMULATOR
void ui_panel_phase_set(long ticks)
{
    nav_phase_set(&pv.phase, ticks, current_tick);
}

void ui_panel_phase_freeze(bool frozen)
{
    phase_live = frozen ? 0 : 1;
    nav_phase_set(&pv.phase, pv.phase.ticks, current_tick);
}
#endif
