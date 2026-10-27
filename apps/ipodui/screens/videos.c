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
 * iPod UI shell: Videos menu (Phase 14a)
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

/* The Videos split screen (docs/phase14a-brief.md 3.6, 4.5): Movies, TV
 * Shows, Settings, built like the Music menu (a data-only ui_menu run by
 * menu.c, as screens/datetime.c does). Movies and TV Shows push the
 * generic browse screen over LIB_MOVIES / LIB_TVSHOWS; Settings pushes
 * V6's Videos settings. The 7G's other rows (Music Videos, Rentals,
 * Video Podcasts, Video Playlists) stay hidden (owner D1/D2). All labels
 * are language-system strings, filled in on enter. */
#include "config.h"
#include "lang.h"
#include "system.h"
#include "ipodui/ipodui.h"
#include "ipodui/menu.h"
#include "ipodui/library.h"
#include "ipodui/preview.h"
#include "ipodui/screens/screens.h"
#include "ipodui/screens/video.h"

#define ANY LIB_FILTER_ANY

static struct ui_browse_arg movies_arg = {
    NULL, LIB_MOVIES, ANY, UI_ALL_NONE, false };
static struct ui_browse_arg tvshows_arg = {
    NULL, LIB_TVSHOWS, ANY, UI_ALL_NONE, false };

/* The panel drifts over the posters, as on the main menu's Videos row */
static struct ui_menu_item videos_items[] = {
    { NULL, &ui_browse_screen, &movies_arg, NULL, NULL,
      &ui_preview_videos, NULL },
    { NULL, &ui_browse_screen, &tvshows_arg, NULL, NULL,
      &ui_preview_videos, NULL },
    { NULL, NULL, NULL, NULL, ui_setvideos_push,
      &ui_preview_videos, NULL },
};

static struct ui_menu videos_menu = {
    NULL, videos_items, ARRAYLEN(videos_items), true,   /* split */
    true, NULL,                                         /* library footer */
};

static int videos_enter(struct ui_screen *s, const void *arg)
{
    (void)arg;
    movies_arg.title = videos_items[0].label = str(LANG_IPODUI_MOVIES);
    tvshows_arg.title = videos_items[1].label = str(LANG_IPODUI_TV_SHOWS);
    videos_items[2].label = str(LANG_SETTINGS);
    videos_menu.title = str(LANG_IPODUI_VIDEOS);
    return ui_menu_screen.enter(s, &videos_menu);
}

static void videos_leave(struct ui_screen *s)
{
    ui_menu_screen.leave(s);
}

static void videos_draw(struct ui_screen *s, unsigned dirty)
{
    ui_menu_screen.draw(s, dirty);
}

static enum ui_result videos_input(struct ui_screen *s,
                                   const struct ui_input *in)
{
    return ui_menu_screen.input(s, in);
}

static int videos_tick(struct ui_screen *s)
{
    return ui_menu_screen.tick(s);
}

const struct ui_screen_ops ui_videos_screen = {
    .name    = "videos",
    .enter   = videos_enter,
    .leave   = videos_leave,
    .draw    = videos_draw,
    .input   = videos_input,
    .timeout = NULL,
    .tick    = videos_tick,
};
