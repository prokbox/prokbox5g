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

/* host stub for tests/ipodui/test_library.c: the tagcache API library.c
 * uses, implemented over an in-memory database by fake_tagcache.c */
#ifndef STUB_TAGCACHE_H
#define STUB_TAGCACHE_H
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

enum tag_type {
    tag_artist, tag_album, tag_genre, tag_title, tag_filename, tag_composer,
    tag_albumartist,
    /* numeric */
    tag_discnumber, tag_tracknumber, tag_length, tag_mtime,
    tag_lastelapsed, tag_lastoffset, tag_playcount, tag_lastplayed,
    tag_bitrate,                /* shared search/statistics host harness */
    TAG_COUNT
};

#define TAGCACHE_UI_AUDIOBOOK   0x0020
#define TAGCACHE_UI_PODCAST     0x0040
#define TAGCACHE_UI_COMPILATION 0x0080
#define TAGCACHE_UI_VIDEO       0x0100
#define TAGCACHE_UI_TV          0x0200
#define TAGCACHE_UI_VIDEO_OTHER 0x1000
#define TAGCACHE_UI_ANY_VIDEO   (TAGCACHE_UI_VIDEO | TAGCACHE_UI_VIDEO_OTHER)
#define TAGCACHE_UI_VIEW_BYTES  12

struct tagcache_search {
    int type;
};
struct tagcache_context;
bool tagcache_search_ctx(struct tagcache_context *ctx,
                         struct tagcache_search *tcs, int tag);

bool tagcache_is_initialized(void);
int  tagcache_get_commit_step(void);
bool tagcache_search(struct tagcache_search *tcs, int tag);
void tagcache_search_finish(struct tagcache_search *tcs);
bool tagcache_retrieve(struct tagcache_search *tcs, int idxid, int tag,
                       char *buf, long size);

int  tagcache_ui_generation(void);
bool tagcache_ui_busy(void);
int  tagcache_ui_count(void);
const int32_t *tagcache_ui_seeks(int idx_id, int32_t *flag);
long tagcache_ui_numeric(int idx_id, int tag);
const char *tagcache_ui_str(int tag, int32_t seek);
bool tagcache_ui_walk(int tag, int32_t *pos, int32_t *idx_id);
int32_t tagcache_ui_commitid(void);
const char *tagcache_ui_db_path(void);
void tagcache_ui_precompute_request(void);
long tagcache_ui_disk_entries(void);
struct tagcache_context;
struct tagcache_context *tagcache_context_active(void);
struct tagcache_context *tagcache_context_acquire(void);
void tagcache_context_release(struct tagcache_context *ctx);
bool tagcache_context_published(struct tagcache_context *ctx);
int tagcache_ui_generation_ctx(struct tagcache_context *ctx);
int tagcache_ui_count_ctx(struct tagcache_context *ctx);
int32_t tagcache_ui_commitid_ctx(struct tagcache_context *ctx);
const char *tagcache_ui_db_path_ctx(struct tagcache_context *ctx);
const int32_t *tagcache_ui_seeks_ctx(struct tagcache_context *ctx,
                                   int idx_id, int32_t *flag);
long tagcache_ui_numeric_ctx(struct tagcache_context *ctx, int idx_id, int tag);
bool tagcache_ui_walk_ctx(struct tagcache_context *ctx, int tag,
                         int32_t *pos, int32_t *idx_id);
const char *tagcache_ui_str_ctx(struct tagcache_context *ctx,
                               int tag, int32_t seek);
#endif
