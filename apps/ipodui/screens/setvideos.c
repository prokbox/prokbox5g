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
 * iPod UI shell: Videos > Settings (Phase 14a)
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

/* Videos > Settings (docs/phase14a-brief.md 3.8): a short full-width list
 * of two blue inline values that SELECT cycles in place, as the Settings
 * rows do.
 *   Start Playing  Where Left Off | From Beginning  (Ask is hidden)
 *   Fit to Screen  On | Off  (stored; the player has no mode for it yet,
 *                  and converted files are 320x240, so both look alike)
 * Hidden until supported: TV Out, TV Signal, Widescreen, Captions,
 * Subtitles, Alternate Audio. The values are the settings
 * ipodui_video_start and ipodui_video_fit (settings_list.c).
 * Compiled only with HAVE_BCMFW_VIDEO (apps/SOURCES). */

#include "config.h"
#include <stdio.h>
#include "lang.h"
#include "settings.h"
#include "ipodui/fe_be.h"   /* P19 S1: settings saves cross */
#include "ipodui/paint.h"
#include "ipodui/menu.h"
#include "ipodui/screens/video.h"

#define VAL_LEN 48

static char val_label[2][VAL_LEN];      /* "Name" UI_VALUE_SEP "Value" */

static void changed(void)
{
    fe_settings_save();
    ui_invalidate(UI_DIRTY_BODY);       /* the rows are rebuilt (refresh) */
}

static void act_start(void)
{
    global_settings.ipodui_video_start =
        global_settings.ipodui_video_start ? 0 : 1;
    changed();
}

static void act_fit(void)
{
    global_settings.ipodui_video_fit = !global_settings.ipodui_video_fit;
    changed();
}

static struct ui_menu_item items[] = {
    { NULL, NULL, NULL, NULL, act_start, NULL, NULL },
    { NULL, NULL, NULL, NULL, act_fit, NULL, NULL },
};

static void refresh(void);

static struct ui_menu menu = {
    NULL, items, 2, false, false, refresh
};

/* menu.c calls it on enter and before every full draw */
static void refresh(void)
{
    menu.title = str(LANG_SETTINGS);
    snprintf(val_label[0], VAL_LEN, "%s%c%s", str(LANG_IPODUI_START_PLAYING),
             UI_VALUE_SEP,
             str(global_settings.ipodui_video_start
                 ? LANG_IPODUI_FROM_BEGINNING : LANG_IPODUI_WHERE_LEFT_OFF));
    snprintf(val_label[1], VAL_LEN, "%s%c%s", str(LANG_IPODUI_FIT_TO_SCREEN),
             UI_VALUE_SEP,
             str(global_settings.ipodui_video_fit ? LANG_ON : LANG_OFF));
    items[0].label = val_label[0];
    items[1].label = val_label[1];
}

void ui_setvideos_push(void)
{
    ui_push(&ui_menu_screen, &menu);
}
