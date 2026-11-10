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
 * iPod UI shell: covers from the stock firmware's cover database as an
 * art source (#225 S3; docs/design/ipod-control-support.md §5).
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
#ifndef IPODUI_SYNCART_H
#define IPODUI_SYNCART_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Read only: nothing here ever writes under iPod_Control. Pure C over
 * open/read/write/lseek/close (the host test, tests/syncart, runs it on
 * real files); the caller (artcache.c, the art builder thread) owns the
 * memory policy, the pacing and the logging of the results.
 *
 * syncart.map (in the art cache's folder; written as .tmp, then
 * safefile_replace()):
 *   512 B header: struct syncart_hdr, then the image-file table
 *   (SYNCART_MAX_FILES names of SYNCART_NAME_MAX bytes)
 *   n entries (struct syncart_ent, 24 B), sorted by key, unique.
 * Its keys are the art keys of the albums whose representative track
 * (the one the lists show the album by) lies under iPod_Control/Music;
 * the join with the song database goes by that track's path. Keyed by
 * album, so any track of the album (Now Playing's) finds the cover. The header names its inputs (the cover database's and
 * the chosen song database's size and CRC of the first and last 8 KB,
 * the library's commit id), so a pass rebuilds it only after a sync or a
 * library change. A map whose song database did not match its cover
 * database (match rate < 50 %) is kept with no entries and the status
 * SYNCART_ST_MISMATCH, so the costly check is not repeated each pass. */

struct syncart_ent {            /* 24 B, sorted by key */
    uint64_t key;               /* the album's art key (artkey.h) */
    uint16_t file;              /* index into the map's file table */
    uint8_t  slot_w, slot_h;    /* the stored slot (picture + padding,
                                   artdb_img_geom()); its stride is
                                   size / (2 * slot_h) pixels */
    uint32_t offset, size;      /* in that image file */
    int16_t  pad_v, pad_h;      /* the stored image's padding */
};

#define SYNCART_MAGIC     0x414e5953u   /* "SYNA" */
#define SYNCART_VERSION   2       /* 2: slot_w/slot_h (were w/2, h/2) */
#define SYNCART_HDR_BYTES 512
#define SYNCART_MAX_FILES 16
#define SYNCART_NAME_MAX  24

enum syncart_status {
    SYNCART_ST_OK = 1,          /* entries (possibly 0) */
    SYNCART_ST_MISMATCH = 2,    /* the song database is not the covers' */
    SYNCART_ST_BAD = 3,         /* the cover database is malformed: no
                                   entries until it changes */
};

struct syncart_hdr {            /* 64 B, then the file table */
    uint32_t magic, version, status, n;
    uint32_t nfile, lib_commit;
    uint32_t art_size, art_crc;     /* the cover database */
    uint32_t db_size, db_crc;       /* the song database used */
    uint32_t db_which;              /* index into the candidate list */
    uint32_t images, matched;       /* the match-rate check's counts */
    uint32_t albums;                /* representatives under Music */
    uint32_t rsv;
    uint32_t hdr_crc;               /* CRC-32 of the 60 bytes above and
                                       the file table */
};

/* Results of syncart_check() and syncart_build() */
enum {
    SYNCART_NONE = 0,           /* no cover database (map removed) */
    SYNCART_SAME = 1,           /* the map holds for these inputs */
    SYNCART_STALE = 2,          /* check: a build is owed (the library
                                   changed, the databases did not) */
    SYNCART_STALE_DBS = 4,      /* check: a build is owed, the databases
                                   changed (or no map): every synced
                                   cover is checked again */
    SYNCART_BUILT = 3,          /* build: a new map is in place */
    SYNCART_ERR = -1,           /* I/O: the old map (if any) stays */
    SYNCART_STOPPED = -2,       /* pace() said stop */
    SYNCART_NOROOM = -3,        /* the block is too small for n */
};

/* All paths are prefixed with this (""; the host test points it at a
 * folder of its own) */
extern const char *syncart_root;

/* FNV-1a 64 of the path with ASCII a-z upper-cased (FAT names are
 * case-blind); 0 becomes 1 */
uint64_t syncart_hash(const char *path);

/* true: the path lies under iPod_Control/Music and not in our own link
 * folder (case-blind): a track a sync app copied in */
bool syncart_synced(const char *path);

/* Cheap (four 8 KB reads at most, into the caller's scratch of at least
 * 8 KB): SYNCART_SAME, SYNCART_STALE, SYNCART_STALE_DBS, or SYNCART_NONE
 * (no cover database or no song database: any map is removed, so lookups
 * find nothing) */
int syncart_check(uint32_t lib_commit, void *scratch, size_t len);

/* One stored row of an image (stride >= w pixels of RGB565 little-endian)
 * as w pixels of 8 bits a channel, in bmp.h struct uint8_rgb's order (blue,
 * green, red, alpha 0xff), the way bmp.c expands a 16-bit BMP: the art
 * builder's decode feeds these to resize.c (the slot's first w pixels; the
 * stride's tail is never shown) */
void syncart_row_rgb(const uint8_t *row, int w, uint8_t *out);

/* the current map's header (scratch: at least SYNCART_HDR_BYTES);
 * false: none or bad */
bool syncart_header(struct syncart_hdr *h, void *scratch, size_t len);

/* bytes syncart_build() needs for n albums */
size_t syncart_need(int n);

/* Builds the map from the albums' representative paths (path_of(ctx, i)
 * into buf, the album's art key into *key; false: skip that album) in
 * blk (syncart_need(n) bytes, the
 * caller's: freed after). pace() false stops it (SYNCART_STOPPED, the
 * old map stays). *st: the status written (may be NULL). */
int syncart_build(uint32_t lib_commit, int n,
                  bool (*path_of)(void *ctx, int i, char *buf, size_t len,
                                  uint64_t *key),
                  void *ctx, void *blk, size_t len, bool (*pace)(void),
                  struct syncart_hdr *st);

/* The cover of the album with this art key: its entry and the image
 * file's full path; false: none (no map, not in it, or a bad entry).
 * Reads the map only (one or two reads: interpolation over the uniform
 * keys). */
bool syncart_lookup(uint64_t key, struct syncart_ent *out,
                    char *file, size_t file_len);

/* The map changed or went (after a build or a check): drops the
 * lookup's cached header */
void syncart_forget(void);

#endif /* IPODUI_SYNCART_H */
