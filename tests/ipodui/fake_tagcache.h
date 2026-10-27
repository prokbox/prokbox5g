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

/* Host fakes for tests/ipodui/test_library.c (see fake_tagcache.c) */
#ifndef FAKE_TAGCACHE_H
#define FAKE_TAGCACHE_H
#include <stdbool.h>

struct fake_entry {
    const char *title, *artist, *album, *albumartist, *genre, *composer;
    const char *path;
    long disc, track, length, mtime, lastelapsed, lastoffset;
    bool deleted;
    long playcount;
};

/* Replaces the database (a new generation); 'e' must stay valid. */
void fake_db_load(struct fake_entry *e, int n);
void fake_db_bump(void);                /* a reload: views go stale */
void fake_db_usable(bool on);           /* RAM cache dropped / back */
void fake_db_set_stat(int idx, int tag, long value);
void fake_db_set_flags(int idx, int value);
void fake_play(int idx, unsigned long elapsed);     /* idx < 0: stopped */
int  fake_blocks_used(void);
/* The precompute: its file's directory, the database stamp, requests */
void fake_db_dir(const char *dir);
void fake_db_commit(void);              /* a new commit id */
extern int fake_pre_requests, fake_build_requests;
extern long fake_disk_entries;          /* tagcache_ui_disk_entries() */
extern int fake_allocs;                 /* core_alloc() calls so far */
extern int fake_alloc_fail;             /* the next N core_alloc()s fail */
extern int fake_regrows;                /* audio_buffer_regrow() calls */

extern char fake_last_open[256], fake_last_pre[256], fake_last_pin[256];
extern int fake_numeric_calls, fake_yields, fake_boost, fake_trace_rows;
extern int fake_pl_n, fake_pl[], fake_pl_start, fake_pl_shuffled;
extern unsigned long fake_pl_elapsed;
#endif
