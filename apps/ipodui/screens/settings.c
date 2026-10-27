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
 * iPod UI shell: the Settings tree (#38)
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

/* The stock 2.0.5 Settings tree as a split screen (spec settings.md,
 * catalogue 7.1, owner photos settings-01 ... settings-17): 17 rows,
 * each with its tile panel; blue inline values on Shuffle, Repeat, Sound
 * Check, Clicker and Sort By, which SELECT cycles in place; Backlight a
 * split value list (backlight-values.jpg); Audiobooks a short submenu
 * (#59): Playback Speed, the full-screen speed prompt (audiobooks-speed.jpg,
 * Phase 6), and Sort By Title | Author. Phase 11 wires every row:
 * Main Menu and Music Menu open the checklists (checklist.c), Volume Limit
 * the dark limit screen (vlimit.c), and About, Brightness, EQ, Date & Time,
 * Language, Legal and Reset Settings their own screens (U2b). Sort By is
 * the stored setting ipodui_sort_last; the EQ panel caption is the preset.
 *
 * The rows are a struct ui_menu that menu.c draws. Its labels come from
 * the language (ui_settings_init(), at shell start), so the item array
 * is writable; an inline value rides in the label after UI_VALUE_SEP
 * (paint.h), composed into a small buffer per value row (5 x 48 B). */

#include "config.h"
#include <stdio.h>
#include <string.h>
#include "system.h"
#include "kernel.h"
#include "lcd.h"
#include "font.h"
#include "logf.h"
#include "settings.h"
#include "ipodui/fe_be.h"   /* P19 S1: settings saves cross */
#include "lang.h"
#include "audio.h"
#include "playlist.h"
#include "misc.h"
#include "backlight.h"
#include "timefuncs.h"
#include "ipodui/theme.h"
#include "ipodui/paint.h"
#include "ipodui/list.h"
#include "ipodui/menu.h"
#include "ipodui/preview.h"
#include "ipodui/screens/panels.h"
#include "ipodui/screens/setwidgets.h"
#include "ipodui/screens/screens.h"    /* the P11 screens */
#include "ipodui/screens/checklist.h"
#include "ipodui/vlimit.h"
#include "ipodui/panelart.h"
#include "ipodui/ab_book.h"
#include "ipodui/library.h"
#include "ipodui/anim.h"
#include "ipodui/screen_stack.h"

/* -- tiles ------------------------------------------------------------------ */

/* An item's panel: its tile art and caption (preview_arg of the item). */
struct set_tile {
    int title;                                  /* LANG_* */
    int art;                                    /* panelart / UI_TILE_* */
    const char *(*caption)(char *buf, size_t len);   /* NULL: none */
};

#define ART_SHUFFLE (-1)        /* 'art': Shuffle's, by its value */
static int shuffle_art(void);

static int tile_draw(const struct ui_panel *p, const void *arg, long phase)
{
    const struct set_tile *t = arg;
    char buf[64];
    int art = t->art == ART_SHUFFLE ? shuffle_art() : t->art;

    (void)phase;
    ui_panel_draw_tile(p, str(t->title), art,
                       t->caption ? t->caption(buf, sizeof(buf)) : NULL);
    return ui_panel_tile_wake(art);     /* the calendar turns at midnight */
}

const struct ui_preview_ops ui_preview_settings_tile = {
    "tile", tile_draw, NAV_REST_ICON, UI_PANEL_XFADE };

/* -- values ------------------------------------------------------------------ */

static void changed(void)
{
    fe_settings_save();
    ui_invalidate(UI_DIRTY_BODY);   /* rows (menu.c rebuilds, refresh()) */
    ui_panel_invalidate();          /* the caption */
}

enum { SHUF_OFF, SHUF_SONGS, SHUF_ALBUMS };

static int shuffle_value(void)
{
    if (!global_settings.playlist_shuffle)
        return SHUF_OFF;
    return global_settings.ipodui_shuffle_albums ? SHUF_ALBUMS : SHUF_SONGS;
}

static void act_shuffle(void)
{
    /* stock order: Off -> Songs -> Albums -> Off (owner, #38 end review).
     * Albums is stored (ipodui_shuffle_albums) and shuffles like Songs
     * until album shuffle is wired into playback (Phase 5) */
    int v = (shuffle_value() + 1) % 3;
    bool was = global_settings.playlist_shuffle;

    global_settings.playlist_shuffle = v != SHUF_OFF;
    global_settings.ipodui_shuffle_albums = v == SHUF_ALBUMS;
    if (was != global_settings.playlist_shuffle
        && (audio_status() & AUDIO_STATUS_PLAY))
    {
        if (global_settings.playlist_shuffle)
            playlist_randomise(NULL, current_tick, true);
        else
            playlist_sort(NULL, true);
    }
    changed();
}

static void act_repeat(void)
{
    /* stock order: Off -> One -> All -> Off */
    switch (global_settings.repeat_mode)
    {
        case REPEAT_OFF: global_settings.repeat_mode = REPEAT_ONE; break;
        case REPEAT_ONE: global_settings.repeat_mode = REPEAT_ALL; break;
        default:         global_settings.repeat_mode = REPEAT_OFF; break;
    }
    if (audio_status() & AUDIO_STATUS_PLAY)
        audio_flush_and_reload_tracks();
    changed();
}

static bool sound_check_on(void)
{
    return global_settings.replaygain_settings.type != REPLAYGAIN_OFF;
}

static void act_sound_check(void)
{
    global_settings.replaygain_settings.type =
        sound_check_on() ? REPLAYGAIN_OFF : REPLAYGAIN_TRACK;
    replaygain_update();
    changed();
}

/* Clicker (owner 2026-10-20): Off or On. On is both the click mixed
 * into the audio (the soft click, ipodui/clicktone.h, heard in headphones) and
 * the piezo line, which drives the haptic mod; never one without the
 * other. A unit saved with only one half on counts as On, and stepping
 * from it goes to Off. */
#define CLICKER_LEVEL 2     /* any level > 0: the soft click's own level
                             * is set in clicktone.h */

static bool clicker_on(void)
{
#ifdef HAVE_HARDWARE_CLICK
    if (global_settings.keyclick_hardware)
        return true;
#endif
    return global_settings.keyclick > 0;
}

static void act_clicker(void)
{
    bool on = !clicker_on();

    global_settings.keyclick = on ? CLICKER_LEVEL : 0;
#ifdef HAVE_HARDWARE_CLICK
    global_settings.keyclick_hardware = on;
#endif
    changed();
}

/* Sort By: First / Last name. Stored (ipodui_sort_last, P11) and shown as
 * stock does; it orders nothing until Contacts exists. */
static void act_sort(void)
{
    global_settings.ipodui_sort_last = !global_settings.ipodui_sort_last;
    changed();
}

static int shuffle_lang(void)
{
    static const int ids[] = { LANG_OFF, LANG_IPODUI_SONGS,
                               LANG_IPODUI_ALBUMS };
    return ids[shuffle_value()];
}

/* Off: two parallel arrows; Songs or Albums: crossed (owner photo
 * settings-02-shuffle, owner note 2026-10-08) */
static int shuffle_art(void)
{
    return shuffle_value() == SHUF_OFF ? PANELART_SHUFFLE
                                       : PANELART_SHUFFLE_ON;
}

static int repeat_lang(void)
{
    return global_settings.repeat_mode == REPEAT_ONE ? LANG_IPODUI_ONE :
           global_settings.repeat_mode == REPEAT_ALL ? LANG_ALL : LANG_OFF;
}

static int sound_check_lang(void)
{
    return sound_check_on() ? LANG_ON : LANG_OFF;
}

static int clicker_lang(void)
{
    return clicker_on() ? LANG_ON : LANG_OFF;
}

static int clicker_cap_lang(void)
{
    return clicker_on() ? LANG_IPODUI_CAP_CLICKER_ON
                        : LANG_IPODUI_CAP_CLICKER_OFF;
}

static int sort_lang(void)
{
    return global_settings.ipodui_sort_last ? LANG_IPODUI_LAST
                                            : LANG_IPODUI_FIRST;
}

/* Audiobooks: the speed (enum ab_speed; out of range reads as Normal) */
static int speed_value(void)
{
    int v = global_settings.ipodui_ab_speed;
    return (v >= 0 && v < AB_SPEED_COUNT) ? v : AB_SPEED_DEFAULT;
}

/* the Playback Speed panel caption follows the value (⚠️14); the
 * Settings row says what the submenu sets (owner, 2026-10-10) */
static int speed_cap_lang(void)
{
    static const short ids[AB_SPEED_COUNT] = {
        LANG_IPODUI_CAP_SLOWER, LANG_IPODUI_CAP_NORMAL,
        LANG_IPODUI_CAP_FASTER };
    return ids[speed_value()];
}

/* Audiobooks > Sort By (#59): Title or Author (enum lib_book_order; out
 * of range reads as Title) */
static bool ab_by_author(void)
{
    return global_settings.ipodui_ab_sort == LIB_BOOKS_BY_AUTHOR;
}

static int ab_sort_lang(void)
{
    return ab_by_author() ? LANG_IPODUI_AB_SORT_AUTHOR
                          : LANG_IPODUI_AB_SORT_TITLE;
}

static void act_ab_sort(void)
{
    global_settings.ipodui_ab_sort = ab_by_author() ? LIB_BOOKS_BY_TITLE
                                                    : LIB_BOOKS_BY_AUTHOR;
    lib_set_book_order(global_settings.ipodui_ab_sort);
    UPDATE_TRACE("settings ab sort=%s", ab_by_author() ? "author" : "title");
    changed();
}

/* EQ: the panel caption is the preset's name (Off, then the 22 presets;
 * eq.c writes ipodui_eq_preset). LANG_IPODUI_EQ_* are in preset order. */
static int eq_cap_lang(void)
{
    int p = global_settings.ipodui_eq_preset;

    return (p >= 1 && p <= 22) ? LANG_IPODUI_EQ_ACOUSTIC + (p - 1) : LANG_OFF;
}

/* Backlight (stock values; no 15 s): seconds, -1 = Off, 0 = Always On */
static const signed char bl_values[] = { -1, 2, 5, 10, 20, 30, 0 };
#define BL_COUNT ((int)ARRAYLEN(bl_values))

static int bl_index(void)
{
    int i;
    for (i = 0; i < BL_COUNT; i++)
        if (bl_values[i] == global_settings.backlight_timeout)
            return i;
    return 3;                                   /* 10 Seconds, the default */
}

static const char *bl_label(int i, char *buf, size_t len)
{
    if (bl_values[i] < 0)
        return str(LANG_OFF);
    if (bl_values[i] == 0)
        return str(LANG_ALWAYS_ON);
    snprintf(buf, len, str(LANG_IPODUI_SECONDS), bl_values[i]);
    return buf;
}

/* -- captions (catalogue 7.1; unphotographed values owner round) ------------------ */

#define CAPTION(name, expr) \
    static const char *cap_##name(char *buf, size_t len) \
    { (void)buf; (void)len; return (expr); }

CAPTION(shuffle, str(shuffle_value() == SHUF_ALBUMS ?
                     LANG_IPODUI_CAP_SHUFFLE_ALBUMS :
                     shuffle_value() == SHUF_SONGS ?
                     LANG_IPODUI_CAP_SHUFFLE_SONGS :
                     LANG_IPODUI_CAP_SHUFFLE_OFF))
CAPTION(repeat, str(global_settings.repeat_mode == REPEAT_ONE ?
                    LANG_IPODUI_CAP_REPEAT_ONE :
                    global_settings.repeat_mode == REPEAT_ALL ?
                    LANG_IPODUI_CAP_REPEAT_ALL : LANG_IPODUI_CAP_REPEAT_OFF))
CAPTION(main_menu, str(LANG_IPODUI_CAP_MAIN_MENU))
CAPTION(music_menu, str(LANG_IPODUI_CAP_MUSIC_MENU))
CAPTION(volume, str(LANG_IPODUI_CAP_VOLUME_LIMIT))
CAPTION(audiobooks, str(LANG_IPODUI_CAP_AB_MENU))   /* what it sets (owner) */
CAPTION(ab_speed, str(speed_cap_lang()))
CAPTION(eq, str(eq_cap_lang()))
CAPTION(sound_check, str(sound_check_on() ? LANG_IPODUI_CAP_SOUND_CHECK_ON :
                         LANG_IPODUI_CAP_SOUND_CHECK_OFF))
CAPTION(clicker, str(clicker_cap_lang()))
CAPTION(sort, str(global_settings.ipodui_sort_last ?
                  LANG_IPODUI_CAP_SORT_LAST : LANG_IPODUI_CAP_SORT_FIRST))
CAPTION(ab_sort, str(ab_by_author() ? LANG_IPODUI_CAP_AB_SORT_AUTHOR :
                     LANG_IPODUI_CAP_AB_SORT_TITLE))
CAPTION(legal, str(LANG_IPODUI_CAP_LEGAL))
CAPTION(reset, str(LANG_IPODUI_CAP_RESET))

static const char *cap_backlight(char *buf, size_t len)
{
    return bl_label(bl_index(), buf, len);
}

static const char *cap_brightness(char *buf, size_t len)
{
#ifdef HAVE_BACKLIGHT_BRIGHTNESS
    int range = MAX_BRIGHTNESS_SETTING - MIN_BRIGHTNESS_SETTING;
    snprintf(buf, len, str(LANG_IPODUI_PERCENT),
             (global_settings.brightness - MIN_BRIGHTNESS_SETTING) * 100 /
             (range ? range : 1));
    return buf;
#else
    (void)buf; (void)len;
    return NULL;                                /* no brightness control */
#endif
}

static const char *cap_date(char *buf, size_t len)
{
    (void)buf; (void)len;
    return str(LANG_IPODUI_SUNDAY + get_time()->tm_wday);
}

static const struct set_tile
    t_shuffle  = { LANG_SHUFFLE, ART_SHUFFLE, cap_shuffle },
    t_repeat   = { LANG_REPEAT, PANELART_REPEAT, cap_repeat },
    t_main     = { LANG_IPODUI_MAIN_MENU, PANELART_MENU, cap_main_menu },
    t_music    = { LANG_IPODUI_MUSIC_MENU, PANELART_MENU, cap_music_menu },
    t_volume   = { LANG_IPODUI_VOLUME_LIMIT, PANELART_VOLUME, cap_volume },
    t_backl    = { LANG_BACKLIGHT, PANELART_TIMER, cap_backlight },
    t_bright   = { LANG_IPODUI_BRIGHTNESS, PANELART_SUN, cap_brightness },
    t_books    = { LANG_IPODUI_AUDIOBOOKS, PANELART_BOOK, cap_audiobooks },
    t_ab_speed = { LANG_IPODUI_AB_PLAYBACK_SPEED, PANELART_BOOK,
                   cap_ab_speed },
    t_ab_sort  = { LANG_IPODUI_SORT_BY, PANELART_BOOK, cap_ab_sort },
    t_eq       = { LANG_IPODUI_EQ, PANELART_EQ, cap_eq },
    t_sound    = { LANG_IPODUI_SOUND_CHECK, PANELART_SPEAKER, cap_sound_check },
    t_clicker  = { LANG_IPODUI_CLICKER, PANELART_CLICKER, cap_clicker },
    t_date     = { LANG_IPODUI_DATE_TIME, UI_TILE_CALENDAR, cap_date },
    t_sort     = { LANG_IPODUI_SORT_BY, PANELART_CONTACTS, cap_sort },
    t_language = { LANG_LANGUAGE, UI_TILE_WORDS, NULL },
    t_legal    = { LANG_IPODUI_LEGAL, UI_TILE_LOGO, cap_legal },
    t_reset    = { LANG_IPODUI_RESET_SETTINGS, PANELART_RESET, cap_reset };

/* -- Backlight: a split value list ------------------------------------------ */

/* Generic split value list (Backlight today): the current value is the
 * highlighted row on entry, no checkmark; SELECT sets it and returns. */
struct set_choice {
    int title;
    const struct set_tile *tile;
    int count;
    int  (*get)(void);
    void (*set)(int idx);
    const char *(*label)(int idx, char *buf, size_t len);
};

static void bl_set(int i)
{
    global_settings.backlight_timeout = bl_values[i];
    backlight_set_timeout(global_settings.backlight_timeout);
}

static const struct set_choice c_backlight = {
    LANG_BACKLIGHT, &t_backl, BL_COUNT, bl_index, bl_set, bl_label };

struct choice_state {
    struct ui_list list;
    const struct set_choice *c;
};
/* P11 screen arena bound (screen_stack.h): the state lives in the arena */
_Static_assert(sizeof(struct choice_state) <= UI_ARENA_B_CHOICE, "choice_state: arena bound");

/* NULL once the screen has left (choice_leave() cleared it): every caller
 * checks, so a stale call does nothing. The arena block never moves. */
static struct choice_state *choice_state(struct ui_screen *s)
{
    struct choice_state *cs = s ? s->priv : NULL;

    if (cs)
        cs->list.data = cs;
    return cs;
}

static const char *choice_label(int idx, void *data, char *buf, size_t len)
{
    const struct choice_state *cs = data;
    return cs->c->label(idx, buf, len);
}

static int choice_enter(struct ui_screen *s, const void *arg)
{
    const struct set_choice *c = arg;
    const struct ui_theme *t = ui_theme;
    struct choice_state *cs = ui_arena_alloc(sizeof(*cs));
    struct viewport vp;

    if (!cs)
    {
        logf("ipodui: settings choice: arena full");
        return -1;
    }
    s->priv = cs;
    s->title = str(c->title);
    s->header_w = t->split_x;
    cs->c = c;
    memset(&vp, 0, sizeof(vp));
    vp.y = t->header_h;
    vp.width = t->split_x;
    vp.height = LCD_HEIGHT - t->header_h;
    ui_list_init(&cs->list, &vp, c->count, choice_label, cs);
    cs->list.selected = c->get();
    ui_list_set_count(&cs->list, c->count);     /* keeps it visible */
    ui_panel_select_screen(s);
    return 0;
}

/* the arena takes the state back with the screen (screen_stack.c) */
static void choice_leave(struct ui_screen *s)
{
    s->priv = NULL;
}

static void choice_draw(struct ui_screen *s, unsigned dirty)
{
    struct choice_state *cs = choice_state(s);
    int y[UI_BANDS_MAX], h[UI_BANDS_MAX], i, n;

    if (!cs)
        return;
    if (dirty & UI_DIRTY_BODY)
    {
        ui_list_draw(&cs->list, cs->list.vp.y, cs->list.vp.height);
        ui_panel_draw_now();
    }
    else if (dirty & UI_DIRTY_BAND)
    {
        n = ui_dirty_bands(y, h);
        for (i = 0; i < n; i++)
            ui_list_draw(&cs->list, y[i], h[i]);
    }
}

static enum ui_result choice_input(struct ui_screen *s,
                                   const struct ui_input *in)
{
    struct choice_state *cs = choice_state(s);
    int y[2], h[2], i, n;

    if (!cs)
        return UI_IGNORED;
    switch (in->key)
    {
        case UI_KEY_WHEEL:
            n = ui_list_move(&cs->list, in->wheel_steps, y, h);
            for (i = 0; i < n; i++)
                ui_invalidate_band(y[i], h[i]);
            return UI_HANDLED;
        case UI_KEY_SELECT:
            if (!in->release || in->long_press)
                return UI_HANDLED;
            cs->c->set(cs->list.selected);
            fe_settings_save();
            return UI_POP;
        default:
            return UI_IGNORED;
    }
}

static int choice_tick(struct ui_screen *s)
{
    struct choice_state *cs = choice_state(s);
    return cs ? ui_list_tick(&cs->list) : TIMEOUT_BLOCK;
}

static void choice_panel(struct ui_screen *s, const struct ui_panel *p)
{
    struct choice_state *cs = choice_state(s);

    if (!cs)
    {
        lcd_set_viewport(p->vp);
        lcd_set_foreground(ui_theme->panel_bg);
        lcd_fillrect(0, 0, UI_PANEL_W, UI_PANEL_H);
        return;
    }
    tile_draw(p, cs->c->tile, 0);
}

static const struct ui_screen_ops choice_screen = {
    .name    = "settings-choice",
    .enter   = choice_enter,
    .leave   = choice_leave,
    .draw    = choice_draw,
    .input   = choice_input,
    .timeout = NULL,
    .tick    = choice_tick,
    .draw_into_panel = choice_panel,
};

/* -- Audiobooks: the speed prompt -------------------------------------------- */

/* A light full-screen prompt, not a list (photo audiobooks-speed.jpg):
 * the heading and the body at the left, and near the bottom a light-gray
 * rounded shelf holding a dark segmented control Slower | Normal |
 * Faster, the chosen segment blue. The wheel or PREV/NEXT moves the
 * segment; SELECT or MENU saves and returns (⚠️ the 7G may apply on the
 * move: owner round). The prompt and the control are shared
 * (setwidgets.h, P11). The segment lives in s->priv: no allocation. */
static const short sp_seg_lang[AB_SPEED_COUNT] = {
    LANG_IPODUI_SLOWER, LANG_IPODUI_NORMAL, LANG_IPODUI_FASTER };

static int sp_get(const struct ui_screen *s)
{
    return (int)(intptr_t)s->priv;
}

static int speed_enter(struct ui_screen *s, const void *arg)
{
    (void)arg;
    s->title = str(LANG_IPODUI_AUDIOBOOKS);
    s->priv = (void *)(intptr_t)speed_value();
    return 0;
}

static void speed_draw(struct ui_screen *s, unsigned dirty)
{
    if (dirty & UI_DIRTY_BODY)
    {
        ui_prompt_draw(LANG_IPODUI_AB_SPEED_HEADING, LANG_IPODUI_AB_SPEED_BODY);
        ui_seg_draw(sp_seg_lang, AB_SPEED_COUNT, sp_get(s), true);
    }
    else if (dirty & UI_DIRTY_BAND)
        ui_seg_draw(sp_seg_lang, AB_SPEED_COUNT, sp_get(s), false);
}

static void speed_commit(struct ui_screen *s)
{
    int v = sp_get(s);

    if (v == global_settings.ipodui_ab_speed)
        return;
    global_settings.ipodui_ab_speed = v;
    fe_settings_save();
    ab_speed_changed();             /* applies at once to a playing book */
}

static enum ui_result speed_input(struct ui_screen *s,
                                  const struct ui_input *in)
{
    int v = sp_get(s), d = 0;

    switch (in->key)
    {
        case UI_KEY_WHEEL:
            d = in->wheel_steps;
            break;
        case UI_KEY_NEXT:
        case UI_KEY_PREV:
            if (!in->release || in->long_press)
                return UI_HANDLED;  /* a step on the release, no seek */
            d = in->key == UI_KEY_NEXT ? 1 : -1;
            break;
        case UI_KEY_SELECT:
        case UI_KEY_MENU:
            if (!in->release || in->long_press)
                return UI_HANDLED;
            speed_commit(s);
            return UI_POP;
        default:
            return UI_IGNORED;
    }
    v += d;
    v = v < 0 ? 0 : v >= AB_SPEED_COUNT ? AB_SPEED_COUNT - 1 : v;
    if (v != sp_get(s))
    {
        s->priv = (void *)(intptr_t)v;
        ui_invalidate_band(UI_SEG_Y, UI_SEG_H);
    }
    return UI_HANDLED;
}

static const struct ui_screen_ops speed_screen = {
    .name    = "settings-speed",
    .enter   = speed_enter,
    .leave   = NULL,
    .draw    = speed_draw,
    .input   = speed_input,
    .timeout = NULL,
};

/* -- the tree ---------------------------------------------------------------- */

static bool refresh(void);

/* a row opens 'target' (arg NULL: the label is passed as the title) */
#define ROW(target, arg, act, t) \
    { NULL, target, arg, NULL, act, &ui_preview_settings_tile, &t }
#define VALUE(act, t)   ROW(NULL, NULL, act, t)

/* -- Audiobooks: Playback Speed and Sort By (#59) ----------------------------- */

/* A short split-screen menu (the Date & Time pattern): Playback Speed
 * opens the prompt above, unchanged; Sort By is a blue inline value that
 * SELECT cycles, Title -> Author -> Title. Both rows show the book tile;
 * the captions follow their values. */
static void ab_refresh(void);

static struct ui_menu_item ab_items[] = {
    ROW(&speed_screen, NULL, NULL, t_ab_speed),
    VALUE(act_ab_sort, t_ab_sort),
};

static char ab_value[48];             /* "Sort By" UI_VALUE_SEP value */

static struct ui_menu ab_menu = {
    NULL, ab_items, ARRAYLEN(ab_items), true, false, ab_refresh
};

/* menu.c calls it on enter and before every full draw */
static void ab_refresh(void)
{
    ab_menu.title = str(LANG_IPODUI_AUDIOBOOKS);
    ab_items[0].label = str(LANG_IPODUI_AB_PLAYBACK_SPEED);
    snprintf(ab_value, sizeof(ab_value), "%s%c%s", str(LANG_IPODUI_SORT_BY),
             UI_VALUE_SEP, str(ab_sort_lang()));
    ab_items[1].label = ab_value;
}

static struct ui_menu_item items[] = {
    { NULL, &ui_about_screen, NULL, refresh, NULL, &ui_preview_capacity,
      NULL },                                           /* About */
    VALUE(act_shuffle, t_shuffle),
    VALUE(act_repeat, t_repeat),
    ROW(&ui_checklist_screen, UI_CHECKLIST_MAIN, NULL, t_main),
    ROW(&ui_checklist_screen, UI_CHECKLIST_MUSIC, NULL, t_music),
    ROW(&ui_vlimit_screen, NULL, NULL, t_volume),
    ROW(&choice_screen, &c_backlight, NULL, t_backl),
    ROW(&ui_brightness_screen, NULL, NULL, t_bright),
    ROW(&ui_menu_screen, &ab_menu, NULL, t_books),
    ROW(&ui_eq_screen, NULL, NULL, t_eq),
    VALUE(act_sound_check, t_sound),
    VALUE(act_clicker, t_clicker),
    ROW(&ui_datetime_menu_screen, NULL, NULL, t_date),
    VALUE(act_sort, t_sort),
    ROW(&ui_language_screen, NULL, NULL, t_language),
    ROW(&ui_legal_screen, NULL, NULL, t_legal),
    ROW(&ui_reset_screen, NULL, NULL, t_reset),
};

/* label ids, in the order of items[] */
static const short item_lang[ARRAYLEN(items)] = {
    LANG_IPODUI_ABOUT, LANG_SHUFFLE, LANG_REPEAT, LANG_IPODUI_MAIN_MENU,
    LANG_IPODUI_MUSIC_MENU, LANG_IPODUI_VOLUME_LIMIT, LANG_BACKLIGHT,
    LANG_IPODUI_BRIGHTNESS, LANG_IPODUI_AUDIOBOOKS, LANG_IPODUI_EQ,
    LANG_IPODUI_SOUND_CHECK, LANG_IPODUI_CLICKER, LANG_IPODUI_DATE_TIME,
    LANG_IPODUI_SORT_BY, LANG_LANGUAGE, LANG_IPODUI_LEGAL,
    LANG_IPODUI_RESET_SETTINGS,
};

/* the rows with an inline value, and its LANG id now */
static const struct {
    unsigned char item;
    int (*value)(void);
} value_rows[] = {
    { 1, shuffle_lang }, { 2, repeat_lang }, { 10, sound_check_lang },
    { 11, clicker_lang }, { 13, sort_lang },
};
#define VALUE_LEN 48
static char value_label[ARRAYLEN(value_rows)][VALUE_LEN];

struct ui_menu ui_settings_menu = {
    NULL, items, ARRAYLEN(items), true, false, NULL
};

/* Composes the value rows' labels ("Name" UI_VALUE_SEP "Value"). menu.c
 * calls the first item's visible() whenever it rebuilds its rows (enter,
 * every full draw), so this doubles as that item's visible() and always
 * says yes. */
static bool refresh(void)
{
    unsigned i;

#ifdef IPODUI_MODULES
    mod_warm(MOD_SETTINGS);             /* #106: Legal, Date & Time, the
                                         * checklists load behind the list */
#endif
    for (i = 0; i < ARRAYLEN(value_rows); i++)
    {
        snprintf(value_label[i], VALUE_LEN, "%s%c%s",
                 str(item_lang[value_rows[i].item]), UI_VALUE_SEP,
                 str(value_rows[i].value()));
        items[value_rows[i].item].label = value_label[i];
    }
    return true;
}

void ui_settings_init(void)
{
    unsigned i;

    ui_settings_menu.title = str(LANG_SETTINGS);
    for (i = 0; i < ARRAYLEN(items); i++)
        items[i].label = str(item_lang[i]);
    refresh();
    ab_refresh();
    /* the saved Sort By, before any books list opens (#59) */
    lib_set_book_order(global_settings.ipodui_ab_sort);
}
