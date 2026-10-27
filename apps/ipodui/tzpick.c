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

/* The region -> city picker (tzpick.h), resident so that Date & Time (the
 * Settings module) and Clocks (its own module) share one copy: moved here
 * from screens/datetime.c unchanged in look and behaviour (P11 3.6). Two
 * value lists (screens/language.c): the regions, then the chosen region's
 * cities in display order, each with the calendar tile at the right; the
 * caller's done() decides what a city does and how many lists leave.
 *
 * State: which picker is open and the region its city list shows (5 B),
 * and the two lists' descriptors (their title is the caller's). One
 * picker is open at a time: it is a leaf of whichever screen opened it. */

#include "config.h"
#include <stddef.h>
#include "lang.h"
#include "ipodui/screen_stack.h"
#include "ipodui/screens/panels.h"
#include "ipodui/screens/screens.h"
#include "ipodui/tzpick.h"

static const struct tzpick *pick;       /* the open picker */
static unsigned char sel_region;        /* the region the city list shows */

static int current(void)
{
    return pick && pick->current ? pick->current(pick->arg) : -1;
}

static int region_get(void)
{
    int c = current();
    return c >= 0 ? tzp_city_region(c) : 0;
}

static const char *region_label(int idx, char *buf, size_t len)
{
    (void)buf; (void)len;
    return str(tzp_region_lang(idx));
}

static int city_first(void)
{
    return tzp_region_first(sel_region);
}

static int city_count(void)
{
    return tzp_region_first(sel_region + 1) - city_first();
}

static int city_get(void)
{
    int i, c = current();

    for (i = 0; i < city_count(); i++)
        if (tzp_order(city_first() + i) == c)
            return i;
    return 0;
}

static const char *city_label(int idx, char *buf, size_t len)
{
    (void)buf; (void)len;
    return str(tzp_city_lang(tzp_order(city_first() + idx)));
}

static int city_set(int idx)
{
    int id = tzp_order(city_first() + idx);
    return pick && pick->done ? pick->done(id, pick->arg) : 2;
}

static struct ui_vlist city_list = {
    LANG_IPODUI_TIME_ZONE, UI_TILE_CALENDAR, city_count, city_get,
    city_set, city_label, city_label };

static int region_set(int idx)
{
    sel_region = idx;
    ui_push(&ui_vlist_screen, &city_list);
    return 0;                       /* the region list stays below */
}

static struct ui_vlist region_list = {
    LANG_IPODUI_TIME_ZONE, UI_TILE_CALENDAR, tzp_region_count, region_get,
    region_set, region_label, NULL };

/* -- the region list as a screen ------------------------------------------ */

static int tzs_enter(struct ui_screen *s, const void *arg)
{
    pick = arg;
    region_list.title = city_list.title = pick->title_lang;
    return ui_vlist_screen.enter(s, &region_list);
}

static void tzs_draw(struct ui_screen *s, unsigned dirty)
{
    ui_vlist_screen.draw(s, dirty);
}

static enum ui_result tzs_input(struct ui_screen *s, const struct ui_input *in)
{
    return ui_vlist_screen.input(s, in);
}

static int tzs_tick(struct ui_screen *s)
{
    return ui_vlist_screen.tick(s);
}

static void tzs_panel(struct ui_screen *s, const struct ui_panel *p)
{
    ui_vlist_screen.draw_into_panel(s, p);
}

const struct ui_screen_ops tzpick_screen = {
    .name    = "vlist",
    .enter   = tzs_enter,
    .leave   = NULL,                    /* arena: released by the pop */
    .draw    = tzs_draw,
    .input   = tzs_input,
    .timeout = NULL,
    .tick    = tzs_tick,
    .draw_into_panel = tzs_panel,
};

void tzpick_push(const struct tzpick *p)
{
    ui_push(&tzpick_screen, p);
}
