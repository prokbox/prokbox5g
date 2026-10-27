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
/* i18n: later */
#include "config.h"
#include <string.h>
#include "system.h"
#include "kernel.h"
#include "lcd.h"
#include "font.h"
#include "backlight.h"
#include "core_alloc.h"
#include "lang.h"
#include "string-extra.h"
#include "logf.h"
#include "ipodui/ipodui.h"
#include "ipodui/theme.h"
#include "ipodui/list.h"
#include "ipodui/menu.h"
#include "ipodui/paint.h"
#include "ipodui/anim.h"
#include "ipodui/library.h"
#include "ipodui/screens/screens.h"
#include "ipodui/screens/panels.h"
#include "ipodui/screens/holdmenu.h"
#include "ipodui/preview.h"
#include "ipodui/menucfg.h"
#include "ipodui/extras.h"
#ifdef HAVE_BCMFW_VIDEO
#include "ipodui/screens/video.h"
#endif

#define MENU_MAX_ITEMS 40

/* Lives in a core_alloc block; may move between calls, so it is always
 * fetched through state(). */
struct menu_state {
    struct ui_list list;
    const struct ui_menu *menu;
    unsigned char map[MENU_MAX_ITEMS];  /* visible row -> item index */
};

/* The root (main) menu is never popped, so its state is static: pushing
 * it cannot fail for lack of memory. The menus under it (Music, Settings:
 * one at a time on the stack) take a static slot too: a core_alloc here
 * while audio is paused or playing makes playback shrink its buffer, and
 * a shrink stops a paused queue for good (playback.c shrink_callback()
 * resumes only a playing one, after a gap). That stopped a paused 1-file
 * queue when Music was opened from the main menu (P6 fix round, item 4).
 * Only a third nested menu would fall back to core_alloc. */
#define MENU_SLOTS 2
static struct menu_state root_state;
static struct menu_state menu_slot[MENU_SLOTS];
static bool menu_slot_used[MENU_SLOTS];

/* -- menu content ------------------------------------------------------- */

#define ANY LIB_FILTER_ANY

static struct ui_browse_arg artists_arg = {
    NULL, LIB_ARTISTS, ANY, UI_ALL_NONE, false };
static struct ui_browse_arg albums_arg = {
    NULL, LIB_ALBUMS, ANY, UI_ALL_NONE, false };
static struct ui_browse_arg compilations_arg = {
    NULL, LIB_COMPILATIONS, ANY, UI_ALL_NONE, false };
static struct ui_browse_arg songs_arg = {
    NULL, LIB_SONGS, ANY, UI_ALL_NONE, false };
static struct ui_browse_arg genres_arg = {
    NULL, LIB_GENRES, ANY, UI_ALL_NONE, false };
static struct ui_browse_arg composers_arg = {
    NULL, LIB_COMPOSERS, ANY, UI_ALL_NONE, false };

/* 7G order (docs/design/7g-spec.md). Cover Flow is hidden until Phase 7
 * builds it (owner, #38 end review): the 7G lists it first;
 * IPODUI_COVERFLOW puts it back. Every Music item shows the drifting art
 * (#38; one key, so moving between them never interrupts the drift).
 *
 * Phase 11: each row is shown when Settings > Music Menu (or Main Menu,
 * below) has it checked: visible() asks menucfg.c, which is a mask test
 * on the saved setting. */
#define M_STUB { NULL, &ui_stub_screen, NULL, NULL, NULL, \
                      &ui_preview_drift, NULL }
#define VIS_MU(name, bit) \
    static bool mu_vis_##name(void) { return ui_mu_visible(bit); }
#define VIS_MM(name, bit) \
    static bool mm_vis_##name(void) { return ui_mm_visible(bit); }
VIS_MU(playlists, UI_MU_PLAYLISTS)
VIS_MU(artists, UI_MU_ARTISTS)
VIS_MU(albums, UI_MU_ALBUMS)
VIS_MU(compilations, UI_MU_COMPILATIONS)
VIS_MU(songs, UI_MU_SONGS)
VIS_MU(genres, UI_MU_GENRES)
VIS_MU(composers, UI_MU_COMPOSERS)
VIS_MU(audiobooks, UI_MU_AUDIOBOOKS)
VIS_MU(search, UI_MU_SEARCH)
VIS_MM(music, UI_MM_MUSIC)
VIS_MM(playlists, UI_MM_PLAYLISTS)
VIS_MM(artists, UI_MM_ARTISTS)
VIS_MM(albums, UI_MM_ALBUMS)
VIS_MM(compilations, UI_MM_COMPILATIONS)
VIS_MM(songs, UI_MM_SONGS)
VIS_MM(genres, UI_MM_GENRES)
VIS_MM(composers, UI_MM_COMPOSERS)
VIS_MM(audiobooks, UI_MM_AUDIOBOOKS)
VIS_MM(search, UI_MM_SEARCH)
#ifdef HAVE_BCMFW_VIDEO
VIS_MM(videos, UI_MM_VIDEOS)
#endif
VIS_MM(podcasts, UI_MM_PODCASTS)
VIS_MM(extras, UI_MM_EXTRAS)
/* P12: the Extras the Main Menu checklist can add, each in its fixed slot
 * after Extras (photo 1189: Alarms right under Extras), in the bits' order;
 * shown when ticked and the Extra is offered on this unit */
#define VIS_MX(name, bit, x) static bool mx_vis_##name(void) \
    { return ui_mm_visible(bit) && extras_available(x); }
VIS_MX(alarms, UI_MM_ALARMS, XT_ALARMS)
VIS_MX(calendars, UI_MM_CALENDARS, XT_CALENDARS)
VIS_MX(clocks, UI_MM_CLOCKS, XT_CLOCKS)
VIS_MX(contacts, UI_MM_CONTACTS, XT_CONTACTS)
VIS_MX(games, UI_MM_GAMES, XT_GAMES)
VIS_MX(notes, UI_MM_NOTES, XT_NOTES)
VIS_MX(screenlock, UI_MM_SCREENLOCK, XT_SCREENLOCK)
VIS_MX(stopwatch, UI_MM_STOPWATCH, XT_STOPWATCH)
#define M_EXTRA(name) { NULL, NULL, NULL, mx_vis_##name, extras_go_##name, \
                        NULL, NULL }
VIS_MM(settings, UI_MM_SETTINGS)
VIS_MM(shuffle, UI_MM_SHUFFLE)

#define ITEM(target, arg, vis, act) \
    { NULL, target, arg, vis, act, &ui_preview_drift, NULL }
#define M_BROWSE(arg, vis) ITEM(&ui_browse_screen, &arg, vis, NULL)
/* #58 (owner): the Audiobooks rows drift over the book covers */
#define M_BOOKS(vis) { NULL, &ui_browse_screen, &ui_audiobooks_arg, vis, \
                       NULL, &ui_preview_drift_books, NULL }

/* Labels and titles are language-system strings, filled in by
 * labels_init() (menu_enter) from the *_lang[] id tables below. */
static struct ui_menu_item music_items[] = {
#ifdef IPODUI_COVERFLOW
    M_STUB,
#endif
    ITEM(&ui_playlists_screen, NULL, mu_vis_playlists, NULL),
    M_BROWSE(artists_arg, mu_vis_artists),
    M_BROWSE(albums_arg, mu_vis_albums),
    M_BROWSE(compilations_arg, mu_vis_compilations),
    M_BROWSE(songs_arg, mu_vis_songs),
    M_BROWSE(genres_arg, mu_vis_genres),
    M_BROWSE(composers_arg, mu_vis_composers),
    M_BOOKS(mu_vis_audiobooks),
    ITEM(&ui_search_screen, NULL, mu_vis_search, NULL),
};

static struct ui_menu music_menu = {
    NULL, music_items, ARRAYLEN(music_items), true,  /* split, like main */
    true, NULL,                                         /* library footer */
};

/* The Settings tree is agent B's (screens/settings.c, #38): ui_settings_menu
 * in screens/panels.h. */

/* The Main Menu rows in the 7G's fixed slots (main-menu-custom-hold.jpg:
 * a checked item takes its own place, not the end of the list): the
 * canonical order of menucfg.h's bits. Photos is hidden (its bit is reserved);
 * Videos shows in a video build (Phase 14a). Settings is never
 * hidden. Now Playing is not a checklist item: it shows while there is a
 * queue. */
static struct ui_menu_item main_items[] = {
    ITEM(&ui_menu_screen, &music_menu, mm_vis_music, NULL),
    ITEM(&ui_playlists_screen, NULL, mm_vis_playlists, NULL),
    M_BROWSE(artists_arg, mm_vis_artists),
    M_BROWSE(albums_arg, mm_vis_albums),
    M_BROWSE(compilations_arg, mm_vis_compilations),
    M_BROWSE(songs_arg, mm_vis_songs),
    M_BROWSE(genres_arg, mm_vis_genres),
    M_BROWSE(composers_arg, mm_vis_composers),
    M_BOOKS(mm_vis_audiobooks),
    ITEM(&ui_search_screen, NULL, mm_vis_search, NULL),
#ifdef HAVE_BCMFW_VIDEO
    /* Phase 14a (T1): Videos sits after Search, before Podcasts (the
     * bit order, 7G: Music, Videos, Photos, Podcasts). The panel is the
     * posters. */
    { NULL, &ui_videos_screen, NULL, mm_vis_videos, NULL,
      &ui_preview_videos, NULL },
#endif
    { NULL, &ui_browse_screen, &ui_podcasts_arg, mm_vis_podcasts, NULL,
      &ui_preview_podcasts, NULL },
    { NULL, &ui_menu_screen, &ui_extras_menu, mm_vis_extras, NULL,
      &ui_preview_extras, NULL },
    M_EXTRA(alarms), M_EXTRA(calendars), M_EXTRA(clocks), M_EXTRA(contacts),
    M_EXTRA(games), M_EXTRA(notes), M_EXTRA(screenlock), M_EXTRA(stopwatch),
    { NULL, &ui_menu_screen, &ui_settings_menu, mm_vis_settings, NULL,
      &ui_preview_capacity, NULL },
    /* art panels (spec main-menu.md: "Music and the other art panels") */
    ITEM(NULL, NULL, mm_vis_shuffle, ui_shuffle_songs),
    /* owner 2026-10-20: the playing cover, still */
    { NULL, NULL, NULL, ui_now_playing_visible, ui_show_wps,
      &ui_preview_nowplaying, NULL },
};

static char main_title[24];             /* filled by labels_init() */
const struct ui_menu ui_main_menu = {
    main_title, main_items, ARRAYLEN(main_items), true, false, NULL
};


#ifdef IPODUI_COVERFLOW
#define CF_ID LANG_IPODUI_COVER_FLOW,
#else
#define CF_ID
#endif
static const short music_lang[] = {
    CF_ID LANG_IPODUI_PLAYLISTS, LANG_IPODUI_ARTISTS, LANG_IPODUI_ALBUMS,
    LANG_IPODUI_COMPILATIONS, LANG_IPODUI_SONGS, LANG_IPODUI_GENRES,
    LANG_IPODUI_COMPOSERS, LANG_IPODUI_AUDIOBOOKS, LANG_IPODUI_SEARCH,
};
static const short main_lang[] = {
    LANG_IPODUI_MUSIC, LANG_IPODUI_PLAYLISTS, LANG_IPODUI_ARTISTS,
    LANG_IPODUI_ALBUMS, LANG_IPODUI_COMPILATIONS, LANG_IPODUI_SONGS,
    LANG_IPODUI_GENRES, LANG_IPODUI_COMPOSERS, LANG_IPODUI_AUDIOBOOKS,
    LANG_IPODUI_SEARCH,
#ifdef HAVE_BCMFW_VIDEO
    LANG_IPODUI_VIDEOS,
#endif
    LANG_IPODUI_PODCASTS, LANG_IPODUI_EXTRAS,
    LANG_IPODUI_X_ALARMS, LANG_IPODUI_X_CALENDARS, LANG_IPODUI_X_CLOCKS,
    LANG_IPODUI_X_CONTACTS, LANG_IPODUI_X_GAMES, LANG_IPODUI_X_NOTES,
    LANG_IPODUI_X_SCREEN_LOCK, LANG_IPODUI_X_STOPWATCH,
    LANG_SETTINGS, LANG_IPODUI_SHUFFLE_SONGS, LANG_IPODUI_NOW_PLAYING,
};
_Static_assert(ARRAYLEN(main_lang) == ARRAYLEN(main_items), "main_lang");

static void labels_init(void)
{
    unsigned i;

    for (i = 0; i < ARRAYLEN(music_items); i++)
        music_items[i].label = str(music_lang[i]);
    for (i = 0; i < ARRAYLEN(main_items); i++)
    {
        int x = extras_of_action(main_items[i].action);

        main_items[i].label = str(main_lang[i]);
        if (x >= 0)                     /* the Extra's own row art */
            main_items[i].preview = extras_panel(x);
    }
    music_menu.title = str(LANG_IPODUI_MUSIC);
    strlcpy(main_title, str(LANG_IPODUI_DEVICE_NAME), sizeof(main_title));
    artists_arg.title = str(LANG_IPODUI_ARTISTS);
    albums_arg.title = str(LANG_IPODUI_ALBUMS);
    compilations_arg.title = str(LANG_IPODUI_COMPILATIONS);
    songs_arg.title = str(LANG_IPODUI_SONGS);
    genres_arg.title = str(LANG_IPODUI_GENRES);
    composers_arg.title = str(LANG_IPODUI_COMPOSERS);
    ui_audiobooks_arg.title = str(LANG_IPODUI_AUDIOBOOKS);
    ui_podcasts_arg.title = str(LANG_IPODUI_PODCASTS);
}

/* -- menu screen -------------------------------------------------------- */

static struct menu_state *state(struct ui_screen *s)
{
    struct menu_state *m = s->handle > 0 ? core_get_data(s->handle) : s->priv;
    m->list.data = m;
    return m;
}

static const char *menu_label(int idx, void *data, char *buf, size_t len)
{
    struct menu_state *m = data;
    (void)buf; (void)len;
    return m->menu->items[m->map[idx]].label;
}

static bool menu_chevron(int idx, void *data)
{
    struct menu_state *m = data;
    const struct ui_menu_item *it = &m->menu->items[m->map[idx]];
#ifdef HAVE_BCMFW_VIDEO
    /* Videos > Settings opens a page (V6) through an action */
    if (it->action == ui_setvideos_push)
        return true;
#endif
    /* P12: an Extra's row opens its screens through an action */
    return it->target != NULL || extras_of_action(it->action) >= 0;
}

/* Rebuilds the visible-row map; returns the visible count. */
static int build_map(struct menu_state *m)
{
    const struct ui_menu *menu = m->menu;
    int i, n = 0;

    for (i = 0; i < menu->count && n < MENU_MAX_ITEMS; i++)
    {
        const struct ui_menu_item *it = &menu->items[i];
        if (!it->visible || it->visible())
            m->map[n++] = i;
    }
    return n;
}

/* The number of visible rows, leaving the map alone. */
static int count_visible(const struct ui_menu *menu)
{
    int i, n = 0;

    for (i = 0; i < menu->count && n < MENU_MAX_ITEMS; i++)
    {
        const struct ui_menu_item *it = &menu->items[i];
        if (!it->visible || it->visible())
            n++;
    }
    return n;
}

/* The Music menu's overlay hook (list.h): the library footer */
static void menu_overlay(void *data, int y, int h)
{
    struct menu_state *m = data;
    ui_libfoot_overlay(&m->list, y, h);
}

static int menu_enter(struct ui_screen *s, const void *arg)
{
    const struct ui_menu *menu = arg;
    const struct ui_theme *t = ui_theme;
    struct menu_state *m;
    struct viewport vp;

    labels_init();
    if (menu == &ui_main_menu)
    {
        s->priv = &root_state;
    }
    else
    {
        int i;

        for (i = 0; i < MENU_SLOTS && menu_slot_used[i]; i++)
            ;
        if (i < MENU_SLOTS)
        {
            menu_slot_used[i] = true;
            s->priv = &menu_slot[i];
        }
        else
        {
            int handle = core_alloc(sizeof(struct menu_state));
            if (handle <= 0)
            {
                logf("ipodui: menu %s: no memory", menu->title);
                return -1;
            }
            s->handle = handle;
        }
    }
    if (menu->refresh)
        menu->refresh();
    s->title = menu->title;
    /* split screen: the header covers only the list, the art panel runs
     * the full height on the right */
    s->header_w = menu->split ? t->split_x : 0;
    m = state(s);
    memset(m, 0, sizeof(*m));           /* a core_alloc block is not zeroed */
    m->menu = menu;

    memset(&vp, 0, sizeof(vp));
    vp.x = 0;
    vp.y = t->header_h;
    vp.width = menu->split ? t->split_x : LCD_WIDTH;
    vp.height = LCD_HEIGHT - t->header_h;

    ui_list_init(&m->list, &vp, build_map(m), menu_label, m);
    m->list.has_chevron = menu_chevron;
    if (menu->lib_footer)
        m->list.overlay = menu_overlay;
    return 0;
}

static void menu_leave(struct ui_screen *s)
{
    int i;

    if (s->handle > 0)
        s->handle = core_free(s->handle);
    for (i = 0; i < MENU_SLOTS; i++)
        if (s->priv == &menu_slot[i])
            menu_slot_used[i] = false;
}

/* -- "Updating Library..." footer (menu.h) ------------------------------ */

/* Layout and colours are ours (~): the 7G never builds its library on the
 * device, so there is no stock capture to match. The band is the list
 * background darkened ~35 % (a slight top-to-bottom gradient around it,
 * like the header's), a 1 px lighter line on top, dark text as in the
 * header, and the activity glyph left of the text, the pair centred. */
#define FOOT_FADE_TICKS (HZ/5)          /* ~200 ms each way */
#define FOOT_SPIN_TICKS (HZ/8)          /* ~8 fps, unboosted, like the marquee */
#define FOOT_SPIN_R     5               /* glyph radius (~) */
#define FOOT_GAP        6               /* glyph to text (~) */
#define FOOT_DARK       166             /* /256 of the list bg: ~35 % darker */
#define FOOT_GRAD       8               /* +- around it, top to bottom */
#define FOOT_HI         224             /* /256: the top line */
#define FOOT_TEXT       LCD_RGBPACK(0x1e, 0x1e, 0x1e)
#define FOOT_DOT        LCD_RGBPACK(0x1e, 0x1e, 0x1e)

/* the glyph's 8 dots, clockwise from the top (the old strip's spinner) */
static const signed char spin_pos[8][2] = {
    { 0, -4 }, { 3, -3 }, { 4, 0 }, { 3, 3 },
    { 0, 4 }, { -3, 3 }, { -4, 0 }, { -3, -3 },
};

/* One footer for the shell (only the top screen ticks): ~236 B */
static struct {
    fb_data rows[UI_LIBFOOT_STEPS][UI_LIBFOOT_H];  /* per level, per row */
    long t0;                    /* tick the current fade began */
    long spin_next;             /* tick of the next glyph step */
    short tw;                   /* text width, 0 = not measured yet */
    unsigned char level;        /* 0 (off) .. UI_LIBFOOT_STEPS (shown) */
    unsigned char from;         /* the level the current fade began at */
    unsigned char spin;         /* glyph phase */
    bool want;                  /* fading in (or shown) */
    bool built;                 /* rows[] filled */
} foot;

static unsigned foot_scale(unsigned c, int k)
{
    return LCD_RGBPACK(RGB_UNPACK_RED(c) * k >> 8,
                       RGB_UNPACK_GREEN(c) * k >> 8,
                       RGB_UNPACK_BLUE(c) * k >> 8);
}

/* The per-row colours of every fade level, once (lean rule 2): a draw is
 * one hline per row, no blend per pixel. */
static void foot_build(void)
{
    unsigned bg = ui_theme->list_bg, full;
    int r, lv, k;

    for (r = 0; r < UI_LIBFOOT_H; r++)
    {
        k = r == 0 ? FOOT_HI
                   : FOOT_DARK + FOOT_GRAD
                     - 2 * FOOT_GRAD * (r - 1) / (UI_LIBFOOT_H - 2);
        full = foot_scale(bg, k);
        for (lv = 1; lv <= UI_LIBFOOT_STEPS; lv++)
            foot.rows[lv - 1][r] = ui_paint_blend(bg, full,
                                                  lv * 15 / UI_LIBFOOT_STEPS);
    }
    foot.built = true;
}

static int foot_y(const struct ui_list *l)
{
    return l->vp.y + l->vp.height - UI_LIBFOOT_H;
}

/* The footer's part of the LCD band [y, y + h), at the current level,
 * into the framebuffer (no push). */
static void foot_draw(const struct ui_list *l, int y, int h)
{
    const struct ui_theme *t = ui_theme;
    const char *text = str(LANG_IPODUI_UPDATING_LIBRARY);
    const fb_data *col;
    struct viewport vp;
    unsigned body;
    int fy = foot_y(l), y0, y1, i, a, x, cy, fh, tw;

    y0 = MAX(y, fy);
    y1 = MIN(y + h, fy + UI_LIBFOOT_H);
    if (!foot.level || y1 <= y0)
        return;
    if (!foot.built)
        foot_build();
    col = foot.rows[foot.level - 1];

    /* a viewport over the visible part: footer row r is at r + fy - y0 */
    memset(&vp, 0, sizeof(vp));
    vp.x = l->vp.x;
    vp.y = y0;
    vp.width = l->vp.width;
    vp.height = y1 - y0;
    vp.font = t->font_small;
    vp.drawmode = DRMODE_SOLID;
    lcd_set_viewport(&vp);
    for (i = y0; i < y1; i++)
    {
        lcd_set_foreground(col[i - fy]);
        lcd_hline(0, vp.width - 1, i - y0);
    }

    a = foot.level * 15 / UI_LIBFOOT_STEPS;     /* the content's weight */
    body = col[UI_LIBFOOT_H / 2];
    if (!foot.tw)
    {
        font_getstringsize(text, &tw, NULL, vp.font);
        foot.tw = tw;
    }
    x = (vp.width - (2 * FOOT_SPIN_R + FOOT_GAP + foot.tw)) / 2;
    if (x < t->text_pad_x)
        x = t->text_pad_x;
    cy = fy + UI_LIBFOOT_H / 2 - y0;
    /* the glyph: the newest dot darkest, the others fading behind it */
    for (i = 0; i < 8; i++)
    {
        int age = (foot.spin - i) & 7;
        lcd_set_foreground(ui_paint_blend(body, FOOT_DOT,
                                          (15 - age * 2) * a / 15));
        lcd_fillrect(x + FOOT_SPIN_R + spin_pos[i][0] - 1,
                     cy + spin_pos[i][1] - 1, 2, 2);
    }
    fh = font_get(vp.font)->height;
    vp.fg_pattern = ui_paint_blend(body, FOOT_TEXT, a);
    vp.drawmode = DRMODE_FG;
    lcd_set_viewport(&vp);
    lcd_putsxy(x + 2 * FOOT_SPIN_R + FOOT_GAP,
               fy + (UI_LIBFOOT_H - fh) / 2 - y0, text);
    lcd_set_viewport(NULL);
}

void ui_libfoot_overlay(const struct ui_list *l, int y, int h)
{
    foot_draw(l, y, h);
}

/* Draws and pushes the footer's own band (not while the list glides: its
 * next frame draws the footer through the overlay hook anyway). */
static void foot_push(const struct ui_list *l)
{
    int fy = foot_y(l);

    if (ui_list_gliding(l))
        return;
    foot_draw(l, fy, UI_LIBFOOT_H);
    ui_lcd_push("libfoot", l->vp.x, fy, l->vp.width, UI_LIBFOOT_H);
}

int ui_libfoot_tick(const struct ui_list *l)
{
    /* only while building: LIB_UNAVAILABLE (RAM cache off or out of
     * memory) never ends, so it shows nothing, like an empty library */
    bool want = lib_state() == LIB_UPDATING;
    int target = want ? UI_LIBFOOT_STEPS : 0;
    long now = current_tick;

    if (want != foot.want)
    {
        foot.want = want;
        foot.from = foot.level;
        foot.t0 = now;
        if (want && !foot.level)
        {
            foot.spin = 0;
            UPDATE_TRACE("lib foot in w=%d", l->vp.width);
        }
    }
    if (foot.level != target)
    {
        /* time based: a late tick lands on the right level (frames are
         * dropped, never queued) */
        int d = (now - foot.t0) * UI_LIBFOOT_STEPS / FOOT_FADE_TICKS;
        int lv = want ? MIN(foot.from + d, UI_LIBFOOT_STEPS)
                      : MAX(foot.from - d, 0);

        if (lv != foot.level)
        {
            foot.level = lv;
            if (lv)
                foot_push(l);
            else
            {
                /* gone: the rows under it come back (the screen's band
                 * draw, pushed by the main loop). Whole rows: a band draw
                 * repaints whole rows but floats the overlays (Search's
                 * letter strip) only over the band it was given. */
                if (!ui_list_gliding(l))
                {
                    int by = foot_y(l), bh = UI_LIBFOOT_H;
                    ui_list_rows_band(l, &by, &bh);
                    ui_invalidate_band(by, bh);
                }
                UPDATE_TRACE("lib foot out w=%d", l->vp.width);
            }
        }
        if (lv != target)
        {
            long next = foot.t0 + ((d + 1) * FOOT_FADE_TICKS
                                   + UI_LIBFOOT_STEPS - 1) / UI_LIBFOOT_STEPS;
            return MAX(next - now, 1);
        }
        foot.spin_next = now + FOOT_SPIN_TICKS;
    }
    if (!foot.level || !is_backlight_on(true))
        return TIMEOUT_BLOCK;   /* nobody sees it spin; a key wakes us */
    if (!TIME_BEFORE(now, foot.spin_next))
    {
        foot.spin++;
        foot.spin_next = now + FOOT_SPIN_TICKS;
        foot_push(l);
    }
    return foot.spin_next - now;
}

/* Split menus: the highlighted item's panel (preview.h). Every move and
 * every full draw (push, pop, return) restarts the rest delay. */
static void select_panel(struct menu_state *m)
{
    const struct ui_menu_item *it;

    if (!m->menu->split || m->list.count <= 0)
        return;
    it = &m->menu->items[m->map[m->list.selected]];
    ui_panel_select(it->preview, it->preview_arg);
}

static void menu_draw(struct ui_screen *s, unsigned dirty)
{
    struct menu_state *m = state(s);
    int y[UI_BANDS_MAX], h[UI_BANDS_MAX], item;

    if (dirty & UI_DIRTY_BODY)
    {
        /* visibility (Now Playing) can change while we are covered */
        if (m->menu->refresh)
            m->menu->refresh();
        /* rows can appear or go while this menu is covered (Settings >
         * Main Menu): the same item stays selected if it is still there */
        item = m->list.count > 0 ? m->map[m->list.selected] : -1;
        ui_list_set_count(&m->list, build_map(m));
        if (item >= 0)
        {
            int i, d = 0;

            for (i = 0; i < m->list.count; i++)
                if (m->map[i] == item)
                    d = i - m->list.selected;
            if (d)
                ui_list_move(&m->list, d, y, h);    /* full draw follows */
        }
        ui_list_draw(&m->list, m->list.vp.y, m->list.vp.height);
        if (m->menu->split)
        {
            /* the panel in full: what is on it now, or the highlighted
             * item's after a full-screen screen (preview.c) */
            select_panel(m);
            ui_panel_draw_now();
        }
    }
    else if (dirty & UI_DIRTY_BAND)
    {
        int i, n = ui_dirty_bands(y, h);
        for (i = 0; i < n; i++)
            ui_list_draw(&m->list, y[i], h[i]);
    }
}

static int menu_tick(struct ui_screen *s)
{
    struct menu_state *m = state(s);
    int wait = ui_list_tick(&m->list), st;

    /* an item's visibility can change while this menu is on top (the
     * queue ended: "Now Playing" goes away); redraw when the count moves */
    if (count_visible(m->menu) != m->list.count)
        ui_invalidate(UI_DIRTY_BODY);
    if (!m->menu->lib_footer)
        return wait;
    st = ui_libfoot_tick(&m->list);
    if (wait == UI_TICK_ANIM || st == TIMEOUT_BLOCK)
        return wait;
    return wait == TIMEOUT_BLOCK ? st : MIN(wait, st);
}

static enum ui_result menu_input(struct ui_screen *s,
                                 const struct ui_input *in)
{
    struct menu_state *m = state(s);
    const struct ui_menu_item *it;
    int y[2], h[2], i, n;

    switch (in->key)
    {
        case UI_KEY_WHEEL:
            i = m->list.selected;
            n = ui_list_move(&m->list, in->wheel_steps, y, h);
            if (m->list.selected != i)
            {
                select_panel(m);    /* the old panel stays meanwhile */
                /* #106 intent preload: a module screen starts loading on
                 * highlight (only while the disk is awake) */
                it = &m->menu->items[m->map[m->list.selected]];
                if (it->target)
                    mod_preload(it->target);
                else
                    extras_preload(extras_of_action(it->action));
            }
            for (i = 0; i < n; i++)
                ui_invalidate_band(y[i], h[i]);
            return UI_HANDLED;

        case UI_KEY_SELECT:
            if (in->long_press && !in->release && m->list.count > 0)
            {
                it = &m->menu->items[m->map[m->list.selected]];
                if (m->menu == &ui_main_menu
                    && it->target == &ui_menu_screen
                    && it->arg == &music_menu)
                {
                    ui_hold_open_music();
                    return UI_HANDLED;
                }
            }
            if (!in->release || in->long_press || m->list.count <= 0)
                return UI_HANDLED;
            it = &m->menu->items[m->map[m->list.selected]];
            /* m may move from here on (allocations) */
            if (it->target)
                ui_push(it->target, it->arg ? it->arg : it->label);
            else if (it->action)
                it->action();
            return UI_HANDLED;

        default:
            return UI_IGNORED;
    }
}

const struct ui_screen_ops ui_menu_screen = {
    .name    = "menu",
    .enter   = menu_enter,
    .leave   = menu_leave,
    .draw    = menu_draw,
    .input   = menu_input,
    .timeout = NULL,
    .tick    = menu_tick,
};
