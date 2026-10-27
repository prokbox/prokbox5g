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
#include "config.h"
#include "fat.h"                   /* #81: FSInfo */
#include <string.h>
#include "system.h"
#include "kernel.h"
#include "lcd.h"
#include "action.h"
#include "audio.h"
#include "sound.h"
#include "misc.h"
#include "settings.h"
#include "playlist.h"
#include "appevents.h"
#include "fe_be.h"
#include "xcore.h"          /* MF-4: lcd_fb_claim() */
#include "screen_access.h"
#include "viewport.h"
#include "wps.h"
#include "logf.h"
#include "panic.h"
#include "debug.h"
#include "backlight.h"
#include "powermgmt.h"
#include "power.h"
#include "audiobook.h"
#include "core_alloc.h"
#include "playback.h"         /* audio_get_filebuflen() */
#include "tagcache.h"
#ifdef HAVE_PITCHCONTROL
#include "tdspeed.h"
#endif
#ifdef HAVE_DAC_STANDBY
#include "dac_ext.h"
#endif
#include "ipodui/dbqueue_core.h"    /* #109: dbq_resume_refused() */
#include "ipodui/ipodui.h"
#include "ipodui/theme.h"
#include "ipodui/input.h"
#include "ipodui/screen_stack.h"
#include "ipodui/header.h"
#include "ipodui/menu.h"
#include "ipodui/anim.h"
#include "ipodui/qscroll.h"
#include "ipodui/library.h"
#include "ipodui/ab_book.h"
#include "ipodui/artcache.h"
#include "ipodui/rowart.h"
#include "ipodui/preview.h"
#include "ipodui/uilog.h"
#include "ipodui/bootstamp.h"          /* #87: the boot window */
#include "ipodui/powerlog.h"
#include "ipodui/battlog.h"
#include "ipodui/clockpol.h"
#include "ipodui/uilat.h"           /* np-lag probe */
#include "codec_cop.h"               /* np-lag: the decode's core */
#include "ipodui/batcal.h"
#include "ipodui/bootbatt.h"
#include "ipodui/unlit.h"
#include "ipodui/devmode.h"
#ifdef HAVE_BCMFW
#include "bcm-devtest.h"       /* bcm_boot_switch(), #60 */
#ifdef HAVE_BCMFW_RESIDENT
#include "video/bcmres.h"      /* bcmres_offer() */
#endif
#endif
#include "bcmjob_pp.h"           /* P19: bcmjob_init() */
#include "ipodui/art_bcm.h"      /* P19 19-6: art_bcm_service() */
#include "ipodui/usbscreen.h"
#include "usb_buf_alloc.h"       /* usb_buf_set_log() */
#include "ipodui/usbbusy.h"
#include "ipodui/notice.h"
#include "ipodui/sysscreen.h"
#include "ipodui/np_playback.h"
#include "ipodui/screens/screens.h"
#include "ipodui/screens/nowplaying.h"
#include "ipodui/screens/panels.h"   /* the reveal's backdrop (P12) */
#include "ipodui/sleep.h"
#include "ipodui/alarm.h"
#include "ipodui/lock.h"
#include "ipodui/modload.h"
#include "ipodui/alarm_ring.h"
#if defined(HAVE_DEVSTATS) && !defined(SIMULATOR)
#include "storage.h"                /* Fit-3b: the boot storage line */
#include "ata.h"
#endif
#include "ipodui/sdk/sdk_int.h"     /* P12 W4-0: sdk_takes_long_menu() */
#ifdef SIMULATOR
#include "stor_pwr.h"               /* sim_stor_ui_thread() (npfix) */
#endif
#include "hang_watch.h"             /* the hang watcher's heartbeat */

/* The header polls battery and hold. There is no hold-switch event for
 * the app (the driver only tells the backlight thread), so poll once a
 * second while the backlight is on -- the lock glyph then shows within
 * 1 s -- and every 5 s when it is off (nobody sees it). */
#define IDLE_TIMEOUT_LIT (HZ*1)
#define IDLE_TIMEOUT     (HZ*5)
#define PLAYING_TIMEOUT  (HZ/2)  /* play state glyph, while lit */

/* A slide in progress (spec index.md "Transitions"; durations from
 * nav_transition_ticks(), cubic ease-out). It moves one rect: below the
 * header between two full-width screens; only the list column between
 * two split screens (the panel stays put); the whole screen when a split
 * screen meets a full-width one (their headers differ): there, the
 * parting below unless UI_SLIDE_PART is 0. The framebuffer
 * rect holds the old screen, the strip the new one (captured by drawing
 * the new screen normally and swapping the two rects), so no second
 * framebuffer is needed: each frame shifts the framebuffer rect in place
 * by the newly due columns and copies the columns that appear from the
 * strip (stride = the rect's width). */
static struct {
    int  dir;                   /* +1 push (from the right), -1 pop, 0 idle */
    int  part;                  /* split <-> full: +1 parting, -1 closing */
    long start;
    int  ticks;                 /* duration */
    int  done;                  /* columns shifted so far, 0..w; parting:
                                 * the halves' eased progress, 0..1024 */
    int  hticks;                /* parting: the header phase's duration */
    int  hh;                    /* ... the full screen's header height */
    int  hdone;                 /* ... header rows moved so far, 0..hh */
    fb_data fill;               /* ... the full screen's body background:
                                 * the header band while no header shows */
    int  x, y, w, h;            /* the moving rect, LCD coordinates */
    bool reveal;                /* the Clocks reveal (below), not a slide */
    int  rv_h;                  /* ... the page's band rows from y 0 */
    int  rv_in;                 /* ... the bands' left edge last drawn */
    int  rv_out;                /* ... in: out_dx last drawn, -1 none yet */
    int  rv_frames;             /* ... frames drawn (the device log line) */
    unsigned long rv_us, rv_max_us; /* ... their cost: sum, worst */
} slide;

/* The Extras > Clocks reveal (P12 W2-W; spec index.md "Transitions" item
 * 4, measured on the owner's video, VIDEO-2 A3): the Extras backdrop (the
 * world map) stays put; the split screen's foreground (header, list, the
 * panel's clock and date) moves out_dx px left while the page's bands
 * stand with their left edge at in_x (nav_reveal_offsets()); back, the
 * mirror. Each frame is composed from three layers, bottom up:
 *  - the backdrop, ui_extras_page_backdrop() (resident, so the pop can
 *    draw it after the page's module has gone);
 *  - the split screen, from the strip: its list column opaque, its panel
 *    with the backdrop's own pixels keyed out to RV_KEY at capture (the
 *    backdrop rendered in chunks into the strip's spare tail);
 *  - the bands, on top: into the page, drawn by its reveal() op at in_x;
 *    back to the list (the page has gone), shifted right in place in the
 *    framebuffer, which holds them at the start.
 * The whole screen is pushed each frame (header + "anim:slide" bands, as
 * any full-height slide); one boost for its 250 ms. Memory: the strip
 * (its 153,600 B capture plus 25,600 B of its tail), nothing more. */
#define RV_KEY          ((fb_data)0xf81f)   /* magenta: not in the panel */
#define RV_FRAME_PX     (LCD_WIDTH * LCD_HEIGHT)
#define RV_CHUNK        ((int)((UI_STRIP_SIZE / sizeof(fb_data) - RV_FRAME_PX) \
                               / UI_PANEL_W))
typedef char reveal_fits_strip[RV_CHUNK >= 16 ? 1 : -1]
    __attribute__((unused));
static int reveal_h;            /* the top page's band rows (its reveal()),
                                 * kept for the pop after it has gone */

/* Split <-> full-screen (owner 2026-10-20, spec index.md "Transitions"
 * item 6): the split screen parts at the seam (split_x), header and
 * all -- the list column with its header part slides out to the left,
 * the panel (its shadow baked into its first columns, so the shadow
 * travels with it) with the header's right part out to the right --
 * uncovering the full screen's body, already drawn in place underneath,
 * and a cleared header band; then the full screen's header bar (its
 * glyphs with it) slides down from above the top edge. Back to a split
 * screen, the mirror: the full screen's header slides up and out, then
 * the halves close in with their header parts over the full screen,
 * which stays put, and meet at the seam. Same memory
 * as a full-height slide: the framebuffer and the strip's 320 x 240
 * capture (153,600 B), nothing more. 0 brings back the full-width slide
 * of every split <-> full push/pop (A/B, device tuning in 8b-4). */
#ifndef UI_SLIDE_PART
#define UI_SLIDE_PART   1
#endif
/* Durations: the measured split -> full push (250 ms) and its pop
 * (290 ms), spec index.md "Timing"; quadratic ease-out (brief O7). */
#define UI_PART_OPEN_TICKS  (HZ * 25 / 100)
#define UI_PART_CLOSE_TICKS (HZ * 29 / 100)
/* the header bar's slide (owner 2026-10-20: "just after the two split
 * panels have moved off frame"), quadratic ease-out */
#define UI_PART_HDR_TICKS   (HZ * 12 / 100)
#define UI_EASE_ONE         1024    /* eased progress units */

/* the largest rect, the whole screen, fits the strip (Rev 1) */
typedef char slide_fits_strip[LCD_WIDTH * LCD_HEIGHT * sizeof(fb_data)
                              <= UI_STRIP_SIZE ? 1 : -1]
    __attribute__((unused));

/* Hold NEXT/PREV seek (spec "Global button behaviour"): playback pauses
 * while held (audio_pre_ff_rewind()) and lands on release. */
static struct {
    int  dir;                   /* 0: not seeking */
    long start;                 /* tick of the hold */
    unsigned long from;         /* elapsed ms at the hold */
    const struct ui_screen *top;    /* the screen it started on */
} seek;

static struct ui_header_state header_state;
static int header_wake = TIMEOUT_BLOCK; /* Time in Title: next text swap */

bool ui_playback_resume(void)
{
    if (audio_status())
        return true;
    if (global_status.resume_index == -1)
        return false;

#ifdef HAVE_PITCHCONTROL
    sound_set_pitch(global_status.resume_pitch);
#endif
    /* #109 S1 (M1, review M-C): nothing to resume (-1) or a database
     * block dbq.hdr doesn't confirm (-19) is a non-resume; the other
     * codes (a torn last line, -15, ...) resume what was read, as before */
    if (dbq_resume_refused(playlist_resume()))
    {
        logf("ipodui: playlist_resume failed");
        return false;
    }
    lib_queue_resumed();            /* a book alone still stops at its end */
    playlist_resume_track(global_status.resume_index,
                          global_status.resume_crc32,
                          global_status.resume_elapsed,
                          global_status.resume_offset);
    return true;
}

/* Now Playing in the main menu (owner, Phase 6 Revision 3): shown while
 * the queue has not STOPPED. Playing or paused: shown. A natural end
 * (book ended, playlist ended without repeat) clears the resume info
 * (playlist_update_resume_info(NULL)); a stop or a cleared queue sets
 * queue_stopped (audio thread event); either hides it until playback
 * starts again. After a reboot it stays hidden until something has
 * played in this boot (owner, #58, overturning the Rev 3 ⚠️17 default):
 * PLAY still resumes the restored queue, and its first track change
 * shows the row. A books.dat spot alone never does. */
/* P19 S1: the queue's stopped flag is the back end's (fe_be.c, the now
 * record's FE_NOW_STOPPED), written by the playback events on the CPU */

/* usb_buf_alloc.h: the USB storage buffer's stop, retry and failure lines
 * (USB thread, at SET_CONFIGURATION) */
static void usb_buf_trace(const char *line)
{
    ui_log("usb", "%s", line);
}

#if defined(HAVE_BCM_JOBS) || defined(HAVE_BCM_JOBS_MODEL)
/* P19 W0-6: the job driver's lines ("bcmjob ...", "bcm power ...") into
 * the shell's log, the first word as the tag */
static void bcmjob_trace(const char *line)
{
    if (!strncmp(line, "bcmjob ", 7))
        ui_log("bcmjob", "%s", line + 7);
    else if (!strncmp(line, "bcm ", 4))
        ui_log("bcm", "%s", line + 4);
    else
        ui_log("bcm", "%s", line);
}

/* P19 19-6: an animation frame (a scroll, a slide) is due now: the job
 * owner's pass then writes no DECODE ring piece and reads no slot rows */
static bool bcmjob_frame_due(void)
{
    return ui_anim_active() && ui_anim_wait() == 0;
}
#endif

bool ui_now_playing_visible(void)
{
    struct fe_now now;
    bool v;

    (void)fe_now_get(&now);         /* a failure logs; the last good copy */
    v = audio_status()
        || (!(now.flags & FE_NOW_STOPPED) && global_status.resume_index != -1);

    UPDATE_TRACE("np row visible=%d", (int)v);  /* menu builds only */
    return v;
}

/* NAV-H1 (Phase 5): Now Playing is a screen on the stack. The nav stub
 * (screens/nowplaying.c) runs the old WPS from enter() and refuses the
 * push, so the screen below repaints in full. */
void ui_show_wps(void)
{
    if (!audio_status() && !ui_playback_resume())
        return;
    if (ui_push(&ui_nowplaying_screen, NULL) < 0)
        ui_invalidate(UI_DIRTY_ALL);    /* one full lcd_update() */
}

/* Shuffle Songs: every music track in the library (not audiobooks or
 * podcasts), built quietly from the database. */
void ui_shuffle_songs(void)
{
    if (lib_shuffle_all() < 0)
        return;                 /* logged; the library may be updating */
    ui_show_wps();
}

static void toggle_pause(void)
{
    int status = audio_status();

    if (status & AUDIO_STATUS_PAUSE)
        be_post(BE_AUDIO_RESUME, 0);
    else if (status & AUDIO_STATUS_PLAY)
        be_post(BE_AUDIO_PAUSE, 0);
    else
        ui_playback_resume();   /* the header picks up the new state */
}

static int min_wait(int a, int b);

/* Draws what is dirty (header and body) without pushing anything.
 * Returns whether the header changed. */
static bool draw_dirty(struct ui_screen *s, unsigned dirty)
{
    bool header;

    if (s->header_w == UI_HEADER_NONE)
    {
        /* P12 W2-W: a headerless page (Clocks): no header drawn or
         * pushed, whatever is dirty (the battery, Hold, Time in Title);
         * #84's busy moment advances, nothing drawn */
        struct ui_header_state quiet = { 0 };

        header_wake = ui_usbbusy_poll(&quiet, false);
        if (dirty & (UI_DIRTY_BODY | UI_DIRTY_BAND))
            s->ops->draw(s, dirty);
        if (dirty & UI_DIRTY_BODY)
            reveal_h = s->ops->reveal ? s->ops->reveal(s, LCD_WIDTH) : 0;
        return false;
    }
    if (s->header_w == UI_HEADER_DARK)
    {
        /* P11: a dark full-screen screen (About, Brightness, Charging):
         * the dark header with its own title, redrawn when forced or when
         * the battery changed; no Time in Title cycle */
        /* #84: the busy moment advances here too, nothing drawn */
        {
            struct ui_header_state quiet = { 0 };

            header_wake = ui_usbbusy_poll(&quiet, false);
        }
        header = ui_dark_header(s->title,
                                dirty & (UI_DIRTY_HEADER | UI_DIRTY_BODY));
        if (dirty & (UI_DIRTY_BODY | UI_DIRTY_BAND))
            s->ops->draw(s, dirty);
        return header;
    }

    /* the title, or the clock (Time in Title); frozen while dark */
    header_wake = ui_header_text(&header_state, s, s->title,
                                 is_backlight_on(true));
    header_state.width = s->header_w;
    ui_header_poll(&header_state);
    /* #84: the post-USB scan's activity glyph; its next step wakes us */
    header_wake = min_wait(header_wake,
                           ui_usbbusy_poll(&header_state,
                                           is_backlight_on(true)));
    header = ui_header_draw(&header_state, dirty & UI_DIRTY_HEADER);

    if (dirty & (UI_DIRTY_BODY | UI_DIRTY_BAND))
        s->ops->draw(s, dirty);
    return header;
}

#ifdef SIMULATOR
/* Simulator-only invariant (owner, device, 2026-10-24: scrolling Extras >
 * Alarms painted a full-width row over the panel): on a split screen a
 * band redraw (a scroll or glide step) never touches the panel. A hash of
 * the panel's columns over the dirty bands, before and after the draw;
 * a change logs "split overdraw" (scripts: "# nolog: split overdraw").
 * Not built for the target. */
static uint32_t panel_band_hash(const struct ui_screen *s)
{
    int y[UI_BANDS_MAX], h[UI_BANDS_MAX], i, n = ui_dirty_bands(y, h);
    uint32_t v = 2166136261u;

    lcd_set_viewport(NULL);         /* FBADDR uses the current viewport */
    for (i = 0; i < n; i++)
    {
        int r, x;

        for (r = MAX(y[i], 0); r < MIN(y[i] + h[i], LCD_HEIGHT); r++)
        {
            /* not ownable: a simulator-only check that reads the PP framebuffer itself, never the panel on the BCM */
            const fb_data *px = FBADDR(0, r);

            for (x = s->header_w; x < LCD_WIDTH; x++)
                v = (v ^ px[x]) * 16777619u;
        }
    }
    return v;
}
#endif

/* Draws what is dirty and pushes only those pixels to the LCD. */
static void refresh(struct ui_screen *s)
{
    const struct ui_theme *t = ui_theme;
    unsigned dirty = s->dirty;
    bool header;
    int y[UI_BANDS_MAX], h[UI_BANDS_MAX];
#ifdef SIMULATOR
    bool band_only = ui_screen_is_split(s) && (dirty & UI_DIRTY_BAND)
                     && !(dirty & UI_DIRTY_BODY);
    uint32_t before = band_only ? panel_band_hash(s) : 0;
#endif

    if ((dirty & UI_DIRTY_BODY) && !ui_screen_is_split(s))
        ui_panel_hide();            /* no panel: nothing may claim for it */
    header = draw_dirty(s, dirty);
#ifdef SIMULATOR
    if (band_only && panel_band_hash(s) != before)
        UPDATE_TRACE("split overdraw %s", s->ops->name);
#endif

    if (dirty & UI_DIRTY_BODY)
    {
        /* boot, return from the WPS, or a push/pop without a strip.
         * Always the whole screen: on a split screen the body includes
         * the panel next to the header. */
        UPDATE_TRACE("update full %s", s->ops->name);
        lcd_update();
        ui_panel_sync(LCD_HEIGHT);
    }
    else
    {
        if (header)
        {
            int hx = 0, hw = LCD_WIDTH;

            if (s->header_w != UI_HEADER_DARK)
                ui_header_push_rect(&hx, &hw);
            ui_lcd_push("header", hx, 0, hw,
                        s->header_w == UI_HEADER_DARK ? UI_DARK_HDR_H
                                                      : t->header_h);
        }
        if (dirty & UI_DIRTY_BAND)
        {
            int i, n = ui_dirty_bands(y, h);
            for (i = 0; i < n; i++)
                ui_lcd_push("band", 0, y[i], LCD_WIDTH, h[i]);
        }
    }
    ui_clear_dirty();
}

/* -- slide transitions -------------------------------------------------- */

enum xfer { TO_STRIP, TO_FB, SWAP };

/* Copies or swaps the framebuffer's slide rect and the strip, row by
 * row: memcpy (IRAM asm) only, a swap through one stack row (F4). */
static void rect_xfer(fb_data *strip, enum xfer how)
{
    fb_data tmp[LCD_WIDTH];
    size_t n = slide.w * sizeof(fb_data);
    int y;

    lcd_set_viewport(NULL);         /* FBADDR uses the current viewport */
    for (y = 0; y < slide.h; y++)
    {
        /* not ownable: a slide/parting frame; its capture settled the rect (slide_capture(), reveal_capture()) */
        fb_data *row = FBADDR(slide.x, slide.y + y);
        fb_data *srow = strip + y * slide.w;

        if (how == TO_STRIP)
            memcpy(srow, row, n);
        else if (how == TO_FB)
            memcpy(row, srow, n);
        else
        {
            memcpy(tmp, row, n);
            memcpy(row, srow, n);
            memcpy(srow, tmp, n);
        }
    }
}

/* The header kinds a slide tells apart (P11: dark screens; P12: none) */
enum { HDR_LIGHT, HDR_SPLIT, HDR_DARK, HDR_NONE };

static int hdr_kind(const struct ui_screen *s)
{
    if (s->header_w == UI_HEADER_DARK)
        return HDR_DARK;
    if (s->header_w == UI_HEADER_NONE)
        return HDR_NONE;
    return ui_screen_is_split(s) ? HDR_SPLIT : HDR_LIGHT;
}

static void slide_geometry(int from, int to)
{
    const struct ui_theme *t = ui_theme;

    slide.x = 0;
    slide.w = LCD_WIDTH;
    slide.y = to == HDR_DARK ? UI_DARK_HDR_H
            : to == HDR_NONE ? 0 : t->header_h;
    if (from == HDR_SPLIT && to == HDR_SPLIT)
        slide.w = t->split_x;       /* the panel stays put */
    else if (from != to)
        slide.y = 0;                /* full height: the header changes shape */
    slide.h = LCD_HEIGHT - slide.y;
}

static void slide_capture(int dir, bool push_above);
static void reveal_capture(int dir);

/* Called after a push/pop, before the new top screen has drawn. Without
 * a strip it does nothing and the normal full redraw follows. The
 * capture, the new screen's draw and the swap run boosted (F4): the
 * first frame follows at once. */
static void slide_start(int dir, int from)
{
    struct ui_screen *s = ui_top();
    int to = hdr_kind(s);

    if (!ui_strip_available())
        return;
    slide.reveal = false;
    if (ui_transition_kind() == UI_T_REVEAL
        && (dir > 0 ? from == HDR_SPLIT && to == HDR_NONE && s->ops->reveal
                    : from == HDR_NONE && to == HDR_SPLIT))
    {
        reveal_capture(dir);        /* P12 W2-W: the Clocks reveal */
        return;
    }
    slide_geometry(from, to);
    /* D7 durations by kind; UI_T_REVEAL and UI_T_SPLIT move as a push
     * in v1 (D6) */
    slide.ticks = nav_transition_ticks(ui_transition_kind(), dir,
                                       from == HDR_SPLIT, to == HDR_SPLIT);
    slide.part = 0;
#if UI_SLIDE_PART
    /* the parting goes by geometry, not by push or pop: into a full
     * screen it opens, back to a split one it closes. The reveal
     * (Clocks, P12) keeps its own motion, a system screen (Charging)
     * the plain slide. */
    if ((from == HDR_SPLIT) != (to == HDR_SPLIT) &&
        ui_transition_kind() != UI_T_REVEAL &&
        ui_transition_kind() != UI_T_SYSTEM)
    {
        slide.part = from == HDR_SPLIT ? 1 : -1;
        slide.ticks = slide.part > 0 ? UI_PART_OPEN_TICKS
                                     : UI_PART_CLOSE_TICKS;
        slide.hticks = UI_PART_HDR_TICKS;
        slide.hh = (slide.part > 0 ? to : from) == HDR_DARK
                   ? UI_DARK_HDR_H
                   : (slide.part > 0 ? to : from) == HDR_NONE
                   ? 0 : ui_theme->header_h;
        slide.hdone = 0;
    }
#endif
    slide_capture(dir, true);
}

/* P11 (usbscreen.h): a page slide inside the top screen (About) */
static struct {
    int dir, y, h;
} band_req;

void ui_slide_band(int dir, int y, int h)
{
    band_req.dir = dir;
    band_req.y = y;
    band_req.h = h;
}

/* After the screen's input(): the requested band slides, the rest of the
 * screen is redrawn in place (and must not have changed) */
static void slide_band_start(void)
{
    int dir = band_req.dir;

    band_req.dir = 0;
    if (!ui_strip_available())
    {
        ui_invalidate(UI_DIRTY_BODY);       /* the new page, at once */
        return;
    }
    slide.x = 0;
    slide.w = LCD_WIDTH;
    slide.y = MAX(band_req.y, 0);
    slide.h = MIN(band_req.h, LCD_HEIGHT - slide.y);
    slide.part = 0;
    slide.ticks = nav_transition_ticks(UI_T_PUSH, dir, false, false);
    /* P19 screen truth: the band is read from the framebuffer */
    ui_fb_settle(slide.x, slide.y, slide.w, slide.h);
    slide_capture(dir, false);
}

static void slide_capture(int dir, bool push_above)
{
    struct ui_screen *s = ui_top();
    fb_data *strip;
    bool kept;

    slide.reveal = false;
    /* P19 screen truth (3.6): the old rect is read from the framebuffer,
     * so the panel's pixels must be the framebuffer's first */
    ui_fb_settle(slide.x, slide.y, slide.w, slide.h);
    ui_anim_begin();
    strip = ui_strip_claim(UI_STRIP_OWNER_SLIDE, &kept);
    rect_xfer(strip, TO_STRIP);             /* old rect -> strip */
    ui_strip_put();                         /* drawing may yield */

    if (!ui_screen_is_split(s))
        ui_panel_hide();
    draw_dirty(s, UI_DIRTY_ALL);            /* new screen -> framebuffer */
    ui_clear_dirty();

    strip = ui_strip_claim(UI_STRIP_OWNER_SLIDE, &kept);
    if (!kept)
    {
        /* someone used the strip meanwhile: no old body, just show it */
        ui_strip_put();
        UPDATE_TRACE("update full %s", s->ops->name);
        lcd_update();
        ui_panel_sync(LCD_HEIGHT);
        return;
    }
    rect_xfer(strip, SWAP);                 /* fb: old rect, strip: new */
    ui_strip_put();

    /* outside the rect (the header above it, or the panel beside it)
     * the new screen just switches */
    if (push_above && slide.y > 0)
        ui_lcd_push("header", 0, 0, LCD_WIDTH, slide.y);
    /* the panel beside a list-column slide, if it changed (#58); a
     * full-width rect brings it in with the frames */
    ui_panel_sync(slide.x + slide.w < LCD_WIDTH ? slide.y : LCD_HEIGHT);

    slide.dir = dir;
    slide.start = current_tick - 1;     /* the first frame already moves */
    slide.done = 0;
}

static void slide_shift(fb_data *strip, int dx)
{
    int keep = slide.w - dx;
    int y;

    lcd_set_viewport(NULL);
    for (y = 0; y < slide.h; y++)
    {
        /* not ownable: a slide/parting frame; its capture settled the rect (slide_capture(), reveal_capture()) */
        fb_data *row = FBADDR(slide.x, slide.y + y);
        fb_data *srow = strip + y * slide.w;

        if (slide.dir > 0)
        {
            /* everything moves left; the new screen's next columns
             * appear at the right edge */
            memmove(row, row + dx, keep * sizeof(fb_data));
            memcpy(row + keep, srow + slide.done, dx * sizeof(fb_data));
        }
        else
        {
            /* everything moves right; the old screen's next columns
             * appear at the left edge */
            memmove(row + dx, row, keep * sizeof(fb_data));
            memcpy(row, srow + slide.w - slide.done - dx,
                   dx * sizeof(fb_data));
        }
    }
}

#if UI_SLIDE_PART
/* The parting's two halves at eased progress e (0..UI_EASE_ONE): how far
 * the list column (*l, 0..seam) and the panel (*r, 0..w - seam) have
 * moved from their resting places. */
static void part_offsets(int e, int *l, int *r)
{
    int seam = ui_theme->split_x;

    *l = (seam * e + UI_EASE_ONE / 2) / UI_EASE_ONE;     /* nearest px */
    *r = ((LCD_WIDTH - seam) * e + UI_EASE_ONE / 2) / UI_EASE_ONE;
}

/* One parting frame, from progress slide.done to e, over the full-screen
 * rect. Opening, the framebuffer holds the split screen's halves moved
 * apart by the last offsets with the full screen between them: each
 * half shifts outward and the columns it uncovers come from the strip
 * (the full screen). Closing, the framebuffer holds the full screen and
 * the strip the split one: each half is copied in at its new place,
 * over the full screen, which is never moved. Rows only: memmove/memcpy
 * of at most 320 px each. */
static void part_shift(fb_data *strip, int e)
{
    int seam = ui_theme->split_x;
    int l0, r0, l1, r1, y;

    part_offsets(slide.done, &l0, &r0);
    part_offsets(e, &l1, &r1);
    lcd_set_viewport(NULL);
    /* the whole height parts, the split header's two parts with their
     * halves (owner, device 2026-10-20) */
    for (y = 0; y < LCD_HEIGHT; y++)
    {
        /* not ownable: a slide/parting frame; its capture settled the rect (slide_capture(), reveal_capture()) */
        fb_data *row = FBADDR(0, y);
        fb_data *srow = strip + y * LCD_WIDTH;

        if (slide.part > 0)
        {
            /* the list column: old columns l1..seam-1 at 0.. */
            memmove(row, row + (l1 - l0), (seam - l1) * sizeof(fb_data));
            /* the panel: old columns seam..w-r1-1 at seam+r1.. */
            memmove(row + seam + r1, row + seam + r0,
                    (LCD_WIDTH - seam - r1) * sizeof(fb_data));
            if (y < slide.hh)
            {
                /* the header band uncovers no header yet: it comes
                 * down once the halves are gone */
                int x;
                for (x = seam - l1; x < seam - l0; x++)
                    row[x] = slide.fill;
                for (x = seam + r0; x < seam + r1; x++)
                    row[x] = slide.fill;
                continue;
            }
            memcpy(row + seam - l1, srow + seam - l1,
                   (l1 - l0) * sizeof(fb_data));
            memcpy(row + seam + r0, srow + seam + r0,
                   (r1 - r0) * sizeof(fb_data));
        }
        else
        {
            /* closing: progress e is how far each half has come in,
             * l1 = the list column's right edge, r1 = the panel's
             * width shown at the right edge */
            memcpy(row, srow + seam - l1, l1 * sizeof(fb_data));
            memcpy(row + LCD_WIDTH - r1, srow + seam,
                   r1 * sizeof(fb_data));
        }
    }
}
/* The header phase, from slide.hdone to k rows moved. Opening, the full
 * screen's header comes down from above the top edge: its bottom k rows
 * at the top, from the strip, over the cleared band. Closing, the full
 * screen's header (in the framebuffer) moves up by the new rows and the
 * band clears behind it (the split header comes in with the halves). */
static void part_header(fb_data *strip, int k)
{
    int y, dk = k - slide.hdone;
    size_t n = LCD_WIDTH * sizeof(fb_data);

    lcd_set_viewport(NULL);
    if (slide.part > 0)
    {
        for (y = 0; y < k; y++)
            /* not ownable: a slide/parting frame; its capture settled the rect (slide_capture(), reveal_capture()) */
            memcpy(FBADDR(0, y), strip + (slide.hh - k + y) * LCD_WIDTH, n);
        return;
    }
    for (y = 0; y < slide.hh - k; y++)
        /* not ownable: a slide/parting frame; its capture settled the rect (slide_capture(), reveal_capture()) */
        memcpy(FBADDR(0, y), FBADDR(0, y + dk), n);
    for (; y < slide.hh; y++)
    {
        /* not ownable: a slide/parting frame; its capture settled the rect (slide_capture(), reveal_capture()) */
        fb_data *row = FBADDR(0, y);
        int x;

        for (x = 0; x < LCD_WIDTH; x++)
            row[x] = slide.fill;
    }
}

/* Quadratic ease-out of el over ticks, 0..UI_EASE_ONE; the last frame
 * that fits in the duration lands it, so a late frame never makes the
 * motion overrun (the tail left is < 2.5 px of the halves) */
static int part_ease(long el, int ticks, bool finish)
{
    long inv;

    if (finish || el + UI_ANIM_FRAME_TICKS > ticks)
        return UI_EASE_ONE;
    if (el <= 0)
        return 0;
    inv = UI_EASE_ONE - el * UI_EASE_ONE / ticks;
    return UI_EASE_ONE - (int)(inv * inv / UI_EASE_ONE);
}

/* One parting frame: the halves' phase and the header's, in the order of
 * the direction (opening: halves, then header; closing: header, then
 * halves). Pushes only the band that moved. */
static void part_frame(fb_data *strip, long el, bool finish)
{
    bool halves_first = slide.part > 0;
    long el_h = halves_first ? el - slide.ticks : el;
    long el_b = halves_first ? el : el - slide.hticks;
    int e = el_b < 0 && !finish ? 0 : part_ease(el_b, slide.ticks, finish);
    int k = el_h < 0 && !finish ? 0
          : slide.hh * part_ease(el_h, slide.hticks, finish) / UI_EASE_ONE;
    bool body = false, head = false;

    if (!slide.done && !slide.hdone)
    {
        /* the full screen's own body background (owner, device
         * 2026-10-20), never a pixel of its first row, which may be the
         * selection bar: a light list's list_bg; a dark screen's body
         * gradient at its top, its first body row's right edge (the
         * strip's opening, the framebuffer's closing) */
        if (slide.hh != UI_DARK_HDR_H)
            slide.fill = (fb_data)ui_theme->list_bg;
        else
            slide.fill = slide.part > 0
                         ? strip[slide.hh * LCD_WIDTH + LCD_WIDTH - 1]
                         /* not ownable: a slide/parting frame; its capture settled the rect (slide_capture(), reveal_capture()) */
                         : *FBADDR(LCD_WIDTH - 1, slide.hh);
    }
    {
        /* landed once both halves are within half a pixel */
        int l, r;

        part_offsets(e, &l, &r);
        if (l == ui_theme->split_x && r == LCD_WIDTH - ui_theme->split_x)
            e = UI_EASE_ONE;
    }
    if (e > slide.done)
    {
        part_shift(strip, e);
        slide.done = e;
        body = true;
    }
    if (k > slide.hdone)
    {
        part_header(strip, k);
        slide.hdone = k;
        head = true;
    }
    if (slide.done >= UI_EASE_ONE && slide.hdone >= slide.hh)
        rect_xfer(strip, TO_FB);    /* the exact final image */
    if (head || body)
        ui_lcd_push("header", 0, 0, LCD_WIDTH, slide.hh);
    if (body)
        ui_lcd_push("anim:slide", 0, slide.hh, LCD_WIDTH,
                    LCD_HEIGHT - slide.hh);
}
#endif /* UI_SLIDE_PART */

static void slide_push(void)
{
    const struct ui_theme *t = ui_theme;

    if (slide.y == 0)
    {
        /* the full-height rect as header + body bands: the body stays
         * the "anim:slide" band of every other slide */
        ui_lcd_push("header", slide.x, 0, slide.w, t->header_h);
        ui_lcd_push("anim:slide", slide.x, t->header_h, slide.w,
                    slide.h - t->header_h);
    }
    else
        ui_lcd_push("anim:slide", slide.x, slide.y, slide.w, slide.h);
}

/* -- the reveal (above slide) -- */

/* The split screen's panel half in the strip: each pixel that is the
 * backdrop's own becomes RV_KEY (one of RV_KEY's colour moves 1 LSB) */
static void reveal_key(fb_data *strip)
{
    fb_data *bd = strip + RV_FRAME_PX;      /* the spare tail */
    const int seam = LCD_WIDTH - UI_PANEL_W;
    int y0, y, x;

    for (y0 = 0; y0 < LCD_HEIGHT; y0 += RV_CHUNK)
    {
        int h = MIN(RV_CHUNK, LCD_HEIGHT - y0);

        ui_extras_backdrop(bd, UI_PANEL_W, y0, h, NULL);
        for (y = 0; y < h; y++)
        {
            fb_data *row = strip + (y0 + y) * LCD_WIDTH + seam;
            const fb_data *b = bd + y * UI_PANEL_W;

            for (x = 0; x < UI_PANEL_W; x++)
                if (row[x] == b[x])
                    row[x] = RV_KEY;
                else if (row[x] == RV_KEY)
                    row[x] = RV_KEY ^ 1;
        }
    }
}

/* Into the page (dir +1) the strip takes the split screen as shown and
 * the page draws (its band rows); back (-1) the page as shown stays in
 * the framebuffer and the split screen drawn goes to the strip. */
static void reveal_capture(int dir)
{
    struct ui_screen *s = ui_top();
    fb_data *strip;
    bool kept;

    slide.x = slide.y = 0;
    slide.w = LCD_WIDTH;
    slide.h = LCD_HEIGHT;
    slide.part = 0;
    slide.ticks = nav_transition_ticks(UI_T_REVEAL, dir, dir < 0, dir > 0);
    ui_fb_settle(0, 0, LCD_WIDTH, LCD_HEIGHT);  /* P19 screen truth */
    ui_anim_begin();
    strip = ui_strip_claim(UI_STRIP_OWNER_SLIDE, &kept);
    rect_xfer(strip, TO_STRIP);             /* the screen shown -> strip */
    ui_strip_put();
    if (dir > 0)
        ui_panel_hide();                    /* the page has no panel */
    slide.rv_h = reveal_h;                  /* back: the page's, kept */
    draw_dirty(s, UI_DIRTY_ALL);            /* the new top */
    ui_clear_dirty();
    if (dir > 0)
        slide.rv_h = reveal_h;              /* in: the page just drawn */

    strip = ui_strip_claim(UI_STRIP_OWNER_SLIDE, &kept);
    if (!kept)
    {
        /* someone used the strip meanwhile: no old screen, just show it */
        ui_strip_put();
        UPDATE_TRACE("update full %s", s->ops->name);
        lcd_update();
        ui_panel_sync(LCD_HEIGHT);
        return;
    }
    if (dir < 0)
        rect_xfer(strip, SWAP);             /* fb: the page, strip: list */
    reveal_key(strip);
    ui_strip_put();
    ui_panel_sync(LCD_HEIGHT);              /* in with the frames */
    UPDATE_TRACE("reveal start dir=%d bands=%d", dir, slide.rv_h);

    slide.reveal = true;
    slide.rv_in = 0;
    slide.rv_out = -1;                      /* the page is in the fb */
    slide.rv_frames = 0;
    slide.rv_us = slide.rv_max_us = 0;
    slide.dir = dir;
    slide.start = current_tick - 1;         /* the first frame moves */
    slide.done = 0;
}

/* The backdrop a reveal frame needs under the split screen: back, all
 * of it the bands and the list column leave clear. In, after the first
 * frame, only what the split screen covered last frame (out_dx and the
 * bands only grow): columns from the list column's new right edge to
 * the split screen's last right edge; the rest is the backdrop already
 * or the bands. Returns the pixels stored (the frame trace). */
static long reveal_backdrop(int out_dx, int in_x)
{
    const int seam = LCD_WIDTH - UI_PANEL_W, b = slide.rv_h;
    int x0 = 0, x1 = LCD_WIDTH;

    if (slide.dir > 0 && slide.rv_out >= 0)
    {
        x0 = MAX(seam - out_dx, 0);
        x1 = LCD_WIDTH - slide.rv_out;
    }
    slide.rv_out = out_dx;
    ui_extras_page_backdrop_cols(0, b, x0, MIN(x1, in_x));
    ui_extras_page_backdrop_cols(b, LCD_HEIGHT - b, x0, x1);
    return (long)b * MAX(MIN(x1, in_x) - x0, 0)
           + (long)(LCD_HEIGHT - b) * MAX(x1 - x0, 0);
}

/* One reveal frame at offsets (out_dx, in_x), but the bands going in
 * (the page's reveal(), drawn once the strip is put back); returns the
 * backdrop pixels stored */
static long reveal_compose(fb_data *strip, int out_dx, int in_x)
{
    const int seam = LCD_WIDTH - UI_PANEL_W, b = slide.rv_h;
    int y, x;
    long bd;

    lcd_set_viewport(NULL);
    if (slide.dir < 0 && in_x > slide.rv_in)
        for (y = 0; y < b; y++)             /* back: the bands right */
        {
            /* not ownable: a slide/parting frame; its capture settled the rect (slide_capture(), reveal_capture()) */
            fb_data *row = FBADDR(0, y);

            memmove(row + in_x, row + slide.rv_in,
                    (LCD_WIDTH - in_x) * sizeof(fb_data));
        }
    slide.rv_in = in_x;
    bd = reveal_backdrop(out_dx, in_x);
    for (y = 0; y < LCD_HEIGHT && out_dx < LCD_WIDTH; y++)
    {
        /* not ownable: a slide/parting frame; its capture settled the rect (slide_capture(), reveal_capture()) */
        fb_data *row = FBADDR(0, y);
        const fb_data *src = strip + y * LCD_WIDTH;
        int lim = y < b ? in_x : LCD_WIDTH; /* the split screen's columns */
        int n = MIN(seam - out_dx, lim);

        if (n > 0)                          /* the list column, opaque */
            memcpy(row, src + out_dx, n * sizeof(fb_data));
        for (x = MAX(seam, out_dx); x < LCD_WIDTH && x - out_dx < lim; x++)
            if (src[x] != RV_KEY)           /* the panel's own pixels */
                row[x - out_dx] = src[x];
    }
    return bd;
}

#ifdef USEC_TIMER
#define RV_NOW_US() ((unsigned long)USEC_TIMER)
#else
#define RV_NOW_US() ((unsigned long)current_tick * (1000000UL / HZ))
#endif

static void reveal_frame(bool finish)
{
    long el = current_tick - slide.start, bd = 0;
    int out_dx, in_x;
    fb_data *strip;
    bool kept = false, ok;
    unsigned long t0 = RV_NOW_US();

    /* the last frame that fits in the duration lands it (as the
     * parting's): a late frame never makes the motion overrun */
    if (finish || el + UI_ANIM_FRAME_TICKS > slide.ticks)
        el = slide.ticks;
    nav_reveal_offsets(el, slide.ticks, slide.dir, &out_dx, &in_x);
    strip = ui_strip_claim(UI_STRIP_OWNER_SLIDE, &kept);
    ok = strip && kept;
    if (ok)
        bd = reveal_compose(strip, out_dx, in_x);
    /* put back before the page draws: its glyph loads may yield (anim.h)
     * and nothing below reads the strip */
    ui_strip_put();
    if (ok)
    {
        struct ui_screen *top = ui_top();
        unsigned long us;

        if (slide.dir > 0 && slide.rv_h > 0 && top->ops->reveal)
            top->ops->reveal(top, in_x);    /* in: the bands on top */
        slide_push();
        /* ms: the motion's time; us: this frame's cost (compose, bands,
         * push), bd: the backdrop pixels it stored */
        us = RV_NOW_US() - t0;
        slide.rv_frames++;
        slide.rv_us += us;
        slide.rv_max_us = MAX(slide.rv_max_us, us);
        UPDATE_TRACE("reveal frame ms=%ld out=%d in=%d us=%lu bd=%ld",
                     el * 1000 / HZ, out_dx, in_x, us, bd);
        (void)bd;
    }
    else
    {
        /* lost the capture: the new screen in full */
        ui_invalidate(UI_DIRTY_ALL);
        slide.dir = 0;
    }
    if (ok && el >= slide.ticks)
    {
        /* the device's line (ipodui.log; per-frame lines would only be
         * counted as repeats): the reveal's frames and their cost */
        ui_log("anim", "reveal dir=%d frames=%d us=%lu max=%lu",
               slide.dir, slide.rv_frames,
               slide.rv_us / MAX(slide.rv_frames, 1), slide.rv_max_us);
        slide.dir = 0;
    }
    if (!slide.dir)
        slide.reveal = false;
}

/* Draws the frame that is due (or the last one if 'finish'). */
static void slide_frame(bool finish)
{
    long el = current_tick - slide.start;
    int want, dx;
    fb_data *strip;
    bool kept = false;

    if (!finish && !ui_anim_frame_due())
        return;
    ui_anim_frame();
    if (slide.reveal)
    {
        reveal_frame(finish);       /* P12 W2-W */
        return;
    }

#if UI_SLIDE_PART
    if (slide.part)
    {
        strip = ui_strip_claim(UI_STRIP_OWNER_SLIDE, &kept);
        if (strip && kept)
            part_frame(strip, el, finish);
        ui_strip_put();
        if (!strip || !kept)
        {
            ui_invalidate(UI_DIRTY_ALL);
            slide.dir = 0;
        }
        else if (slide.done >= UI_EASE_ONE && slide.hdone >= slide.hh)
            slide.dir = 0;
        return;
    }
#endif
    if (finish || el >= slide.ticks)
        want = slide.w;
    else
    {
        /* cubic ease-out, 1/1024 units: 1 - (1 - p)^3 */
        long inv = 1024 - el * 1024 / slide.ticks;
        long rest = inv * inv / 1024 * inv / 1024;
        want = slide.w - (int)(slide.w * rest / 1024);
    }
    dx = want - slide.done;

    strip = ui_strip_claim(UI_STRIP_OWNER_SLIDE, &kept);
    if (strip && kept && dx > 0)
    {
        if (want == slide.w)
            rect_xfer(strip, TO_FB);        /* the exact final image */
        else
            slide_shift(strip, dx);
        slide.done = want;
        slide_push();
    }
    ui_strip_put();

    if (!strip || !kept)
    {
        /* lost the capture: redraw the new screen in full */
        ui_invalidate(UI_DIRTY_ALL);
        slide.dir = 0;
    }
    else if (slide.done >= slide.w)
        slide.dir = 0;
}

/* -- global buttons (spec index.md "Global button behaviour", Rev 3) ---- */

/* A tap of NEXT/PREV from any screen: skip, or restart / previous track
 * by the 3 s rule; chapters in an audiobook. The screen stays. */
static void skip(int dir)
{
    struct mp3entry *id3 = audio_current_track();
    struct audiobook_chapters *ab = id3 ? id3->audiobook : NULL;

    if (!audio_status() || !id3)
        return;
    UPDATE_TRACE("skip %s", dir > 0 ? "next" : "prev");
    if (dir > 0)
    {
        if (ab && ab->chapter_count > 0
            && ab_chapter_skip(ab, 1, id3->elapsed))
            return;
        audio_next();
    }
    else if (id3->elapsed < AB_SKIP_THRESH)
        audio_prev();
    else if (ab && ab->chapter_count > 0)
        ab_chapter_skip(ab, -1, id3->elapsed);
    else
    {
        audio_pre_ff_rewind();
        audio_ff_rewind(0);
    }
}

/* Hold NEXT/PREV: audio ms covered after 'held' ticks (~5x for 2 s, then
 * 20x; no reference, ~). The Now Playing scrubber (Phase 5) is the
 * visible seek. */
unsigned long ui_seek_span_ms(long held)
{
    if (held < 2 * HZ)
        return held * 50;
    return 2 * HZ * 50 + (held - 2 * HZ) * 200;
}

static void seek_end(void)
{
    struct mp3entry *id3 = audio_current_track();
    unsigned long d = ui_seek_span_ms(current_tick - seek.start);
    unsigned long pos;

    if (id3)
    {
        if (seek.dir > 0)
        {
            unsigned long end = id3->length > 1000 ? id3->length - 1000 : 0;
            pos = MIN(seek.from + d, end);
        }
        else
            pos = seek.from > d ? seek.from - d : 0;
        UPDATE_TRACE("seek %s to %lu ms", seek.dir > 0 ? "fwd" : "back",
                     pos);
        audio_ff_rewind(pos);
    }
    seek.dir = 0;
}

/* NEXT/PREV the top screen left alone. */
static void skip_or_seek(const struct ui_input *in)
{
    int dir = in->key == UI_KEY_NEXT ? 1 : -1;
    struct mp3entry *id3;

    if (in->release)
    {
        if (seek.dir)
            seek_end();
        else if (!in->long_press)
            skip(dir);
        return;
    }
    if (!in->long_press || seek.dir)
        return;                     /* a press, or a repeat of the hold */
    id3 = audio_current_track();
    if (!audio_status() || !id3)
        return;
    seek.dir = dir;
    seek.top = ui_top();
    seek.start = current_tick;
    seek.from = id3->elapsed;
    audio_pre_ff_rewind();
}

/* A hold whose release never came (a dropped event, a screen that took
 * the release): lands the seek on anything else, so playback does not
 * stay paused (end review #38). */
static void seek_check(const struct ui_input *in)
{
    if (!seek.dir)
        return;
    if (ui_top() != seek.top
        || (in && in->key != UI_KEY_NEXT && in->key != UI_KEY_PREV))
    {
        UPDATE_TRACE("seek: no release, landing (%s)",
                     in ? "other input" : "screen change");
        seek_end();
    }
}

/* The charging sleep below is the device's path (ipodvideo cannot power
 * off on the charger). config.h defines HAVE_POWEROFF_WHILE_CHARGING for
 * every simulator, so the sim models the target here instead, and
 * scripts reach it with "charger on" + hold PLAY (P11). */
#if CONFIG_CHARGING && \
    (!defined(HAVE_POWEROFF_WHILE_CHARGING) || defined(SIMULATOR))
#define UI_CHARGING_SLEEP
#endif

#ifdef UI_CHARGING_SLEEP
/* Hold PLAY with the charger in: this target cannot power off while
 * charging (clean_shutdown() would only save the status, then the power
 * is cut ~20 s later with nothing stopped or flushed). Sleep as the 7G
 * does instead: playback paused, resume state and settings saved, then
 * the Charging screen (P11 E1, sysscreen.c), an ordinary stack screen:
 * the backlight times out on it as on any screen (the unlit face, then
 * LCD sleep). Any key leaves it, paused. The hold's release is
 * swallowed. */
static bool sleep_on_release;

static void charging_sleep_prepare(void)
{
    if ((audio_status() & (AUDIO_STATUS_PLAY | AUDIO_STATUS_PAUSE))
        == AUDIO_STATUS_PLAY)
        audio_pause();
    if (audio_status())             /* the resume point, as a stop would */
        playlist_update_resume_info(audio_current_track());
    status_save(true);              /* resume point + settings */
    sdk_persist();                  /* P12: a game's record */
    sleep_on_release = true;
    UPDATE_TRACE("sleep: charging, paused=%d saved",
                 (int)((audio_status() & AUDIO_STATUS_PAUSE) != 0));
}
#endif

/* Holds that act from every screen before it sees them: MENU (to the
 * main menu) and PLAY (sleep). True if consumed. */
static bool global_hold(const struct ui_input *in)
{
#ifdef UI_CHARGING_SLEEP
    if (sleep_on_release && in->key == UI_KEY_PLAY && in->release)
    {
        /* P11 (Rev 1 H-1): the Charging screen is up; the release is
         * swallowed and the backlight times out as on any screen (the
         * unlit face, then LCD sleep), no forced backlight_off() */
        sleep_on_release = false;
        UPDATE_TRACE("sleep: release swallowed%s", "");
        return true;
    }
#endif
    if (!in->long_press)
        return false;
    if (in->key == UI_KEY_MENU)
    {
        /* P12 W4-0 (Q29): a Class A game's long MENU leaves the game */
        if (sdk_takes_long_menu())
            return false;
        if (!in->release && ui_depth() > 1)
        {
            UPDATE_TRACE("hold MENU: to the main menu from %s",
                         ui_top()->ops->name);
            ui_pop_to_root();
        }
        return true;                /* its release does nothing more */
    }
    if (in->key == UI_KEY_PLAY)
    {
        if (!in->release)
        {
            /* P12 Q5: hold PLAY while ringing dismisses first, then
             * sleeps */
            if (alarm_ringing())
            {
                alarm_ring_end(ALARM_END_DISMISS);
                alarm_ring_close();
            }
#ifdef UI_CHARGING_SLEEP
            if (charger_inserted())
            {
                charging_sleep_prepare();   /* the save comes first */
                /* P11 E1: the big battery, an ordinary pushed screen
                 * (sysscreen.c); not again if it is already up */
                if (ui_top()->ops != &ui_sys_charging_screen)
                    ui_push_t(&ui_sys_charging_screen, NULL, UI_T_SYSTEM);
                return true;
            }
#endif
            /* P12 Q1(2): an alarm due within a day and no bootloader
             * alarm window yet: no power-off (Hold at the alarm's power-on
             * would start the other firmware), the dark wait instead */
            if (!ui_sleep_soft_get() && alarm_blocks_poweroff())
            {
                alarm_dark_wait();
                return true;
            }
            UPDATE_TRACE("sleep: hold PLAY%s", "");
            /* P18 (#86): soft sleep, or today's power-off by the tier;
             * the main loop runs it before its next draw */
            ui_sleep_request(SLEEP_HOLD_PLAY);
        }
        return true;
    }
    return false;
}

/* -- main loop ---------------------------------------------------------- */

static bool shell_started;

/* usbscreen.h: true once the shell's fonts and strip are reserved (the
 * USB screen draws its static form before that: boot-time devmode) */
bool ui_shell_started(void)
{
    return shell_started;
}

#ifdef HAVE_DAC_STANDBY
/* P15 (docs/phase15-brief.md section 2.4): the codec's standby counts,
 * traced when they change (firmware can't trace). The device log gets the
 * same line at most every DAC_LOG_TICKS: the counts are cumulative, so a
 * skipped line loses nothing. */
#define DAC_LOG_TICKS (HZ*10)
static void dac_trace(void)
{
    static unsigned int last_sleeps, last_wakes;
#ifndef SIMULATOR
    static long log_tick;
    static bool log_owed;
#endif
    struct dac_stats st;

    dac_get_stats(&st);
    if (st.sleeps != last_sleeps || st.wakes != last_wakes)
    {
        last_sleeps = st.sleeps;
        last_wakes = st.wakes;
        UPDATE_TRACE("dac standby=%d sleeps=%u wakes=%u wake_us=%u",
                     st.standby, st.sleeps, st.wakes, st.last_wake_us);
#ifndef SIMULATOR
        log_owed = true;
#endif
    }
#ifndef SIMULATOR
    if (log_owed && TIME_AFTER(current_tick, log_tick + DAC_LOG_TICKS))
    {
        log_owed = false;
        log_tick = current_tick;
        ui_log("dac", "standby=%d sleeps=%u wakes=%u wake_us=%u",
               st.standby, st.sleeps, st.wakes, st.last_wake_us);
    }
#endif
}

/* P15 end review M1: a headphone click while stopped wakes the codec, and
 * the audio thread may be blocked with no timeout by then; the shell polls
 * at 2 Hz while a standby is due and nothing plays, else it blocks */
static int dac_wait(void)
{
    int st = audio_status();

    if (!dac_idle_pending()
        || ((st & AUDIO_STATUS_PLAY) && !(st & AUDIO_STATUS_PAUSE)))
        return TIMEOUT_BLOCK;
    dac_idle_poll();
    return dac_idle_pending() ? HZ/2 : TIMEOUT_BLOCK;
}
#else
static inline int dac_wait(void) { return TIMEOUT_BLOCK; }
#endif /* HAVE_DAC_STANDBY */

#ifdef HAVE_TC_RAMCACHE
/* 8b brief 3.3: the library's empty window (every list empty, the
 * "Updating Library..." footer), from the tagcache thread's own tick
 * stamps, logged once per window on the next pass after it closed (exact
 * even when the pass comes late, e.g. while dark). empty_ms: the lists
 * were empty; total_ms: from the post-USB scan's start (-1: no scan
 * started this window, e.g. the boot load or a rebuild). */
static void lib_window_log(void)
{
    static long logged;                 /* 'ready' of the last line */
    long scan, empty, ready, total, gone;
    int playing;

    tagcache_ui_window(&scan, &empty, &ready);
    if (!ready || ready == logged)
        return;
    /* empty == 0: no window (an update built behind a live library) */
    total = scan && (!empty || !TIME_AFTER(scan, empty))
            && TIME_AFTER(scan, logged)
            ? (ready - scan) * 1000 / HZ : -1;
    gone = empty ? (ready - empty) * 1000 / HZ : -1;
    logged = ready;
    playing = (audio_status() & AUDIO_STATUS_PLAY) ? 1 : 0;
    /* two device lines: a log line holds 61 characters (uilog.h) */
    ui_log("lib", "window empty_ms=%ld total_ms=%ld playing=%d", gone,
           total, playing);
    ui_log("lib", "window scan=%ld empty=%ld ready=%ld", scan, empty, ready);
    UPDATE_TRACE("lib window scan=%ld empty=%ld ready=%ld empty_ms=%ld "
                 "total_ms=%ld playing=%d", scan, empty, ready, gone, total,
                 playing);
}
#else
static inline void lib_window_log(void) {}
#endif /* HAVE_TC_RAMCACHE */

/* P11 (docs/phase11-brief.md section 4.1): the screen arena, once, at
 * shell start, before anything can start playback. Pinned for life:
 * screens keep plain pointers into it (screen_stack.h). Without it every
 * ui_arena_alloc() fails and the screens that need one refuse to open
 * (logged); nothing else depends on it. */
static void arena_reserve(void)
{
    const size_t bytes = UI_ARENA_SIZE + UI_ARENA_ALIGN - 1;
    int h = core_alloc(bytes);

    if (h <= 0)
    {
        UPDATE_TRACE("arena none size=%u", (unsigned)bytes);
        return;
    }
    core_pin(h);
    ui_arena_init(core_get_data(h), bytes);
    UPDATE_TRACE("arena reserve size=%u free=%u", (unsigned)bytes,
                 (unsigned)ui_arena_free());
}

/* np-lag: the codec decodes on this (the UI thread's) core. Today the
 * CPU, unless a developer A/B mode put the run on the COP; with the UI
 * on the COP (19-9) the answer flips by itself */
static bool decode_on_ui_core(void)
{
    bool codec_cop = false;
#ifdef HAVE_COP_CODEC
    codec_cop = codec_cop_active();
#endif
#if NUM_CORES > 1
    return codec_cop == (CURRENT_CORE == COP);
#else
    return !codec_cop;
#endif
}

/* Do the UI's frames compete with decode for the clock? Inline or on a CPU
 * thread: decode runs on the UI's core. On the COP (ucop-fix, owner bench
 * 2026-10-26: slower, the split panel behind the cursor while music
 * plays): both cores run on one clock, and every back-end call the shell
 * makes is served on the CPU beside the codec, so the input boost applies
 * there too. */
static bool ui_competes_with_decode(void)
{
    return decode_on_ui_core() || fe_be_mode() == FE_COP;
}

static int min_wait(int a, int b)
{
    if (a == TIMEOUT_BLOCK)
        return b;
    if (b == TIMEOUT_BLOCK)
        return a;
    return MIN(a, b);
}

static void shell_main(void);

/* P19 S1/S3: the shell's start splits at the job driver.  Part one runs
 * on the main thread (the CPU): the log, the boot records, the BCM switch
 * and the job driver's init (it allocates and pins; BCM session).  Then
 * fe_be_start() runs shell_main() as the front end: on this thread
 * (FE_INLINE, the default: exactly as before), or on the ui thread, with
 * this thread turned into the back end's service thread be_svc. */
void ipodui_main(void)
{
    fe_be_init();                   /* the now record, the playback events */
    ui_log_init();                  /* the log's idle flush (idempotent) */
    usb_buf_set_log(usb_buf_trace); /* before any USB configuration */
    ui_log("boot", "variant %s", IPOD_VARIANT);    /* Phase 9 (7.8) */
#ifdef HAVE_PANIC_RECORD
    ui_log_panic_record();          /* the last boot's panic, if any */
#endif
#if defined(HAVE_DEVSTATS) && !defined(SIMULATOR) && \
    (CONFIG_STORAGE & STORAGE_ATA)
    {   /* Fit-3b: what this unit's storage reports (one build for flash
         * and disk); RAM only, out with the boot flush */
        const unsigned short *id = ata_get_identify();
        ui_log("storage", "id w0=%04x w76=%04x w83=%04x w93=%04x w163=%04x",
               id[0], id[76], id[83], id[93], id[163]);
        ui_log("storage", "id w168=%04x w217=%04x ssd=%d spinup_ms=%d",
               id[168], id[217], ata_disk_isssd(),
               storage_spinup_time() * (1000 / HZ));
    }
#endif
#ifndef SIMULATOR
    {   /* #81: the FSInfo a host will find (0xffffffff: unknown, and a
         * host then walks the whole FAT before it mounts) */
        unsigned long fc = 0, nf = 0, cl = 0;
        if (fat_get_fsinfo(IF_MV(0,) &fc, &nf, &cl))
            ui_log("fat", "fsinfo free=%lu next=0x%lx cl=%lu", fc, nf, cl);
        {   /* #81: the dirty flags (a dirty volume makes macOS run its
             * full repair check before mounting) */
            unsigned int bf = 0;
            unsigned long f1 = 0;
            if (fat_get_dirty(IF_MV(0,) &bf, &f1))
                ui_log("fat", "dirty bs41=%02x fat1=%08lx clean=%d herr=%d",
                       bf, f1, (int)((f1 >> 27) & 1), (int)(!((f1 >> 26) & 1)));
        }
    }
#endif
    powerlog_boot();                /* #10, devmode: a log left on resumes */
    /* P18 Q4 (18-1 review M1): the idle timer now asks the shell for a
     * soft sleep (input.c, sleep.c) instead of powering off */
    ui_sleep_soft_boot();           /* the developer switch, default Off */
    powermgmt_set_sleep_handler(true);
    batcal_boot();                  /* #70: a run's end, the learned table
                                     * (be_svc's: P19 S2) */
    clockpol_init();                /* Fit-1 C6: 24 MHz idle, PLL off */
    ui_usb_host_markers();          /* no Spotlight on the library */
#ifdef HAVE_BCMFW
    /* #60: our VideoCore image in place of Apple's when the toggle and a
     * stage pass flag (developer builds) or verified.txt (the build's
     * default, HAVE_BCMFW_DEFAULT) allow it; after main.c's RIGHT-held
     * Developer escape.  A no-op once level 1's early swap ran. */
#ifdef HAVE_BCMFW_RESIDENT
    bcmres_offer();                 /* the resident image as the driver */
#endif
    bcm_boot_switch();
#endif
#if defined(HAVE_BCM_JOBS) || defined(HAVE_BCM_JOBS_MODEL)
    /* P19 W0-6: the job client's state (sim: the resident image's
     * reservation); nothing is sent to the BCM2722 until a caller asks */
    bcmjob_init(bcmjob_trace);
    bcmjob_frame_due_hook(bcmjob_frame_due);  /* 19-6: the owner's budget */
    {
        /* review M5: the floor with the BCM's reservation, on the device
         * as in the simulator (the resident copy counts once L2 loads
         * it; today only while our image is the resident one) */
#ifdef HAVE_BCM_JOBS
        unsigned res = bcm_boot_resident_active() ? BCM_RESIDENT_BYTES : 0;
#else
        unsigned res = BCM_RESIDENT_BYTES;  /* the model's reservation */
#endif
        ui_log("mem", "floor dev audio=%lu bcm_res=%u free=%lu",
               (unsigned long)audio_get_filebuflen(), res,
               (unsigned long)core_available());
    }
#endif
    fe_be_start(shell_main);        /* never returns */
}

static void shell_main(void)
{
    struct ui_input in;

    action_set_input_thread();      /* action_userabort(): here only */
#if defined(HAVE_BCM_JOBS) || defined(HAVE_BCM_JOBS_MODEL)
    bcmjob_claim_owner();           /* 19-9: the ring's owner is this thread,
                                     * before its first bcmjob call */
#endif
    /* MF-4 (xcore.h): the framebuffer's producer is this thread's core,
     * before its first draw; a CPU thread that pushes the frame (the wake
     * frame, the devtest's hand-back) then asks for fe_fb_publish() and
     * reads uncached.  Developer builds; nothing runs on one core */
    lcd_fb_claim();
    /* the hang watcher: this thread's passes (and its input waits, idle:
     * button_get_w_tmo()) from here */
    hang_watch_register(HANG_UI);
    ui_log("fe", "shell up mode=%s", fe_be_mode_name(fe_be_mode()));
    /* P11 T1 (owner, docs/phase11-brief.md section 1.2): the stored Time
     * in Title setting (default Off) before the first header draw, so a
     * boot with the default never shows a clock frame */
    ui_header_set_time_in_title(global_settings.ipodui_time_in_title);
    push_current_activity(ACTIVITY_MAINMENU);
    viewportmanager_theme_enable(SCREEN_MAIN, false, NULL);
#ifdef HAVE_BACKDROP_IMAGE
    /* the shell draws no backdrop; a theme's (config.cfg) would make every
     * DRMODE_INVERSEVID fill copy from it, offscreen buffers included */
    lcd_set_backdrop(NULL);
#endif
#ifdef HAVE_PITCHCONTROL
    /* P6 Rev 1 item 5: audiobook speed needs time-stretch; an old
     * config.cfg may say off. tdspeed's buffers come now, before any
     * playback (inactive at 100 %) */
    if (!global_settings.timestretch_enabled)
    {
        global_settings.timestretch_enabled = true;
        dsp_timestretch_enable(true);
    }
#endif
    /* the playback events: fe_be_init(), on the main thread (P19 S1) */
#ifdef SIMULATOR
    /* tests only: shots.sh "# resume: playing" seeds a queue that counts
     * as played this boot, so a script gets the Now Playing row without
     * starting playback (tests/ui/README.md) */
    if (file_exists(ROCKBOX_DIR "/.sim_np_row"))
        be_post(BE_NOW_PLAYED, 0);
#ifdef HAVE_STORAGE_POWEROFF
    sim_stor_ui_thread(thread_self());  /* npfix: "stor_watch_ui" */
#endif
#endif
    ab_book_init();                 /* P6: the book block + books.dat
                                     * index, before any playback */
    ui_theme_init();
    ui_eq_apply();                  /* the stored preset, once: an old config
                                     * may have the EQ on under "Off" */
    ui_qscroll_load();              /* once, before any playback */
    ui_strip_init();                /* once; without it nothing animates */
    ui_nowplaying_reserve();        /* the "np" block, at shell start (P5 Rev 3) */
    ui_preview_reserve();           /* once, before any playback: the
                                     * drift slot (and the panel scratch
                                     * if UI_PANEL_SCRATCH_SIZE > 0);
                                     * without them panels stay plain */
    ui_rowart_init();               /* once, before any playback: list
                                     * thumbnails and glyph tiles */
    artcache_reserve();             /* once, before any playback: the art
                                     * builder's and reader's blocks */
    lib_reserve();                  /* once, before any playback: the
                                     * library precompute's block */
    arena_reserve();                /* P11: once, before any playback: the
                                     * screen arena (screen_stack.h) */
    mod_init();                     /* #106: once, before any playback: the
                                     * module arena (modload.h); P12 W1-F:
                                     * the resident build's table too */
    /* P12: alarms.dat, an alarm power-on, misses, the PMU armed */
    alarm_init();
#ifdef HAVE_TC_RAMCACHE
    /* #87: the boot screen holds while the library's boot check runs (the
     * no-change stamp, #89), capped (Q4); after the reservations above, so
     * the RAM cache never lands below them. P12 (5.2): an alarm power-on
     * goes straight to the ring; the window's optional work waits */
    if (!alarm_ringing())
        maint_boot_window();
#endif
    shell_started = true;
    /* the root menu keeps static state (menu.c), so this cannot fail
     * for lack of memory; the stack is empty */
    if (ui_push(&ui_menu_screen, &ui_main_menu) < 0)
        panicf("ipodui: root");
    /* P12 Screen Lock: a locked unit's first frame is the locked screen,
     * never one of the Main Menu (lock.h) */
    (void)ui_lock_gate();
    /* the first-boot legal notice, once per build (notice.h): over the
     * menu and a locked screen; the boot work above is already done */
    ui_notice_boot();

    for (;;)
    {
        struct ui_screen *s = ui_top();
        enum ui_result r;
        bool animating = false;
        int timeout, wake = TIMEOUT_BLOCK, dir, np_wake;
        int from_split;             /* hdr_kind() of the screen left */

        hang_beat(HANG_UI);         /* a pass: progress */
#if defined(SIMULATOR) && defined(HAVE_HANG_RECORD)
        {
            /* tests only (sim script "hang_ui MS"): this thread stuck at
             * work, never yielding, as a frozen device's would be */
            extern volatile long sim_hang_ui_ms;
            if (sim_hang_ui_ms)
            {
                long end = current_tick + sim_hang_ui_ms * HZ / 1000;
                sim_hang_ui_ms = 0;
                while (TIME_BEFORE(current_tick, end))
                    ;
            }
        }
#endif
        if (ui_sleep_pending())
        {
            /* P18: dark, the entry window, asleep; back on the screen it
             * left (Q3), redrawn in full from the RTC before the light */
            if (slide.dir)
                slide_frame(true);  /* land a slide first */
            ui_anim_idle();
            if (ui_sleep_run())
            {
                /* Time in Title: the minute from the RTC now (the dark
                 * redraw below never reads the clock itself), M6 */
                ui_header_clock(NULL);
                ui_unlit_foreign_draw();
                /* P12 W2-W: the first lit frame is the right screen for
                 * every wake: an alarm's ring (pushed here, not a pass
                 * later), over the locked screen if locked; locked, the
                 * locked screen, never the one beneath. The lock first,
                 * so a ring lands over it even when it was not on the
                 * stack (the gate defers to a ring already up). No
                 * slide. */
                (void)ui_lock_gate();
                alarm_poll_prelight();
                (void)ui_take_transition();
                ui_invalidate(UI_DIRTY_ALL);
                refresh(ui_top());
                ui_sleep_lit();
            }
            continue;
        }
        mod_pass();                 /* #106: preloads landing, the batch;
                                     * P12 W1-F: the class accountant and
                                     * the boost-burst cap, every build */
        alarm_poll();               /* P12: a due ring, the beep, a write
                                     * owed since USB; boot and every wake */
        ui_unlit_tick();            /* the face's minute and status, the
                                     * end of a wake press's swallowing */
        /* P19 S2: the back end's per-pass services (log rotation, power
         * log, battery session and calibration, the drained latch, the
         * clock policy, the view pool's size) on be_svc; this thread
         * waits for them, as when it ran them itself */
        (void)be_call(BE_PASS, 0, 0, 0);
#ifdef HAVE_ART_BCM
        /* P19 19-6: DECODE offered (the merged image's overlay loaded
         * through the strip at rest); a cover slot's FREE on the job
         * owner */
        art_bcm_service(!slide.dir && !ui_anim_active());
#endif
#ifdef HAVE_DAC_STANDBY
        dac_trace();                /* P15: codec standby counts */
#endif
        lib_window_log();           /* 8b: a closed empty window, once */
        if (ui_unlit_active())
        {
            /* H2: the face suspends drawing, never logic. The top screen,
             * header, panels and slides wait; playback housekeeping
             * (refill) runs, only Now Playing's auto-return push waits
             * for the dismiss. One wake a minute, lit or dark. */
            slide.dir = 0;          /* the dismiss redraws in full */
            ui_anim_idle();
            timeout = min_wait(ui_unlit_timeout(), np_pb_idle());
            timeout = min_wait(timeout, dac_wait());
            timeout = min_wait(timeout, (int)alarm_timeout());
            timeout = min_wait(timeout, ipodui_devmode_ab_wait());
            timeout = min_wait(timeout, (int)settings_save_wait());
            ui_input_get(&in, timeout); /* every key goes to the face */
            continue;
        }
        ui_panel_phase_poll();      /* lit time only */
        if (slide.dir)
        {
            slide_frame(false);
            animating = slide.dir != 0;
        }
        if (!slide.dir)
        {
            /* a full repaint is due (the face dismissed, the USB screen
             * gone, a lost slide): it goes first, so the tick starts
             * from a correct framebuffer and pushes no band the full
             * update covers (#58: a row slide captured the face) */
            if (s->dirty & UI_DIRTY_BODY)
                refresh(s);
            if (s->ops->tick)
            {
                from_split = hdr_kind(s);
                wake = s->ops->tick(s);
                if (wake == UI_TICK_ANIM)
                {
                    animating = true;
                    wake = TIMEOUT_BLOCK;
                }
                /* the tick pushed, or turned its screen into another
                 * page (ui_restage(): the video's failure prompt): the
                 * slide starts now, over the framebuffer it left, not
                 * after the next key */
                dir = ui_take_transition();
                if (dir)
                {
                    slide_start(dir, from_split);
                    continue;
                }
            }
            refresh(s);
            /* the panel: arrivals, drift, animated providers; only at
             * rest on a split screen (D15) */
            if (!animating && ui_screen_is_split(s))
            {
                int pw = ui_panel_tick();
                if (pw == UI_TICK_ANIM)
                    animating = true;
                else
                    wake = min_wait(wake, pw);
            }
        }

        /* a screen that asked to leave from its tick (Now Playing once
         * playback stopped): popped here, never through the input path,
         * so a wake press's swallowing can't eat it (8a fix M2) */
        if (ui_take_pop_request())
        {
            UPDATE_TRACE("pop request %s", s->ops->name);
            from_split = hdr_kind(s);
            ui_anim_idle();
            ui_pop();
            dir = ui_take_transition();
            if (dir)
                slide_start(dir, from_split);
            continue;
        }

        /* NAV-H2: Now Playing's background work (refill), every pass */
        np_wake = ui_nowplaying_idle();
        /* np-lag: the input boost's release (its hold ran out) */
        np_wake = min_wait(np_wake, clockpol_input_wait());
        uilat_service();            /* np-lag probe (developer builds) */
        if (animating)
        {
            /* at least a tick: button_get_w_tmo(0) does not yield */
            timeout = MAX(min_wait(ui_anim_wait(), np_wake), 1);
        }
        else
        {
            bool lit = is_backlight_on(true);

            ui_anim_idle();         /* drops the boost after the last frame */
            if (s->ops->timeout)
                timeout = s->ops->timeout(s);
            else if (!lit)
                timeout = IDLE_TIMEOUT;     /* nobody sees the glyphs */
            else if (audio_status() & AUDIO_STATUS_PLAY)
                timeout = PLAYING_TIMEOUT;
            else
                timeout = IDLE_TIMEOUT_LIT;
            timeout = min_wait(timeout, wake);
            timeout = min_wait(timeout, np_wake);
            timeout = min_wait(timeout, ui_unlit_timeout());
            timeout = min_wait(timeout, dac_wait());
            timeout = min_wait(timeout, (int)alarm_timeout());  /* P12 */
            timeout = min_wait(timeout, ipodui_devmode_ab_wait());
            /* cfgsafe: a changed setting's write (BE_PASS) */
            timeout = min_wait(timeout, (int)settings_save_wait());
            if (lit)                /* the next title <-> clock swap */
                timeout = min_wait(timeout, header_wake);
        }

        if (!ui_input_get(&in, timeout))
            continue;
        /* the art builder keeps off for 2 s; np-lag: lit input while the
         * UI's frames compete with decode boosts for a second past the
         * last event (clockpol.c). A post: no round trip per event with
         * the UI on the COP (ucop-fix) */
        fe_note_input(ui_competes_with_decode());
        ui_nowplaying_note_input(); /* idle auto-return to Now Playing */
        if (ui_header_note_input()) /* Time in Title: the title again */
            ui_invalidate(UI_DIRTY_HEADER);

        seek_check(&in);
        if (slide.dir)
            slide_frame(true);      /* input wins: land the slide first */
        from_split = hdr_kind(s);
        /* anything but the wheel may block (an action, a push that loads,
         * the WPS): never hold the boost through it. A glide that is
         * still running boosts again on its next frame. */
        if (in.key != UI_KEY_WHEEL)
            ui_anim_idle();
#ifdef HAVE_IPOD_UI_DEVMODE
        /* the hidden chord, armed on the About screen only (M6) */
        if (ipodui_devmode_feed(s, in.raw))
            r = UI_HANDLED;
        else
#endif
        if (!ui_notice_up() && global_hold(&in))
            r = UI_HANDLED;
        else
            r = s->ops->input ? s->ops->input(s, &in) : UI_IGNORED;
        if (r == UI_POP)
        {
            ui_pop();
        }
        else if (r == UI_IGNORED)
        {
            if (in.key == UI_KEY_NEXT || in.key == UI_KEY_PREV)
                skip_or_seek(&in);
            else if (in.release && !in.long_press)
            {
                if (in.key == UI_KEY_MENU)
                    ui_pop();
                else if (in.key == UI_KEY_PLAY)
                    toggle_pause();
            }
        }

        seek_check(NULL);           /* the screen changed under a hold */
        dir = ui_take_transition();
        if (dir)
        {
            band_req.dir = 0;       /* the screen went: no page slide */
            slide_start(dir, from_split);
        }
        else if (band_req.dir)
            slide_band_start();     /* P11: About's page turn */
    }
}
