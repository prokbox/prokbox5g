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

/* The resident Extras list (docs/phase12-brief.md 3.2, M2; spec
 * extras.md; owner photos 1191, photo 1197): a split menu, the eight
 * Extras in stock order, the highlighted row's art in the panel. Drawn by
 * menu.c and list.c like every other menu; a row opens its Extra through
 * extras_open(). A row whose module this unit cannot run is hidden. */
#include "config.h"
#include "lang.h"
#include "ipodui/menu.h"
#include "ipodui/preview.h"
#include "ipodui/extras.h"

#define VIS(name, x) \
    static bool vis_##name(void) { return extras_available(x); }
VIS(clocks, XT_CLOCKS)
VIS(calendars, XT_CALENDARS)
VIS(contacts, XT_CONTACTS)
VIS(alarms, XT_ALARMS)
VIS(games, XT_GAMES)
VIS(notes, XT_NOTES)
VIS(screenlock, XT_SCREENLOCK)
VIS(stopwatch, XT_STOPWATCH)

#define ROW(name) { NULL, NULL, NULL, vis_##name, extras_go_##name, NULL, NULL }
static struct ui_menu_item rows[XT__N] = {
    [XT_CLOCKS]     = ROW(clocks),
    [XT_CALENDARS]  = ROW(calendars),
    [XT_CONTACTS]   = ROW(contacts),
    [XT_ALARMS]     = ROW(alarms),
    [XT_GAMES]      = ROW(games),
    [XT_NOTES]      = ROW(notes),
    [XT_SCREENLOCK] = ROW(screenlock),
    [XT_STOPWATCH]  = ROW(stopwatch),
};

/* menu.c calls this on enter and before every full draw: the labels
 * follow the language, the panels the landed art */
static void extras_refresh(void)
{
    int x;

    ui_extras_menu.title = str(LANG_IPODUI_EXTRAS);
    for (x = 0; x < XT__N; x++)
    {
        rows[x].label = str(extras_table[x].title_lang);
        rows[x].preview = extras_panel(x);
    }
}

struct ui_menu ui_extras_menu = {
    NULL, rows, XT__N, true,            /* split, like the 7G's */
    false, extras_refresh,
};
