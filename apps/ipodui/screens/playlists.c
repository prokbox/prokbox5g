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
 * iPod UI shell: Playlists (Music > Playlists)
 *
 *
 * Rows: "On-The-Go" first (~, as the 7G keeps it with the user's lists),
 * then every .m3u / .m3u8 file directly under /Playlists, by name without
 * the extension, sorted case-insensitively. The directory is read once
 * when the screen opens (and again after On-The-Go's Save Playlist added
 * a file, ui_pl_changed()), into the screen's state (names packed, at
 * most PL_MAX files / PL_BYTES of names), never while scrolling.
 * Selecting a playlist replaces the current playlist with it and plays it
 * from the top; selecting On-The-Go opens its list (otglist.c).
 *
 * On-The-Go is its own store since P11 (otg.h): adding to it never
 * touches what plays (⚠️ H3).
 *
 * State: the screen arena (P11, screen_stack.h).
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
#include "string-extra.h"
#include "file.h"
#include "system.h"
#include "kernel.h"
#include "lcd.h"
#include "lang.h"
#include "dir.h"
#include "playlist.h"
#include "logf.h"
#include "ipodui/ipodui.h"
#include "ipodui/theme.h"
#include "ipodui/list.h"
#include "ipodui/screens/screens.h"
#include "ipodui/safefile.h"

#define PL_MAX      64          /* files listed */
#define PL_BYTES    2048        /* their names */

struct playlists {
    struct ui_list list;
    int  n;                     /* files */
    short off[PL_MAX];          /* name offsets into names[], sorted */
    char names[PL_BYTES];
};
/* P11 screen arena bound (screen_stack.h) */
_Static_assert(sizeof(struct playlists) <= UI_ARENA_B_PLAYLISTS, "playlists: arena bound");

static bool pl_stale;           /* ui_pl_changed(): re-read on the next
                                   full draw */

static struct playlists *state(struct ui_screen *s)
{
    return s->priv;
}

static bool is_playlist(const char *name)
{
    const char *dot = strrchr(name, '.');
    return name[0] != '.' && dot &&
           (!strcasecmp(dot, ".m3u") || !strcasecmp(dot, ".m3u8"));
}

int ui_pl_read_dir(short *off, char *names, int max, int bytes)
{
    static bool recovered;
    DIR *dir;
    struct dirent *e;
    int n = 0, used = 0, i, j;

    if (!recovered)
    {
        /* #91: a cut save was finished from its marker at boot
         * (playlist_init()); a "<name>_temp" left here is stray and goes,
         * never rolled forward (L1); once per boot, before the listing */
        recovered = true;
        safefile_recover_dir(UI_PL_DIR, "_temp", SAFEFILE_PL_INTENT);
    }
    dir = opendir(UI_PL_DIR);

    if (!dir)
        return 0;               /* no folder: no playlists */
    while ((e = readdir(dir)) && n < max)
    {
        struct dirinfo info = dir_get_info(dir, e);
        int len = strlen(e->d_name) + 1;

        if ((info.attribute & ATTR_DIRECTORY) || !is_playlist(e->d_name))
            continue;
        if (used + len > bytes)
        {
            logf("ipodui: playlists: listing full");
            break;
        }
        memcpy(names + used, e->d_name, len);
        off[n++] = used;
        used += len;
    }
    closedir(dir);

    /* insertion sort, case-insensitive (a few dozen names) */
    for (i = 1; i < n; i++)
    {
        short x = off[i];
        for (j = i - 1; j >= 0 && strcasecmp(names + off[j], names + x) > 0;
             j--)
            off[j + 1] = off[j];
        off[j + 1] = x;
    }
    return n;
}

void ui_pl_changed(void)
{
    pl_stale = true;
}

bool ui_pl_play(const char *name)
{
    if (playlist_create(UI_PL_DIR, name) < 0 || playlist_amount() <= 0)
    {
        logf("ipodui: playlists: %s empty or unreadable", name);
        return false;
    }
    playlist_start(0, 0, 0);
    ui_show_wps();
    return true;
}

static const char *pl_label(int row, void *data, char *buf, size_t len)
{
    struct playlists *p = data;
    char *dot;

    if (row == 0)
        return str(LANG_IPODUI_ON_THE_GO);
    strlcpy(buf, p->names + p->off[row - 1], len);
    dot = strrchr(buf, '.');
    if (dot)
        *dot = '\0';
    return buf;
}

static bool pl_chevron(int row, void *data)
{
    (void)data;
    return row == 0;            /* On-The-Go opens its list */
}

static int pl_enter(struct ui_screen *s, const void *arg)
{
    const struct ui_theme *t = ui_theme;
    struct playlists *p = ui_screen_alloc(s, sizeof(struct playlists));
    struct viewport vp;

    if (!p)
    {
        logf("ipodui: playlists: no memory");
        return -1;
    }
    s->title = arg;
    pl_stale = false;
    p->n = ui_pl_read_dir(p->off, p->names, PL_MAX, PL_BYTES);

    memset(&vp, 0, sizeof(vp));
    vp.y = t->header_h;
    vp.width = LCD_WIDTH;
    vp.height = LCD_HEIGHT - t->header_h;
    ui_list_init(&p->list, &vp, p->n + 1, pl_label, p);
    p->list.has_chevron = pl_chevron;
    return 0;
}

static void pl_leave(struct ui_screen *s)
{
    ui_screen_free(s);
}

static void pl_draw(struct ui_screen *s, unsigned dirty)
{
    struct playlists *p = state(s);
    int y[UI_BANDS_MAX], h[UI_BANDS_MAX];

    if (dirty & UI_DIRTY_BODY)
    {
        if (pl_stale)           /* back from On-The-Go's Save Playlist */
        {
            pl_stale = false;
            p->n = ui_pl_read_dir(p->off, p->names, PL_MAX, PL_BYTES);
            ui_list_set_count(&p->list, p->n + 1);
        }
        ui_list_draw(&p->list, p->list.vp.y, p->list.vp.height);
    }
    else if (dirty & UI_DIRTY_BAND)
    {
        int i, n = ui_dirty_bands(y, h);
        for (i = 0; i < n; i++)
            ui_list_draw(&p->list, y[i], h[i]);
    }
}

static int pl_tick(struct ui_screen *s)
{
    return ui_list_tick(&state(s)->list);
}

static enum ui_result pl_input(struct ui_screen *s, const struct ui_input *in)
{
    struct playlists *p = state(s);
    char name[MAX_PATH];
    int y[2], h[2], i, n;

    switch (in->key)
    {
        case UI_KEY_WHEEL:
            n = ui_list_move(&p->list, in->wheel_steps, y, h);
            for (i = 0; i < n; i++)
                ui_invalidate_band(y[i], h[i]);
            return UI_HANDLED;

        case UI_KEY_SELECT:
            /* a hold does nothing here: a playlist file has no "Add to
             * On-The-Go" until the store can take a file (otg.h) */
            if (!in->release || in->long_press)
                return UI_HANDLED;
            if (p->list.selected == 0)
                ui_push(&ui_otg_screen, str(LANG_IPODUI_ON_THE_GO));
            else
            {
                /* copy out: playing allocates and yields */
                strlcpy(name, p->names + p->off[p->list.selected - 1],
                        sizeof(name));
                ui_pl_play(name);
            }
            return UI_HANDLED;

        default:
            return UI_IGNORED;
    }
}

const struct ui_screen_ops ui_playlists_screen = {
    .name    = "playlists",
    .enter   = pl_enter,
    .leave   = pl_leave,
    .draw    = pl_draw,
    .input   = pl_input,
    .timeout = NULL,
    .tick    = pl_tick,
};
