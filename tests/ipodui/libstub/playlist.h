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

/* host stub for tests/ipodui/test_library.c: records what is inserted
 * (fake_tagcache.c) */
#ifndef STUB_PLAYLIST_H
#define STUB_PLAYLIST_H
#include <stdbool.h>

#define PLAYLIST_INSERT_LAST (-3)

struct playlist_info {
    int max_playlist_size;
    unsigned long created_tick;
};
struct playlist_insert_context {
    int count;
};

struct playlist_info *playlist_get_current(void);
int  playlist_amount(void);
int  playlist_create(const char *dir, const char *file);
int  playlist_insert_context_create(struct playlist_info *playlist,
                                    struct playlist_insert_context *context,
                                    int position, bool queue, bool progress);
int  playlist_insert_context_add(struct playlist_insert_context *context,
                                 const char *filename);
void playlist_insert_context_release(struct playlist_insert_context *c);
int  playlist_shuffle(int random_seed, int start_index);
void playlist_start(int start_index, unsigned long elapsed,
                    unsigned long offset);
#endif
