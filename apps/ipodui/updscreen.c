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
#include <string.h>
#include "system.h"
#include "kernel.h"
#include "lcd.h"
#include "font.h"
#include "lang.h"
#include "audio.h"
#include "power.h"
#include "powermgmt.h"
#include "tagcache.h"
#include "ipodui/anim.h"
#include "ipodui/theme.h"
#include "ipodui/paint.h"
#include "ipodui/header.h"
#include "ipodui/screen_stack.h"
#include "ipodui/usbscreen.h"
#include "ipodui/usbbusy.h"
#include "ipodui/dbqueue.h"
#include "ipodui/uilog.h"
#include "ipodui/preview.h"     /* ui_fb_settle() (P19) */
#include "ipodui/updscreen.h"

/* Geometry: the 7G's bar (usb-ok-to-disconnect.png, 320 x 240) is 200 px
 * wide from x 60, about 11 rows; ours sits 10 rows higher to leave room
 * for the step line under it. The title keeps the USB screen's baseline. */
#define BAR_X       60
#define BAR_W       200
#define BAR_Y       197
#define BAR_H       11
#define STEP_BASE   224                 /* Reg 12 (cap 9) */
#define BAND_Y      (BAR_Y - 1)
#define BAND_H      (STEP_BASE + 4 - BAND_Y)
#define TEXT_W      300

#define PEND_TICKS  (HZ)                /* unplug: shorter work, no page */
#define QUIET_TICKS (HZ)                /* phases a gap apart: one run */
#define FULL_TICKS  (HZ / 3)            /* the full bar, then the menu */
#define STEP_TICKS  (HZ / UPD_FPS)
#define STUCK_TICKS (10 * HZ)           /* boot: no work and no library */

enum { MODE_BOOT, MODE_UNPLUG };

/* The phases (TC_UI_PROG_*) and where each one's share of the bar ends
 * (permille), per mode. A first build (#237): the count of the tree,
 * then the walk, which reads every file's tags and is nearly all of the
 * wait (the device's 0.8.0 logs: ~70 ms a file against a commit of
 * seconds), the commit, the load and the lists. After an unplug (#157):
 * the post-USB runs' measured shares (8b brief 3.3, p16 logs) without
 * the art pass, which fills in behind the menu here. A scan owed but not
 * started counts as PH_PROBE. */
enum { PH_NONE = TC_UI_PROG_NONE, PH_PROBE = TC_UI_PROG_PROBE,
       PH_WALK = TC_UI_PROG_WALK, PH_COMMIT = TC_UI_PROG_COMMIT,
       PH_JOBS = TC_UI_PROG_JOBS, PH_COUNT = TC_UI_PROG_COUNT, PH_N };
static const short ph_end[2][PH_N] = {
    { 0, 20, 960, 985, 995, 20 },       /* MODE_BOOT */
    { 0, 250, 650, 900, 990, 250 },     /* MODE_UNPLUG */
};

struct upd {
    long t0;                    /* the push */
    long quiet;                 /* work last seen */
    long next;                  /* the next sample */
    long full;                  /* the bar reached 1000 (done) */
    unsigned usb;               /* ui_usb_sessions() at the push */
    unsigned scans;             /* tagcache_ui_scans_ended() at the push */
    short val;                  /* the bar, permille, never down */
    short drawn;                /* the bar drawn, -1 none */
    short ph_start;             /* val when the phase began */
    signed char phase;
    signed char step;           /* the step line drawn, -1 none */
    unsigned char mode;
    bool shown, done;
    bool power;                 /* boot: waiting for power (the build held) */
    bool held;                  /* the page's build hold is taken */
    unsigned char yields;       /* UPD_YIELD_*: the hold given back meanwhile */
};
_Static_assert(sizeof(struct upd) <= UI_ARENA_B_UPDATE, "upd: arena bound");

static const struct ui_screen_ops upd_screen;
static struct upd *upd_live;    /* the page up (entered, not left) */

/* The library build as the page's dedicated job (full speed, few yields:
 * no menu to serve). apps/tagcache.c owns the boost and the yields
 * (p081-buildspeed's counted holder). Only upd_hold_sync() calls it. */
static void upd_dedicated_build(bool on)
{
    tagcache_dedicated_build(on);
#if defined(SIMULATOR) && defined(HAVE_IPOD_UI)
    {   /* tests: every on is matched by one off (the scripts' "depth=0") */
        static int depth;
        depth += on ? 1 : -1;
        UPDATE_TRACE("upd dedicated depth=%d", depth);
    }
#endif
}

/* The page's one hold on the build: taken while the page is up, the
 * build not waiting for power and nothing it yielded to running (a
 * sleep's entry to its wake, a USB session: the device's power_sleep()
 * refuses any boost held, and no boost goes through a USB session).
 * Every take and give of the hold is here, so it is one at most, and
 * the leave gives it back. */
static void upd_hold_sync(struct upd *u, const char *why)
{
    bool want = u == upd_live && !u->power && !u->yields;

    if (want == u->held)
        return;
    u->held = want;
    upd_dedicated_build(want);
    UPDATE_TRACE("upd hold %s why=%s", want ? "take" : "give", why);
    (void)why;
}

/* The library work running now and its progress (permille, -1 unknown).
 * The art pass is not part of it: the songs are there without it. */
static int upd_sample(int *pm)
{
    int ph = tagcache_ui_progress(pm);

    if (ph != TC_UI_PROG_NONE)
        return ph;
    *pm = -1;
    /* the publication, the RAM load, the lists, a statistics job */
    if (tagcache_ui_busy() || tagcache_ui_updating() || dbq_working())
        return PH_JOBS;
    /* posted and able to run (deferred while audio holds the memory: the
     * busy glyph's rule, usbbusy.c) */
    if (tagcache_ui_scan_owed() && audio_status() == 0)
        return PH_PROBE;
    return PH_NONE;
}

/* the step line's identity: the phase, or -2 while waiting for power */
static int step_id(const struct upd *u)
{
    return u->power ? -2 : u->phase;
}

static int step_of(const struct upd *u)
{
    if (u->power)
        return LANG_IPODUI_LIB_STEP_POWER;
    switch (u->phase)
    {
        case PH_WALK:
            return u->mode == MODE_BOOT ? LANG_IPODUI_LIB_STEP_ADD
                                        : LANG_IPODUI_UPD_LIBRARY;
        case PH_COMMIT:
            return LANG_IPODUI_LIB_STEP_SORT;
        case PH_JOBS:
            return LANG_IPODUI_LIB_STEP_FINISH;
        default:            /* nothing yet, the count, a probe */
            return u->mode == MODE_BOOT ? LANG_IPODUI_LIB_STEP_FIND
                                        : LANG_IPODUI_UPD_LIBRARY;
    }
}

/* -- drawing ------------------------------------------------------------ */

static unsigned mix(unsigned c0, unsigned c1, int j, int n)
{
    return ui_paint_blend(c0, c1, n > 1 ? j * 15 / (n - 1) : 0);
}

/* The rounded bar: a dark recessed track, the light glossy fill over its
 * first 'fill' columns (a square right end, as the 7G's). Rows straight
 * into the framebuffer over the body the caller drew; the round ends get
 * one blended pixel of coverage (4-bit). */
static void draw_bar(int fill)
{
    const unsigned t_top = LCD_RGBPACK(18, 22, 31),
                   t_bot = LCD_RGBPACK(58, 66, 82),
                   f_top = LCD_RGBPACK(252, 253, 254),
                   f_mid = LCD_RGBPACK(226, 231, 238),
                   f_bot = LCD_RGBPACK(178, 188, 202);
    const int r2 = BAR_H * BAR_H;       /* (2r)^2, doubled coordinates */
    int j, x;

    lcd_set_viewport(NULL);             /* FBADDR uses the current one */
    /* P19 screen truth: the edges blend over what the framebuffer holds */
    ui_fb_settle(BAR_X, BAR_Y, BAR_W, BAR_H);
    for (j = 0; j < BAR_H; j++)
    {
        fb_data *row = FBADDR(BAR_X, BAR_Y + j);
        unsigned tc = mix(t_top, t_bot, j, BAR_H);
        unsigned fc = j < BAR_H / 2 ? mix(f_top, f_mid, j, BAR_H / 2)
                                    : mix(f_mid, f_bot, j - BAR_H / 2,
                                          BAR_H - BAR_H / 2);
        int dy = 2 * j + 1 - BAR_H;     /* doubled, from the bar's axis */

        for (x = 0; x < BAR_W; x++)
        {
            /* the distance into the round end, doubled */
            int ex = x < BAR_H / 2 ? 2 * x + 1 - BAR_H
                   : x >= BAR_W - BAR_H / 2 ? 2 * (x - (BAR_W - BAR_H)) + 1
                                              - BAR_H
                   : 0;
            int d2 = ex * ex + dy * dy, a;
            unsigned c = x < fill ? fc : tc;

            if (ex && d2 >= r2)
            {
                /* the edge band: coverage over ~2 doubled units */
                a = (r2 + 2 * BAR_H - d2) * 15 / (4 * BAR_H);
                if (a <= 0)
                    continue;
                row[x] = ui_paint_blend(row[x], c, MIN(a, 15));
            }
            else
                row[x] = c;
        }
    }
}

static void draw_text(int y_base, int font, unsigned colour, const char *s)
{
    struct viewport vp;

    memset(&vp, 0, sizeof(vp));
    vp.width = LCD_WIDTH;
    vp.height = LCD_HEIGHT;
    vp.flags = VP_DEFAULT_FLAGS;
    vp.drawmode = DRMODE_FG;
    vp.font = font;
    lcd_set_viewport(&vp);
    lcd_set_foreground(colour);
    ui_paint_text_centre(LCD_WIDTH / 2, y_base, TEXT_W, s);
    lcd_set_viewport(NULL);
}

static void draw_band(struct upd *u)
{
    ui_dark_body(BAND_Y, BAND_H);
    draw_bar((int)((long)BAR_W * u->val / 1000));
    draw_text(STEP_BASE, ui_theme->font_small, LCD_RGBPACK(206, 212, 222),
              str(step_of(u)));
    if (u->drawn > u->val)
        UPDATE_TRACE("upd bar back %d %d", u->drawn, u->val);
    UPDATE_TRACE("upd draw bar=%d step=%d", u->val, step_id(u));
    u->drawn = u->val;
    u->step = (signed char)step_id(u);
}

/* -- the screen ----------------------------------------------------------- */

static int upd_enter(struct ui_screen *s, const void *arg)
{
    struct upd *u = ui_arena_alloc(sizeof(*u));

    if (!u)
    {
        ui_log("usb", "update screen: no arena");
        return -1;
    }
    memset(u, 0, sizeof(*u));
    s->priv = u;
    s->header_w = UI_HEADER_DARK;
    s->title = ui_header_time_in_title() ? ui_header_clock(NULL)
                     : (const char *)str(LANG_IPODUI_DEVICE_NAME);
    u->mode = (unsigned char)(intptr_t)arg;
    u->t0 = u->quiet = u->next = current_tick;
    u->usb = ui_usb_sessions();
    u->scans = tagcache_ui_scans_ended();
    u->drawn = -1;
    u->step = -1;
    u->shown = u->mode == MODE_BOOT;    /* unplug: a second's grace */
    s->hidden = !u->shown;              /* nothing drawn or pushed meanwhile */
    upd_live = u;
    upd_hold_sync(u, "enter");
    UPDATE_TRACE("upd push mode=%s", u->mode == MODE_BOOT ? "boot"
                                                           : "unplug");
    return 0;
}

static bool power_low(void)
{
    int pct = battery_level();

    return !charger_inserted() && pct >= 0
           && pct < TAGCACHE_UI_BUILD_MIN_PCT;
}

/* Boot page (owner 2026-11-09): low on battery and no charger, the build
 * is held at a safe point (tagcache's sleep-entry cut: the walk stops at
 * its next folder, a candidate is dropped and owed again; a first build
 * does not start, tagcache.c) until power comes back. Asserted at every
 * sample while low (a sleep's wake clears the cut). */
static void upd_power(struct upd *u)
{
    bool low = u->mode == MODE_BOOT && power_low();

    if (low)
        tagcache_ui_sleep_cut(true);
    else if (u->power)
        tagcache_ui_sleep_cut(false);
    if (low != u->power)
    {
        u->power = low;
        /* the build held for power runs nothing: no boost meanwhile (on
         * a low battery of all times); back to full speed with power */
        upd_hold_sync(u, low ? "power" : "power back");
        /* the build starts over: its walk fills the rest of the bar from
         * where it stands (a new phase), never back */
        u->phase = PH_NONE;
        ui_log("db", "library page: %s batt=%d",
               low ? "waits for power" : "power back", battery_level());
    }
}

static void upd_leave(struct ui_screen *s)
{
    struct upd *u = s->priv;

    if (u->power)
        tagcache_ui_sleep_cut(false);   /* the build held for power: free */
    upd_live = NULL;
    upd_hold_sync(u, "leave");
    ui_log("usb", "update screen %s ms=%ld shown=%d",
           u->done ? "done" : "left",
           (long)(current_tick - u->t0) * (1000 / HZ), u->shown);
}

static void upd_draw(struct ui_screen *s, unsigned dirty)
{
    struct upd *u = s->priv;

    if (!u->shown)
        return;                 /* the USB screen's frame stays */
    if (dirty & UI_DIRTY_BODY)
    {
        ui_dark_body(UI_DARK_HDR_H, LCD_HEIGHT - UI_DARK_HDR_H);
        if (u->mode == MODE_BOOT)
            ui_usb_sync_disc();
        else
            ui_usb_eject_disc();
        draw_text(UI_USB_TEXT1_BASE, ui_theme->font_ui, LCD_WHITE,
                  str(u->mode == MODE_BOOT ? LANG_IPODUI_LIB_PAGE_TITLE
                                           : LANG_IPODUI_OK_TO_DISCONNECT));
        draw_band(u);
    }
    else if (dirty & UI_DIRTY_BAND)
        draw_band(u);
}

/* the page is over: the menu (a plain slide), or nothing drawn yet (a
 * cut: the frame under it just shows) */
static int upd_end(struct ui_screen *s, const char *why)
{
    struct upd *u = s->priv;

    UPDATE_TRACE("upd %s ms=%ld", why, (current_tick - u->t0) * (1000 / HZ));
    (void)why;
    if (u->shown)
        ui_pop_request(s);
    else
        ui_pop_request_cut(s);
    return TIMEOUT_BLOCK;
}

static int upd_tick(struct ui_screen *s)
{
    struct upd *u = s->priv;
    long now = current_tick;
    int ph, pm;
    bool settled;

    if (ui_usb_sessions() != u->usb)
        return upd_end(s, "usb");   /* ui_upd_after_usb() decides anew */
    if (u->done)
    {
        if (TIME_BEFORE(now, u->full + FULL_TICKS))
            return u->full + FULL_TICKS - now;
        return upd_end(s, "done");
    }
    if (TIME_BEFORE(now, u->next))
        return u->next - now;
    u->next = now + STEP_TICKS;

    upd_power(u);
    ph = upd_sample(&pm);
    if (u->power)
        u->quiet = now;         /* held on purpose: never "stuck" */
    if (ph != PH_NONE)
    {
        int end = ph_end[u->mode][ph], v = u->val;

        u->quiet = now;
        if (ph != u->phase)
        {
            u->phase = (signed char)ph;
            u->ph_start = u->val;
        }
        /* within the phase by its own units; a phase that comes round
         * again (a probe, then the scan) starts from where the bar is */
        if (pm >= 0 && end > u->ph_start)
            v = u->ph_start + (end - u->ph_start) * pm / 1000;
        if (v > u->val)
            u->val = (short)v;
    }

    /* over: no work for a while and, at boot, the library is there
     * (loaded: the menu shows it at once); after an unplug, the post-USB
     * moment has closed too */
    settled = ph == PH_NONE && !TIME_BEFORE(now, u->quiet + QUIET_TICKS);
    if (u->mode == MODE_BOOT)
    {
        if (settled && tagcache_ui_count() < 0)
        {
            /* nothing runs and still no library (a build that cannot
             * start: memory, an unreadable disk): the menu after a
             * while rather than a page that never ends; logged */
            if (TIME_BEFORE(now, u->quiet + STUCK_TICKS))
                return STEP_TICKS;
            ui_log("db", "library page: no library, no work: the menu");
            return upd_end(s, "stuck");
        }
    }
    else if (!u->shown)
    {
        if (ph != PH_NONE && !TIME_BEFORE(now, u->t0 + PEND_TICKS))
        {
            u->shown = true;
            s->hidden = false;
            UPDATE_TRACE("upd show ms=%ld", (now - u->t0) * (1000 / HZ));
            ui_invalidate(UI_DIRTY_ALL);
        }
        else if (ph == PH_NONE && (!ui_usbbusy_moment()
                 || tagcache_ui_scans_ended() != u->scans))
            /* nothing ran, or it ended inside the first second (the
             * post-USB scan has ended: no grace to wait out) */
            return upd_end(s, "skip");
        return STEP_TICKS;
    }
    else
        settled = settled && !ui_usbbusy_moment();

    if (settled)
    {
        u->done = true;
        u->full = now;
        u->val = 1000;
        ui_invalidate_band(BAND_Y, BAND_H);
        return FULL_TICKS;
    }
    /* a gap between phases keeps the last phase (and its words) */
    if (u->val != u->drawn || step_id(u) != u->step)
        ui_invalidate_band(BAND_Y, BAND_H);
    return STEP_TICKS;
}

static int upd_timeout(struct ui_screen *s)
{
    struct upd *u = s->priv;
    long now = current_tick;

    if (u->done)
        return MAX(u->full + FULL_TICKS - now, 1);
    return MAX(u->next - now, 1);
}

/* Every key, the wheel and chords do nothing while the library is being
 * made current (owner, 2026-11-09: the menu shows the music as it is);
 * hold MENU too (ui_upd_up(), ui_main.c). Hold PLAY still sleeps or
 * powers off: a cut build is made again at the next boot. */
static enum ui_result upd_input(struct ui_screen *s, const struct ui_input *in)
{
    (void)s;
    if (in->key != UI_KEY_NONE && in->release)
        UPDATE_TRACE("upd key ignored%s", "");
    return UI_HANDLED;
}

static const struct ui_screen_ops upd_screen = {
    .name    = "sys-update",
    .enter   = upd_enter,
    .leave   = upd_leave,                /* arena: released by the pop */
    .draw    = upd_draw,
    .input   = upd_input,
    .timeout = upd_timeout,
    .tick    = upd_tick,
};

void ui_upd_yield(unsigned what, bool yield)
{
    struct upd *u = upd_live;
    static const char *const why[2][2] = {
        { "sleep", "wake" }, { "usb", "usb end" } };

    if (!u)
        return;
    if (yield)
        u->yields |= what;
    else if (what == UPD_YIELD_USB && ui_usb_sessions() != u->usb)
        return;     /* a session ran: the page is over (upd_tick(), then
                     * ui_upd_after_usb() decides anew); nothing to take */
    else
    {
        u->yields &= ~what;
        /* the page did not sample meanwhile (the UI thread was in the
         * sleep's entry window, asleep or in the handler): that time is
         * not "no work", and a build cut at the entry starts again a
         * moment after the wake, so its quiet counts from now (else a
         * window of more than STUCK_TICKS ends the page as "stuck") */
        u->quiet = u->next = current_tick;
    }
    upd_hold_sync(u, why[what == UPD_YIELD_USB][!yield]);
}

bool ui_upd_up(void)
{
    struct ui_screen *top = ui_top();

    return top && top->ops == &upd_screen;
}

static void upd_push(int mode)
{
    if (ui_push_t(&upd_screen, (const void *)(intptr_t)mode,
                  UI_T_SYSTEM) < 0)
        return;
    (void)ui_take_transition();     /* no slide: a cut from what shows */
    if (mode == MODE_UNPLUG)
        ui_top()->dirty = 0;        /* the USB frame stays: no draw yet */
}

void ui_upd_boot(void)
{
    long t0 = current_tick;
    int none;

    /* the tagcache thread looks at the disk at its start: normally long
     * before this; a moment's wait at most */
    while ((none = tagcache_ui_no_library()) < 0
           && TIME_BEFORE(current_tick, t0 + 2 * HZ))
        sleep(HZ / 50);
    ui_log("db", "library page at boot: %s",
           none > 0 ? "no library" : none < 0 ? "unknown" : "library");
    if (none > 0)
        upd_push(MODE_BOOT);
}

void ui_upd_after_usb(void)
{
    /* the last session's page, still up when this one began: its work
     * is this session's now */
    if (ui_upd_up())
    {
        ui_pop();
        (void)ui_take_transition();
    }
    /* no library yet (the session cut a first build, or the computer
     * removed it): the boot page again, at once */
    if (tagcache_ui_no_library() > 0)
    {
        upd_push(MODE_BOOT);
        return;
    }
    if (!ui_usb_host_wrote())
    {
        UPDATE_TRACE("upd skip nowrite%s", "");
        return;
    }
    upd_push(MODE_UNPLUG);
}
