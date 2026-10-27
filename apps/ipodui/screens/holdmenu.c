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
 * iPod UI shell: the hold-SELECT menu
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
/* Holding SELECT on a song, a list row or in Now Playing (owner photo
 * hold-select-popup-np-b: from Now Playing). Not an overlay: a full-screen
 * ordinary list under the normal header, no chevrons, Cancel last; the
 * header keeps the title of the screen it was opened from. Items by
 * context (ui_hold_items(), ⚠️ H1):
 *   Now Playing, a song         Add to On-The-Go, Browse Album,
 *                               Browse Artist, Cancel
 *   an album, artist, genre,    Add to On-The-Go, Cancel
 *   composer, book or show row
 *   a song inside On-The-Go     Remove from On-The-Go, Browse Album,
 *                               Browse Artist, Cancel
 *   Music (main menu)           Rebuild Cache, Build Artwork, Cancel
 * A book or a podcast episode never gets the Browse items (Rev 1 L-6), nor
 * a track whose database entry is gone (a rebuild since).
 *
 * The menu opens on the long press while SELECT is still down; the
 * release of that press is dropped (Rev 1 L-6), so it never acts on Add.
 *
 * Add confirms nothing visible (stock just returns). Browse Album pushes
 * the album's songs, Browse Artist the artist's albums ("All Songs"
 * first); a compilation's track goes to its album artist (⚠️ H7: the
 * library's artist rows key on the album artist). From Now Playing the
 * menu and Now Playing both leave first, so MENU returns to the screen
 * Now Playing was opened from (⚠️ H2).
 *
 * State: the screen arena (UI_ARENA_B_HOLD). No I/O except the action.
 *
 * Trace (UPDATE_TRACE):
 *   hold open ctx=<np|song|group|otg> items=<mask, decimal>
 *   hold act <add|remove|album|artist|cancel> rc=<n> */
#include "config.h"
#include <string.h>
#include <stdio.h>
#include "string-extra.h"
#include "system.h"
#include "kernel.h"
#include "lcd.h"
#include "font.h"
#include "lang.h"
#include "logf.h"
#include "audio.h"
#include "tagcache.h"
#include "ipodui/ipodui.h"
#include "ipodui/artcache.h"
#include "ipodui/theme.h"
#include "ipodui/list.h"
#include "ipodui/anim.h"
#include "ipodui/otg.h"
#include "ipodui/screens/screens.h"
#include "ipodui/screens/holdmenu.h"
#include "ipodui/screens/nowplaying.h"

#define HOLD_TITLE_LEN  40

struct hold {                   /* what the actions need of the arg */
    struct ui_list list;
    int32_t tc_idx, db_gen;     /* 0: no (valid) track */
    int32_t view, idx, otg_pos;
    unsigned char ctx;          /* enum ui_hold_ctx */
    bool swallow;               /* the opening press's SELECT release */
    bool maintenance;           /* Rebuild Cache: persistent progress page */
    bool art_only;              /* Build Artwork: no library scan */
    unsigned char item[UI_HOLD_ITEMS];  /* enum ui_hold_item per row */
    uint32_t shown;             /* the progress text on screen (a hash) */
};
_Static_assert(sizeof(struct hold) <= UI_ARENA_B_HOLD, "hold: arena bound");

static const short item_lang[UI_HOLD_ITEMS] = {
    LANG_IPODUI_ADD_TO_OTG, LANG_IPODUI_REMOVE_FROM_OTG,
    LANG_IPODUI_BROWSE_ALBUM, LANG_IPODUI_BROWSE_ARTIST,
    LANG_IPODUI_REBUILD_CACHE, LANG_IPODUI_BUILD_ARTWORK, LANG_CANCEL_0,
};
static const char *const item_name[UI_HOLD_ITEMS] = {
    "add", "remove", "album", "artist", "rebuild", "art", "cancel",
};
static const char *const ctx_name[] = { "np", "song", "group", "otg",
                                        "music" };

static short art_wait_lang(enum artcache_build_wait wait)
{
    switch (wait)
    {
        case ARTCACHE_BUILD_WAIT_DB:       return LANG_IPODUI_WAIT_LIBRARY;
        case ARTCACHE_BUILD_WAIT_PAUSE:    return LANG_IPODUI_ART_PAUSED;
        case ARTCACHE_BUILD_WAIT_PLAYBACK: return LANG_IPODUI_WAIT_PLAYBACK;
        case ARTCACHE_BUILD_WAIT_BATTERY:  return LANG_IPODUI_CHARGE_TO_CONTINUE;
        case ARTCACHE_BUILD_WAIT_OFF:      return LANG_IPODUI_ART_WAITING;
        case ARTCACHE_BUILD_WAIT_RETRY:    return LANG_IPODUI_ART_UPDATE_PENDING;
        default:                           return LANG_IPODUI_UPDATING_ARTWORK;
    }
}

static short art_phase_lang(enum artcache_build_phase phase)
{
    switch (phase)
    {
        case ARTCACHE_BUILD_PHASE_PREPARE:   return LANG_IPODUI_PREPARING_ARTWORK;
        case ARTCACHE_BUILD_PHASE_REQUESTED: return LANG_IPODUI_REQUESTED_ARTWORK;
        case ARTCACHE_BUILD_PHASE_ALBUMS:    return LANG_IPODUI_ALBUM_ARTWORK;
        case ARTCACHE_BUILD_PHASE_PANELS:    return LANG_IPODUI_PANEL_ARTWORK;
        case ARTCACHE_BUILD_PHASE_CHAPTERS:  return LANG_IPODUI_CHAPTER_ARTWORK;
        case ARTCACHE_BUILD_PHASE_BOOKS:     return LANG_IPODUI_BOOK_CACHE;
        case ARTCACHE_BUILD_PHASE_FINALIZE:  return LANG_IPODUI_FINISHING_ARTWORK;
        default:                            return LANG_IPODUI_UPDATING_ARTWORK;
    }
}

/* The header title of the screen below, taken by ui_hold_push() just
 * before the push (that screen outlives the menu). */
static const char *hold_title;

#define BIT(i) (1u << (i))

unsigned ui_hold_items(const struct ui_hold_arg *a)
{
    unsigned m = BIT(UI_HOLD_CANCEL);

    switch (a->ctx)
    {
        case UI_HOLD_NP:
        case UI_HOLD_SONG:
            m |= BIT(UI_HOLD_ADD_OTG) | BIT(UI_HOLD_BROWSE_ALBUM)
                 | BIT(UI_HOLD_BROWSE_ARTIST);
            break;
        case UI_HOLD_GROUP:
            m |= BIT(UI_HOLD_ADD_OTG);
            break;
        case UI_HOLD_IN_OTG:
            m |= BIT(UI_HOLD_REMOVE_OTG) | BIT(UI_HOLD_BROWSE_ALBUM)
                 | BIT(UI_HOLD_BROWSE_ARTIST);
            break;
        case UI_HOLD_MUSIC:
            m |= BIT(UI_HOLD_REBUILD) | BIT(UI_HOLD_BUILD_ART);
            break;
    }
    if (a->spoken || a->tc_idx <= 0)
        m &= ~(BIT(UI_HOLD_BROWSE_ALBUM) | BIT(UI_HOLD_BROWSE_ARTIST));
    return m;
}

int ui_hold_db_gen(void)
{
    return tagcache_ui_count() >= 0 ? tagcache_ui_generation() : -1;
}

/* The track's entry while the database it came from is the one in RAM:
 * fills *flag and returns its tag seeks, else NULL. RAM pointers: valid
 * until a yield. */
static const int32_t *track_seeks(long tc_idx, int db_gen, int32_t *flag)
{
    if (tc_idx <= 0 || db_gen != ui_hold_db_gen())
        return NULL;
    return tagcache_ui_seeks(tc_idx - 1, flag);
}

static void push(const struct ui_hold_arg *a)
{
    struct ui_screen *top = ui_top();

    hold_title = top ? top->title : NULL;
    ui_push(&ui_hold_screen, a);
}

void ui_hold_open_np(long tc_idx, int db_gen)
{
    struct ui_hold_arg a;

    memset(&a, 0, sizeof(a));
    a.ctx = UI_HOLD_NP;
    a.tc_idx = tc_idx;
    a.db_gen = db_gen;
    a.view = -1;
    a.idx = -1;
    push(&a);
}

void ui_hold_open_music(void)
{
    struct ui_hold_arg a;

    memset(&a, 0, sizeof(a));
    a.ctx = UI_HOLD_MUSIC;
    a.view = -1;
    a.idx = -1;
    push(&a);
}

void ui_hold_push(const struct ui_hold_arg *a)
{
    push(a);
}

static const char *hold_label(int row, void *data, char *buf, size_t len)
{
    const struct hold *h = data;

    (void)buf; (void)len;
    if (row < 0 || row >= h->list.count)
        return "";
    return str(item_lang[h->item[row]]);
}

static int hold_enter(struct ui_screen *s, const void *arg)
{
    const struct ui_theme *t = ui_theme;
    struct hold *h = ui_screen_alloc(s, sizeof(struct hold));
    struct ui_hold_arg a = *(const struct ui_hold_arg *)arg;
    struct viewport vp;
    const int32_t *seeks;
    int32_t flag = 0;
    unsigned m;
    int i, n = 0;

    if (!h)
        return -1;
    /* the track's own class decides too: a book or an episode is browsed
     * through Audiobooks or its show (Rev 1 L-6); a gone entry has
     * nothing to browse */
    seeks = track_seeks(a.tc_idx, a.db_gen, &flag);
    if (!seeks)
        a.tc_idx = 0;
    else if (flag & (TAGCACHE_UI_AUDIOBOOK | TAGCACHE_UI_PODCAST
#ifdef HAVE_BCMFW_VIDEO
                     | TAGCACHE_UI_VIDEO       /* the episode items (3.2a) */
#endif
                     ))
        a.spoken = true;
    m = ui_hold_items(&a);
#ifdef HAVE_BCMFW_VIDEO
    /* a video never joins On-The-Go (the audio queue skips it) */
    if (seeks && (flag & TAGCACHE_UI_VIDEO))
        m &= ~BIT(UI_HOLD_ADD_OTG);
#endif
    for (i = 0; i < UI_HOLD_ITEMS; i++)
        if (m & BIT(i))
            h->item[n++] = i;
    h->tc_idx = a.tc_idx;
    h->db_gen = a.db_gen;
    h->view = a.view;
    h->idx = a.idx;
    h->otg_pos = a.otg_pos;
    h->ctx = a.ctx;
    h->swallow = true;
    s->title = hold_title ? hold_title : "";
    hold_title = NULL;

    memset(&vp, 0, sizeof(vp));
    vp.y = t->header_h;
    vp.width = LCD_WIDTH;
    vp.height = LCD_HEIGHT - t->header_h;
    ui_list_init(&h->list, &vp, n, hold_label, h);
    UPDATE_TRACE("hold open ctx=%s items=%u", ctx_name[a.ctx], m);
    (void)ctx_name; (void)item_name;    /* used by the traces only */
    return 0;
}

static void hold_leave(struct ui_screen *s)
{
    ui_screen_free(s);
}

/* The maintenance page's two lines: the step and its count. Returns a hash
 * of both, so the tick redraws only when the text changes. */
static uint32_t maint_text(const struct hold *h, const char **step,
                           char *count, size_t len)
{
    struct tagcache_stat *tc = tagcache_get_stat();
    struct artcache_build_status art;
    bool lib = !h->art_only
               && (tagcache_maintenance_pending() || tagcache_ui_busy()
                   || tagcache_scan_active());
    uint32_t sig;
    const char *c;

    artcache_build_status(&art);
    if (lib)
        *step = str(LANG_IPODUI_UPDATING_LIBRARY);
    else if (art.wait == ARTCACHE_BUILD_READY)
        *step = str(art_phase_lang(art.phase));
    else
        *step = str(art_wait_lang(art.wait));
    if (lib)
    {
        if (tc->progress >= 0)
            snprintf(count, len, "%d%%", tc->progress);
        else
            count[0] = '\0';
    }
    else if (art.phase == ARTCACHE_BUILD_PHASE_ALBUMS && art.total > 0)
        snprintf(count, len, "%d / %d", art.done, art.total);
    else if (art.phase_total > 0)
        snprintf(count, len, "%d / %d", art.phase_done, art.phase_total);
    else
        count[0] = '\0';
    sig = (uint32_t)(uintptr_t)*step * 31u + 1;
    for (c = count; *c; c++)
        sig = sig * 31u + (unsigned char)*c;
    return sig;
}

/* the band the two lines occupy, in screen coordinates */
static void maint_band(int *y, int *hgt)
{
    const struct ui_theme *t = ui_theme;
    int fh = font_get(t->font_ui)->height;
    int body = LCD_HEIGHT - t->header_h;

    *y = t->header_h + body / 2 - fh - 5 - 2;
    *hgt = 2 * fh + 10 + 4;
}

static void hold_draw(struct ui_screen *s, unsigned dirty)
{
    struct hold *h = s->priv;
    int y[UI_BANDS_MAX], hh[UI_BANDS_MAX], i, n;

    if (h->maintenance)
    {
        const struct ui_theme *t = ui_theme;
        const char *step;
        struct viewport vp;
        char count[20];
        int w, fh, y0, by, bh;

        if (!(dirty & (UI_DIRTY_BODY | UI_DIRTY_BAND)))
            return;
        h->shown = maint_text(h, &step, count, sizeof(count));
        memset(&vp, 0, sizeof(vp));
        vp.y = t->header_h;
        vp.width = LCD_WIDTH;
        vp.height = LCD_HEIGHT - t->header_h;
        vp.font = t->font_ui;
        vp.drawmode = DRMODE_FG;
        lcd_set_viewport(&vp);
        lcd_set_foreground(t->list_bg);
        if (dirty & UI_DIRTY_BODY)
            lcd_fillrect(0, 0, vp.width, vp.height);
        else
        {   /* only the text's band (hold_tick) */
            maint_band(&by, &bh);
            lcd_fillrect(0, by - vp.y, vp.width, bh);
        }
        lcd_set_foreground(t->list_fg);
        font_getstringsize((const unsigned char *)step, &w, &fh, vp.font);
        y0 = vp.height / 2 - fh - 5;
        lcd_putsxy((vp.width - w) / 2, y0, (const unsigned char *)step);
        if (count[0])
        {
            font_getstringsize((const unsigned char *)count, &w, NULL,
                               vp.font);
            lcd_putsxy((vp.width - w) / 2, y0 + fh + 10,
                       (const unsigned char *)count);
        }
        lcd_set_viewport(NULL);
        return;
    }

    if (dirty & UI_DIRTY_BODY)
        ui_list_draw(&h->list, h->list.vp.y, h->list.vp.height);
    else if (dirty & UI_DIRTY_BAND)
    {
        n = ui_dirty_bands(y, hh);
        for (i = 0; i < n; i++)
            ui_list_draw(&h->list, y[i], hh[i]);
    }
}

static int hold_tick(struct ui_screen *s)
{
    struct hold *h = s->priv;

    if (h->maintenance)
    {
        if ((!h->art_only && !tagcache_maintenance_pending()
             && !tagcache_ui_busy() && !artcache_maintenance_pending())
            || (h->art_only && !artcache_maintenance_pending()))
            ui_pop_request(s);
        else
        {   /* redraw the two lines' band, and only when they change */
            const char *step;
            char count[20];
            int by, bh;

            if (maint_text(h, &step, count, sizeof(count)) != h->shown)
            {
                maint_band(&by, &bh);
                ui_invalidate_band(by, bh);
            }
        }
        /* The PP5022 LCD DMA shares the memory bus with the filesystem.
         * One status refresh per second keeps this deliberate full-speed
         * job observable without turning its progress page into traffic. */
        return HZ;
    }
    return ui_list_tick(&h->list);
}

/* The album's or the artist's name for the pushed list's header. */
static void browse_title(const struct hold *h, bool artist, char *buf,
                         size_t len)
{
    int32_t flag;
    const int32_t *seeks = track_seeks(h->tc_idx, h->db_gen, &flag);
    const char *v = NULL;

    if (seeks && artist)
    {
        v = tagcache_ui_str(tag_albumartist, seeks[tag_albumartist]);
        if (!v || !*v)
            v = tagcache_ui_str(tag_artist, seeks[tag_artist]);
    }
    else if (seeks)
        v = tagcache_ui_str(tag_album, seeks[tag_album]);
    if (!v || !*v)
        v = str(artist ? LANG_IPODUI_UNKNOWN_ARTIST
                       : LANG_IPODUI_UNKNOWN_ALBUM);
    strlcpy(buf, v, len);
}

/* Browse Album / Browse Artist: this menu (and Now Playing under it, ⚠️
 * H2) leave, then the list is pushed. Everything needed is copied out
 * first: the menu's state goes with its pop. */
static void browse(struct hold *h, bool artist)
{
    struct ui_browse_arg b;
    char title[HOLD_TITLE_LEN];
    bool from_np = h->ctx == UI_HOLD_NP;
    int id = h->tc_idx - 1;
    struct ui_screen *top;

    browse_title(h, artist, title, sizeof(title));
    memset(&b, 0, sizeof(b));
    b.title = title;
    b.f.artist = b.f.album = b.f.genre = b.f.composer = LIB_ANY;
    if (artist)
    {
        b.cat = LIB_ALBUMS;
        b.f.artist = id;
        b.all = UI_ALL_SONGS;
    }
    else
    {
        b.cat = LIB_SONGS;
        b.f.album = id;
    }
    ui_pop();                       /* the menu: h is gone from here on */
    top = ui_top();
    if (from_np && top && top->ops == &ui_nowplaying_screen)
        ui_pop();
    ui_push(&ui_browse_screen, &b);
}

static enum ui_result hold_act(struct hold *h)
{
    enum ui_hold_item it = h->item[h->list.selected];
    int rc = 0;

    switch (it)
    {
        case UI_HOLD_ADD_OTG:
            /* never touches what plays (⚠️ H3) */
            if (h->ctx == UI_HOLD_NP)
                rc = h->tc_idx > 0 ? otg_add_track(h->tc_idx) : -1;
            else
                rc = otg_add_row(h->view, h->idx);
            ui_otg_changed();
            break;
        case UI_HOLD_REMOVE_OTG:
            rc = otg_remove(h->otg_pos);
            ui_otg_changed();
            break;
        case UI_HOLD_BROWSE_ALBUM:
        case UI_HOLD_BROWSE_ARTIST:
            UPDATE_TRACE("hold act %s rc=0", item_name[it]);
            browse(h, it == UI_HOLD_BROWSE_ARTIST);
            return UI_HANDLED;
        case UI_HOLD_REBUILD:
            /* The tag database finishes first; artcache waits for its
             * fresh RAM generation, then continues at maintenance speed.
             * This is an explicitly dedicated job, not background work:
             * release a paused session's codec/buffer and its I/O before
             * either cache worker starts. */
            if (audio_status())
                audio_stop();
            tagcache_rebuild_maintenance();
            artcache_build_maintenance_request();
            h->maintenance = true;
            ui_invalidate(UI_DIRTY_BODY);
            return UI_HANDLED;
        case UI_HOLD_BUILD_ART:
            /* Checks every album against its source and fills missing or
             * stale art, without changing the library database. As above,
             * it owns the device until the pass is done. */
            if (audio_status())
                audio_stop();
            artcache_build_maintenance_request();
            h->maintenance = true;
            h->art_only = true;
            ui_invalidate(UI_DIRTY_BODY);
            return UI_HANDLED;
        default:
            break;
    }
    UPDATE_TRACE("hold act %s rc=%d", item_name[it], rc);
    (void)rc;                       /* the trace compiles out on target */
    return UI_POP;
}

static enum ui_result hold_input(struct ui_screen *s,
                                 const struct ui_input *in)
{
    struct hold *h = s->priv;
    int y[2], hh[2], i, n;

    if (h->maintenance)
    {
        struct artcache_build_status art;

        /* "Charge to continue" has no end the user can wait out: MENU
         * leaves the page, and the job goes on by itself once charging */
        artcache_build_status(&art);
        if (in->key == UI_KEY_MENU
            && art.wait == ARTCACHE_BUILD_WAIT_BATTERY)
            return UI_IGNORED;
        return UI_HANDLED;           /* completion or global USB owns it */
    }
    switch (in->key)
    {
        case UI_KEY_WHEEL:
            n = ui_list_move(&h->list, in->wheel_steps, y, hh);
            for (i = 0; i < n; i++)
                ui_invalidate_band(y[i], hh[i]);
            return UI_HANDLED;

        case UI_KEY_SELECT:
            if (!in->release)
                return UI_HANDLED;      /* the opening hold's repeats */
            if (h->swallow)
            {
                h->swallow = false;     /* the opening press's release */
                return UI_HANDLED;
            }
            if (in->long_press || h->list.count <= 0)
                return UI_HANDLED;
            return hold_act(h);

        default:
            return UI_IGNORED;          /* MENU pops: Cancel */
    }
}

const struct ui_screen_ops ui_hold_screen = {
    .name    = "hold",
    .enter   = hold_enter,
    .leave   = hold_leave,
    .draw    = hold_draw,
    .input   = hold_input,
    .timeout = NULL,
    .tick    = hold_tick,
};
