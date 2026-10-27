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
#ifndef IPODUI_MENUCFG_H
#define IPODUI_MENUCFG_H

#include <stdbool.h>

/* Phase 11 Settings > Main Menu / Music Menu checklists
 * (docs/phase11-brief.md sections 3.4, 4.5). Contract, committed before
 * fan-out; menucfg.c (U2a) implements the functions. The masks are the
 * settings ipodui_main_menu / ipodui_music_menu (settings_list.c, which
 * includes this header for the defaults).
 *
 * The bits are persisted in config.cfg: FROZEN, APPEND-ONLY. Never
 * reorder or reuse one. */
enum ui_mm_bit { UI_MM_MUSIC, UI_MM_COVERFLOW, UI_MM_PLAYLISTS, UI_MM_ARTISTS,
    UI_MM_ALBUMS, UI_MM_COMPILATIONS, UI_MM_SONGS, UI_MM_GENRES, UI_MM_COMPOSERS,
    UI_MM_AUDIOBOOKS, UI_MM_SEARCH, UI_MM_GENIUS, UI_MM_VIDEOS, UI_MM_PHOTOS,
    UI_MM_PODCASTS, UI_MM_EXTRAS, UI_MM_ALARMS, UI_MM_CALENDARS, UI_MM_CLOCKS,
    UI_MM_CONTACTS, UI_MM_GAMES, UI_MM_NOTES, UI_MM_SCREENLOCK, UI_MM_STOPWATCH,
    UI_MM_SETTINGS, UI_MM_SHUFFLE, UI_MM_BITS };
enum ui_mu_bit { UI_MU_COVERFLOW, UI_MU_GENIUS, UI_MU_PLAYLISTS, UI_MU_ARTISTS,
    UI_MU_ALBUMS, UI_MU_COMPILATIONS, UI_MU_SONGS, UI_MU_GENRES, UI_MU_COMPOSERS,
    UI_MU_AUDIOBOOKS, UI_MU_SEARCH, UI_MU_BITS };

/* Defaults (section 3.4): Main = today's main menu; Music = everything but
 * Compilations (off per G12 p.30, ⚠️ C4). */
#define UI_MM_DEFAULT_BASE ((1u<<UI_MM_MUSIC)|(1u<<UI_MM_PODCASTS)|(1u<<UI_MM_EXTRAS)| \
                       (1u<<UI_MM_SETTINGS)|(1u<<UI_MM_SHUFFLE))
#define UI_MU_DEFAULT ((1u<<UI_MU_PLAYLISTS)|(1u<<UI_MU_ARTISTS)|(1u<<UI_MU_ALBUMS)| \
                       (1u<<UI_MU_SONGS)|(1u<<UI_MU_GENRES)|(1u<<UI_MU_COMPOSERS)| \
                       (1u<<UI_MU_AUDIOBOOKS)|(1u<<UI_MU_SEARCH))
/* The rows offered (shipped screens only, ⚠️ C1; compile-time constants,
 * owner section 1.2): Main = MUSIC..COMPOSERS except COVERFLOW, then
 * AUDIOBOOKS, SEARCH, PODCASTS, EXTRAS, SETTINGS, SHUFFLE. Music =
 * PLAYLISTS..SEARCH. Cover Flow, Genius, Videos, Photos and the Extras
 * children keep their bits and are offered when their phase ships (the
 * Extras children: UI_MM_EXTRAS_LANDED). */
#define UI_MM_OFFERED_BASE ((1u<<UI_MM_MUSIC)|(1u<<UI_MM_PLAYLISTS)|(1u<<UI_MM_ARTISTS)| \
                       (1u<<UI_MM_ALBUMS)|(1u<<UI_MM_COMPILATIONS)|(1u<<UI_MM_SONGS)| \
                       (1u<<UI_MM_GENRES)|(1u<<UI_MM_COMPOSERS)|(1u<<UI_MM_AUDIOBOOKS)| \
                       (1u<<UI_MM_SEARCH)|(1u<<UI_MM_PODCASTS)|(1u<<UI_MM_EXTRAS)| \
                       (1u<<UI_MM_SETTINGS)|(1u<<UI_MM_SHUFFLE)| \
                       UI_MM_EXTRAS_LANDED)
/* P12: the Extras children that have shipped (wave 1, and Contacts in
 * wave 3; Calendars and Games follow); offered, off by default (stock) */
#define UI_MM_EXTRAS_LANDED ((1u<<UI_MM_ALARMS)|(1u<<UI_MM_CLOCKS)| \
                       (1u<<UI_MM_CONTACTS)| \
                       (1u<<UI_MM_NOTES)|(1u<<UI_MM_SCREENLOCK)| \
                       (1u<<UI_MM_STOPWATCH))
/* Phase 14a (docs/phase14a-brief.md 3.6, T1): a video build offers
 * Videos and turns it on by default (stock 7G); a saved mask from before
 * keeps it off until it is ticked in Settings > Main Menu.
 * p19-bcm-release: a release build carries the player for the BCM path
 * (--bcmfw-video without --bcmfw-devtest) but keeps Videos hidden, its
 * bit and screens kept and not offered, like Photos (CLAUDE.md: hide them,
 * don't make them impossible); the developer build and the simulator
 * offer it as before. */
#if defined(HAVE_BCMFW_VIDEO) && \
    (defined(SIMULATOR) || defined(HAVE_BCMFW_DEVTEST))
#define UI_MM_DEFAULT (UI_MM_DEFAULT_BASE | (1u<<UI_MM_VIDEOS))
#define UI_MM_OFFERED (UI_MM_OFFERED_BASE | (1u<<UI_MM_VIDEOS))
#else
#define UI_MM_DEFAULT UI_MM_DEFAULT_BASE
#define UI_MM_OFFERED UI_MM_OFFERED_BASE
#endif
#define UI_MU_OFFERED ((1u<<UI_MU_PLAYLISTS)|(1u<<UI_MU_ARTISTS)|(1u<<UI_MU_ALBUMS)| \
                       (1u<<UI_MU_COMPILATIONS)|(1u<<UI_MU_SONGS)|(1u<<UI_MU_GENRES)| \
                       (1u<<UI_MU_COMPOSERS)|(1u<<UI_MU_AUDIOBOOKS)|(1u<<UI_MU_SEARCH))
/* The settings' ranges (settings_list.c): every bit, offered or not */
#define UI_MM_MASK    ((1u<<UI_MM_BITS) - 1)        /* 0x3ffffff */
#define UI_MU_MASK    ((1u<<UI_MU_BITS) - 1)        /* 0x7ff */

bool ui_mm_visible(enum ui_mm_bit b);    /* setting & offered; SETTINGS always */
bool ui_mu_visible(enum ui_mu_bit b);

#endif /* IPODUI_MENUCFG_H */
