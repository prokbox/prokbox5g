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

/* Host test of the pure part of the Main Menu / Music Menu checklists
 * (apps/ipodui/menucfg_core.h, docs/phase11-brief.md sections 3.4, 4.5):
 * masks in, rows and visible order out. */
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include "ipodui/menucfg.h"
#include "ipodui/menucfg_core.h"

static int fails;

#define CHECK(cond, ...) do { if (!(cond)) { \
    printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); \
    printf("\n"); fails++; } } while (0)

/* the visible bits of a mask, in menu order: what menu.c's visible()
 * chain produces (offered & mask, in ascending bit order) */
static int visible_list(unsigned mask, unsigned offered, int pinned, int *out)
{
    int n = 0, b;

    for (b = 0; b < 32; b++)
        if (((offered >> b) & 1u) && mc_checked(mask, offered, b, pinned))
            out[n++] = b;
    return n;
}

int main(void)
{
    int v[32], n, i, b;
    unsigned m;
    static const int main_canon[] = {
        UI_MM_MUSIC, UI_MM_PLAYLISTS, UI_MM_ARTISTS, UI_MM_ALBUMS,
        UI_MM_COMPILATIONS, UI_MM_SONGS, UI_MM_GENRES, UI_MM_COMPOSERS,
        UI_MM_AUDIOBOOKS, UI_MM_SEARCH, UI_MM_PODCASTS, UI_MM_EXTRAS,
        /* P12: the Extras children that have shipped (wave 1; Contacts,
         * wave 3), in bit order */
        UI_MM_ALARMS, UI_MM_CLOCKS, UI_MM_CONTACTS, UI_MM_NOTES,
        UI_MM_SCREENLOCK, UI_MM_STOPWATCH,
        UI_MM_SETTINGS, UI_MM_SHUFFLE };
    enum { N_MAIN = sizeof(main_canon) / sizeof(main_canon[0]) };
    static const int music_canon[] = {
        UI_MU_PLAYLISTS, UI_MU_ARTISTS, UI_MU_ALBUMS, UI_MU_COMPILATIONS,
        UI_MU_SONGS, UI_MU_GENRES, UI_MU_COMPOSERS, UI_MU_AUDIOBOOKS,
        UI_MU_SEARCH };

    /* offered rows: counts and canonical order (the brief's lists) */
    CHECK(mc_count(UI_MM_OFFERED) == N_MAIN, "main offered %d", mc_count(UI_MM_OFFERED));
    CHECK(mc_count(UI_MU_OFFERED) == 9, "music offered %d", mc_count(UI_MU_OFFERED));
    for (i = 0; i < N_MAIN; i++)
        CHECK(mc_nth_bit(UI_MM_OFFERED, i) == main_canon[i],
              "main row %d: bit %d != %d", i, mc_nth_bit(UI_MM_OFFERED, i),
              main_canon[i]);
    for (i = 0; i < 9; i++)
        CHECK(mc_nth_bit(UI_MU_OFFERED, i) == music_canon[i],
              "music row %d", i);
    CHECK(mc_nth_bit(UI_MM_OFFERED, N_MAIN) == -1, "past the end");
    for (i = 0; i < N_MAIN; i++)
        CHECK(mc_row_of(UI_MM_OFFERED, main_canon[i]) == i, "row_of %d", i);
    CHECK(mc_row_of(UI_MM_OFFERED, UI_MM_COVERFLOW) == -1, "coverflow not offered");
    CHECK(mc_row_of(UI_MM_OFFERED, UI_MM_VIDEOS) == -1, "videos not offered");

    /* the reserved bits are never offered */
    for (b = 0; b < UI_MM_BITS; b++)
        if (b == UI_MM_COVERFLOW || b == UI_MM_GENIUS || b == UI_MM_VIDEOS ||
            b == UI_MM_PHOTOS || b == UI_MM_CALENDARS ||
            b == UI_MM_GAMES)
            CHECK(!((UI_MM_OFFERED >> b) & 1u), "bit %d offered", b);
    CHECK(!((UI_MU_OFFERED >> UI_MU_COVERFLOW) & 1u), "cover flow offered");
    CHECK(!((UI_MU_OFFERED >> UI_MU_GENIUS) & 1u), "genius offered");

    /* defaults: the shipped main menu and the music menu minus
     * Compilations (G12 p.30) */
    n = visible_list(UI_MM_DEFAULT, UI_MM_OFFERED, UI_MM_SETTINGS, v);
    CHECK(n == 5 && v[0] == UI_MM_MUSIC && v[1] == UI_MM_PODCASTS &&
          v[2] == UI_MM_EXTRAS && v[3] == UI_MM_SETTINGS &&
          v[4] == UI_MM_SHUFFLE, "default main menu: %d rows", n);
    n = visible_list(UI_MU_DEFAULT, UI_MU_OFFERED, -1, v);
    CHECK(n == 8, "default music menu: %d rows", n);
    for (i = 0; i < n; i++)
        CHECK(v[i] != UI_MU_COMPILATIONS, "compilations in the default");

    /* a customised main menu takes fixed slots (main-menu-custom-hold.jpg):
     * Audiobooks right under Music, not at the end */
    m = UI_MM_DEFAULT | (1u << UI_MM_AUDIOBOOKS);
    n = visible_list(m, UI_MM_OFFERED, UI_MM_SETTINGS, v);
    CHECK(n == 6 && v[0] == UI_MM_MUSIC && v[1] == UI_MM_AUDIOBOOKS &&
          v[2] == UI_MM_PODCASTS, "audiobooks slot");
    /* Compilations on sits between Albums and Songs */
    m = UI_MU_DEFAULT | (1u << UI_MU_COMPILATIONS);
    n = visible_list(m, UI_MU_OFFERED, -1, v);
    CHECK(n == 9 && v[2] == UI_MU_ALBUMS && v[3] == UI_MU_COMPILATIONS &&
          v[4] == UI_MU_SONGS, "compilations slot");

    /* Settings is pinned: shown even with its bit clear, and a toggle of
     * it changes nothing */
    CHECK(mc_checked(0, UI_MM_OFFERED, UI_MM_SETTINGS, UI_MM_SETTINGS),
          "settings pinned");
    CHECK(mc_toggle(UI_MM_DEFAULT, UI_MM_OFFERED, UI_MM_SETTINGS,
                    UI_MM_SETTINGS) == UI_MM_DEFAULT, "settings toggled");
    n = visible_list(0, UI_MM_OFFERED, UI_MM_SETTINGS, v);
    CHECK(n == 1 && v[0] == UI_MM_SETTINGS, "all off: only Settings");

    /* toggles: on, off again; a bit that is not offered is refused and
     * the reserved bits of the mask survive */
    m = UI_MM_DEFAULT | (1u << UI_MM_VIDEOS);       /* a reserved bit set */
    m = mc_toggle(m, UI_MM_OFFERED, UI_MM_PLAYLISTS, UI_MM_SETTINGS);
    CHECK((m >> UI_MM_PLAYLISTS) & 1u, "toggle on");
    CHECK((m >> UI_MM_VIDEOS) & 1u, "reserved bit kept");
    m = mc_toggle(m, UI_MM_OFFERED, UI_MM_PLAYLISTS, UI_MM_SETTINGS);
    CHECK(!((m >> UI_MM_PLAYLISTS) & 1u), "toggle off");
    CHECK(mc_toggle(m, UI_MM_OFFERED, UI_MM_COVERFLOW, UI_MM_SETTINGS) == m,
          "not offered");
    CHECK(mc_toggle(m, UI_MM_OFFERED, -1, UI_MM_SETTINGS) == m, "bit -1");
    CHECK(mc_toggle(m, UI_MM_OFFERED, 32, UI_MM_SETTINGS) == m, "bit 32");
    /* a reserved bit set in the mask doesn't show (not offered) */
    n = visible_list(1u << UI_MM_VIDEOS, UI_MM_OFFERED, UI_MM_SETTINGS, v);
    CHECK(n == 1, "reserved bit visible");

    /* the setting ranges hold every bit */
    CHECK(UI_MM_MASK == 0x3ffffff, "main mask %x", UI_MM_MASK);
    CHECK(UI_MU_MASK == 0x7ff, "music mask %x", UI_MU_MASK);
    CHECK((UI_MM_DEFAULT & ~UI_MM_MASK) == 0 && (UI_MU_DEFAULT & ~UI_MU_MASK) == 0,
          "defaults inside the ranges");
    CHECK((UI_MM_DEFAULT & ~UI_MM_OFFERED) == 0, "default main row not offered");
    CHECK((UI_MU_DEFAULT & ~UI_MU_OFFERED) == 0, "default music row not offered");

    if (fails)
    {
        printf("%d failure(s)\n", fails);
        return 1;
    }
    printf("menucfg: ok\n");
    return 0;
}
