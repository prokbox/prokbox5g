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

/* The Extras registry (docs/phase12-brief.md 3.2, W1-F): the stock order,
 * each Extra's module, root screen, transition and row panel; opening an
 * Extra from the Extras list or a Main Menu item, in every build. */
#include "config.h"
#include <stdint.h>
#include "system.h"
#include "lang.h"
#include "ipodui/uilog.h"
#include "ipodui/anim.h"
#include "ipodui/screen_stack.h"
#include "ipodui/menu.h"
#include "ipodui/preview.h"
#include "ipodui/modload.h"
#include "ipodui/extras.h"
#include "ipodui/modclass.h"
#include "ipodui/screens/panels.h"
#include "ipodui/stopwatch.h"       /* the Stopwatch row's panel (extras_roots.h) */
#include "ipodui/sdk/sdk_int.h"     /* W4-0: the Games list, the games' tables */

/* Stock order (spec extras.md; photos 1191/1197). Split per the
 * brief: Calendars, Alarms, Stopwatch; the rest full width. */
const struct extra_ent extras_table[XT__N] = {
    [XT_CLOCKS]     = { LANG_IPODUI_X_CLOCKS, MOD_CLOCKS, MOD_SLOT_FIRST,
                        UI_T_REVEAL, 0, &ui_preview_extras },
    [XT_CALENDARS]  = { LANG_IPODUI_X_CALENDARS, MOD_CALENDARS,
                        MOD_SLOT_FIRST, UI_T_PUSH, 1,
                        &ui_preview_x_calendars },
    [XT_CONTACTS]   = { LANG_IPODUI_X_CONTACTS, MOD_CONTACTS,
                        MOD_SLOT_FIRST, UI_T_PUSH, 0,
                        &ui_preview_x_contacts },
    [XT_ALARMS]     = { LANG_IPODUI_X_ALARMS, MOD_ALARMS, MOD_SLOT_FIRST,
                        UI_T_PUSH, 1, &ui_preview_x_alarms },
    /* the Games list is resident (M2), wave 4 */
    [XT_GAMES]      = { LANG_IPODUI_X_GAMES, XT_NO_MODULE, 0, UI_T_PUSH, 0,
                        &ui_preview_x_games },
    [XT_NOTES]      = { LANG_IPODUI_X_NOTES, MOD_NOTES, MOD_SLOT_FIRST,
                        UI_T_PUSH, 0, &ui_preview_x_notes },
    [XT_SCREENLOCK] = { LANG_IPODUI_X_SCREEN_LOCK, MOD_LOCKSET,
                        MOD_SLOT_FIRST, UI_T_PUSH, 0,
                        &ui_preview_x_screenlock },
    [XT_STOPWATCH]  = { LANG_IPODUI_X_STOPWATCH, MOD_STOPWATCH,
                        MOD_SLOT_FIRST, UI_T_PUSH, 1, NULL },
};

/* -- the landed Extras (extras_roots.h) ------------------------------------ */
#ifndef IPODUI_MODLOAD
/* a static build links each module's table */
#define XT_ROOT(x, name, panel) extern const void *const mod_exports_##name[];
#include "ipodui/extras_roots.h"
#undef XT_ROOT
#define XT_TAB(name) mod_exports_##name
#else
#define XT_TAB(name) NULL               /* the target reads the pack */
#endif

static const struct {
    int8_t x;
    const void *const *tab;
    const struct ui_preview_ops *panel;
} roots[] = {
#define XT_ROOT(x, name, panel) { x, XT_TAB(name), panel },
#include "ipodui/extras_roots.h"
#undef XT_ROOT
    { -1, NULL, NULL }
};

static int root_of(int x)
{
    unsigned i;

    for (i = 0; i + 1 < ARRAYLEN(roots); i++)
        if (roots[i].x == x)
            return i;
    return -1;
}

const void *const *extras_static_exports(int mod, int *n)
{
    unsigned i;

    for (i = 0; i + 1 < ARRAYLEN(roots); i++)
        if (roots[i].tab && extras_table[roots[i].x].module == mod)
        {
            *n = MOD_SLOT_FIRST + 1;    /* the core uses slots 0 and 1 */
            return roots[i].tab;
        }
    return sdk_games_static_exports(mod, n);    /* W4: SDK games */
}

/* The row's panel: the package's, else the table's, else the plain one */
const struct ui_preview_ops *extras_panel(enum extra_id x)
{
    int r = root_of(x);

    if (r >= 0 && roots[r].panel)
        return roots[r].panel;
    return extras_table[x].panel ? extras_table[x].panel
                                 : &ui_preview_x_plain;
}

bool extras_available(enum extra_id x)
{
    const struct mod_rt *r;

    if ((unsigned)x >= XT__N)
        return false;
    if (x == XT_GAMES)
    {
        /* the resident Games list: offered only if a game can run here
         * (a build without the module arena hides every game, L3; the
         * 7G lists only what the unit has). The Main Menu bit is kept,
         * so the row comes back in a build that runs games. */
        int id;

        for (id = 0; id < MOD__N; id++)
        {
            r = mod_rt_get((enum mod_id)id);
            if (r && r->kind == MOD_KIND_SDK && mod_runnable(r))
                return true;
        }
        return false;
    }
    if (root_of(x) < 0)
        return true;                    /* a placeholder row */
    /* not in the pack yet (or no pack): the row stays, its stand-in shows
     * the styled page if the load fails; unrunnable here: hidden */
    r = mod_rt_get(extras_table[x].module);
    return !r || mod_runnable(r);
}

/* -- open Extras ----------------------------------------------------------- */
/* The Extras whose root is on the stack, newest last: at most two (an
 * Extra opened from inside another, or under a ring screen) */
#define OPEN_MAX 2
static struct { int8_t mod; uint8_t depth; } open_x[OPEN_MAX];
static int n_open;

static void prune(void)
{
    int d = ui_depth();

    while (n_open > 0 && open_x[n_open - 1].depth > d)
        n_open--;
}

int extras_top_module(void)
{
    prune();
    return n_open > 0 ? open_x[n_open - 1].mod : -1;
}

bool extras_module_live(int mod)
{
    int i;

    prune();
    for (i = 0; i < n_open; i++)
        if (open_x[i].mod == mod)
            return true;
    return false;
}

int extras_open(enum extra_id x)
{
    const struct extra_ent *e;
    const struct ui_screen_ops *ops;
    const void *arg;
    int rc, r;

    if ((unsigned)x >= XT__N || !extras_available(x))
        return -1;
    e = &extras_table[x];
    if (x == XT_GAMES)                  /* W4-0: the resident Games list */
        return ui_push_t(&sdk_games_screen, NULL, e->transition);
    r = root_of(x);
    if (r < 0)
    {
        /* not built yet: the empty placeholder with the Extra's title */
        UPDATE_TRACE("extras open %d placeholder", (int)x);
        return ui_push_t(&ui_stub_screen, str(e->title_lang), e->transition);
    }
#ifdef IPODUI_STANDINS
    ops = &mod_stub_extra;              /* swaps to the root when it lands */
    arg = (const void *)(intptr_t)x;
#else
    ops = (const struct ui_screen_ops *)roots[r].tab[e->slot];
    arg = NULL;
#endif
    /* the accountant's visit and entry window open before the root's
     * enter() reads (the main loop's pass would see them too late) */
    modclass_entry(e->module);
    rc = ui_push_t(ops, arg, e->transition);
    UPDATE_TRACE("extras open %d mod=%d rc=%d", (int)x, e->module, rc);
    if (rc >= 0)
    {
        prune();
        if (n_open == OPEN_MAX)
        {
            open_x[0] = open_x[1];
            n_open--;
        }
        open_x[n_open].mod = e->module;
        open_x[n_open].depth = ui_depth();
        n_open++;
    }
    return rc;
}

void extras_preload(enum extra_id x)
{
    if ((unsigned)x < XT__N && root_of(x) >= 0)
        mod_preload_id(extras_table[x].module);
}

/* -- row actions ----------------------------------------------------------- */
#define GO(fn, x) void fn(void) { extras_open(x); }
GO(extras_go_clocks, XT_CLOCKS)
GO(extras_go_calendars, XT_CALENDARS)
GO(extras_go_contacts, XT_CONTACTS)
GO(extras_go_alarms, XT_ALARMS)
GO(extras_go_games, XT_GAMES)
GO(extras_go_notes, XT_NOTES)
GO(extras_go_screenlock, XT_SCREENLOCK)
GO(extras_go_stopwatch, XT_STOPWATCH)

int extras_of_action(void (*fn)(void))
{
    static void (*const go[XT__N])(void) = {
        extras_go_clocks, extras_go_calendars, extras_go_contacts,
        extras_go_alarms, extras_go_games, extras_go_notes,
        extras_go_screenlock, extras_go_stopwatch,
    };
    int i;

    for (i = 0; i < XT__N; i++)
        if (go[i] == fn)
            return i;
    return -1;
}
