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

/* Settings > Reset Settings (docs/phase11-brief.md section 3.6; frozen
 * default D3): the light prompt shared with Audiobooks (setwidgets.h)
 * with a segmented Reset | Cancel, Cancel preselected. The wheel or
 * NEXT/PREV moves the segment; SELECT acts; MENU cancels.
 *
 * Reset puts back the factory value of every setting the 7G's Settings
 * shows (the list below), then applies each one where it acts (targeted:
 * backlight, brightness, EQ, Sound Check, audiobook speed, Time in Title,
 * shuffle and repeat on a playing queue), saves, and shows the Language
 * list; SELECT there lands on the main menu (COMP-7).
 *
 * Kept: the device name, the volume limit and its combination (Screen
 * Lock's is forgotten, P12), serial
 * and model, the time zone (resetting it would move the clock), and
 * everything that is not a setting (play counts and ratings, resume
 * points, books.dat, On-The-Go). Rockbox's own hidden settings are kept
 * too: they hold the values the 7G behaviour needs, and some only act
 * at boot.
 *
 * Why not settings_reset() (the brief's first plan): it resets every
 * Rockbox setting, hidden ones included (database, buffering, playlist
 * size...), whose defaults are not the shipped config's and only apply
 * at the next boot, and it unloads the stock UI font. Resetting the
 * stock-visible list with reset_setting() keeps the rest exactly as
 * installed, and never calls settings_apply(true) (fonts, skins:
 * allocations; checklist item 2). */

#include "config.h"
#include <stdbool.h>
#include "system.h"
#include "kernel.h"
#include "lang.h"
#include "settings.h"
#include "ipodui/fe_be.h"   /* P19 S1: settings saves cross */
#include "audio.h"
#include "playlist.h"
#include "misc.h"
#include "backlight.h"
#include "ipodui/anim.h"
#include "ipodui/header.h"
#include "ipodui/vlimit.h"
#include "ipodui/alarm.h"
#include "ipodui/ab_book.h"
#include "ipodui/library.h"
#include "ipodui/screens/setwidgets.h"
#include "ipodui/screens/screens.h"

enum { SEG_RESET, SEG_CANCEL, SEG_COUNT };
static const short seg_lang[SEG_COUNT] = { LANG_IPODUI_RESET, LANG_CANCEL_0 };

/* s->priv: the segment, plus RESET_DONE once the reset ran (the prompt
 * then leaves when it comes back on top) */
#define RESET_DONE  0x100

/* the settings the 7G's Settings tree shows, back to their defaults */
static void *const reset_vars[] = {
    &global_settings.playlist_shuffle,
    &global_settings.ipodui_shuffle_albums,
    &global_settings.repeat_mode,
    &global_settings.ipodui_main_menu,
    &global_settings.ipodui_music_menu,
    &global_settings.backlight_timeout,
#ifdef HAVE_BACKLIGHT_BRIGHTNESS
    &global_settings.brightness,
#endif
    &global_settings.ipodui_ab_speed,
    &global_settings.ipodui_ab_sort,
    &global_settings.ipodui_eq_preset,
    &global_settings.replaygain_settings.type,
    &global_settings.keyclick,              /* Clicker: both halves */
#ifdef HAVE_HARDWARE_CLICK
    &global_settings.keyclick_hardware,
#endif
    &global_settings.timeformat,
    &global_settings.ipodui_time_in_title,
    &global_settings.ipodui_sort_last,
    /* P12 Screen Lock (lock_core.h, review M12): Reset forgets its
     * combination (reachable only unlocked); Volume Limit's is kept */
    &global_settings.ipodui_lock_code,
    &global_settings.ipodui_locked,
};

static void do_reset(void)
{
    bool was_shuffle = global_settings.playlist_shuffle;
    int was_repeat = global_settings.repeat_mode;
    bool playing = (audio_status() & AUDIO_STATUS_PLAY) != 0;
    unsigned i, n = 0;

    for (i = 0; i < ARRAYLEN(reset_vars); i++)
    {
        const struct settings_list *set = find_setting(reset_vars[i]);
        if (set)
        {
            reset_setting(set, reset_vars[i]);
            n++;
        }
    }

    /* targeted applies: what each setting drives, nothing else */
    backlight_set_timeout(global_settings.backlight_timeout);
#ifdef HAVE_BACKLIGHT_BRIGHTNESS
    backlight_set_brightness(global_settings.brightness);
#endif
    ui_eq_apply();                      /* Off: bands flat, precut 0 */
    replaygain_update();                /* Sound Check */
    ab_speed_changed();                 /* a playing book's speed */
    lib_set_book_order(global_settings.ipodui_ab_sort);  /* Title (#59) */
    ui_header_set_time_in_title(global_settings.ipodui_time_in_title);
    if (playing && was_shuffle != global_settings.playlist_shuffle)
    {
        if (global_settings.playlist_shuffle)
            playlist_randomise(NULL, current_tick, true);
        else
            playlist_sort(NULL, true);
    }
    if (playing && was_repeat != global_settings.repeat_mode)
        audio_flush_and_reload_tracks();
    ui_vlimit_apply();                  /* the kept limit, clamped at once */
    alarm_reset();                      /* P12: alarms.dat and the PMU */
    fe_settings_save();
    UPDATE_TRACE("reset: settings n=%u tit=%d eq=%d", n,
                 (int)global_settings.ipodui_time_in_title,
                 (int)global_settings.eq_enabled);
    (void)n;                            /* trace only */
}

static int seg(const struct ui_screen *s)
{
    return (int)((intptr_t)s->priv & 0xff);
}

static int reset_enter(struct ui_screen *s, const void *arg)
{
    (void)arg;
    s->title = str(LANG_IPODUI_RESET_SETTINGS);
    s->priv = (void *)(intptr_t)SEG_CANCEL;     /* Cancel preselected */
    return 0;
}

static void reset_draw(struct ui_screen *s, unsigned dirty)
{
    if (dirty & UI_DIRTY_BODY)
    {
        ui_prompt_draw(LANG_IPODUI_RESET_HEADING, LANG_IPODUI_RESET_BODY);
        ui_seg_draw(seg_lang, SEG_COUNT, seg(s), true);
    }
    else if (dirty & UI_DIRTY_BAND)
        ui_seg_draw(seg_lang, SEG_COUNT, seg(s), false);
}

static enum ui_result reset_input(struct ui_screen *s,
                                  const struct ui_input *in)
{
    int v = seg(s), d = 0;

    if ((intptr_t)s->priv & RESET_DONE)
        return UI_HANDLED;              /* leaving (tick) */
    switch (in->key)
    {
        case UI_KEY_WHEEL:
            d = in->wheel_steps;
            break;
        case UI_KEY_NEXT:
        case UI_KEY_PREV:
            if (!in->release || in->long_press)
                return UI_HANDLED;
            d = in->key == UI_KEY_NEXT ? 1 : -1;
            break;
        case UI_KEY_SELECT:
            if (!in->release || in->long_press)
                return UI_HANDLED;
            if (v != SEG_RESET)
                return UI_POP;
            do_reset();
            s->priv = (void *)(intptr_t)(v | RESET_DONE);
            ui_push(&ui_vlist_screen, &ui_language_after_reset);
            return UI_HANDLED;
        default:
            return UI_IGNORED;          /* MENU: back, nothing reset */
    }
    v = MIN(MAX(v + d, 0), SEG_COUNT - 1);
    if (v != seg(s))
    {
        s->priv = (void *)(intptr_t)v;
        ui_invalidate_band(UI_SEG_Y, UI_SEG_H);
    }
    return UI_HANDLED;
}

/* back on top after the reset (MENU on the language list): leave */
static int reset_tick(struct ui_screen *s)
{
    if ((intptr_t)s->priv & RESET_DONE)
        ui_pop_request(s);
    return TIMEOUT_BLOCK;
}

const struct ui_screen_ops ui_reset_screen = {
    .name    = "reset",
    .enter   = reset_enter,
    .leave   = NULL,
    .draw    = reset_draw,
    .input   = reset_input,
    .timeout = NULL,
    .tick    = reset_tick,
};
