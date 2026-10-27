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
#ifndef _IPODUI_LIBFILE_H_
#define _IPODUI_LIBFILE_H_

/* The library precompute file (docs/phase4b-brief.md, Decisions 3-6):
 * the shell's list contents, computed once per database load on the
 * tagcache thread (library.c, lib_precompute()) and read by lib_open()
 * instead of scanning the RAM cache. Also read by the art cache builder
 * (the album table), which must not include library.h; so the format
 * lives here, with no dependencies.
 *
 * File: <tagcache db dir>/LIBFILE_NAME, written as LIBFILE_TEMP, then
 * renamed. Native byte order (written and read on the same device).
 *
 *   struct libfile_hdr         (LIBFILE_HDR_SIZE bytes, written last)
 *   sections, each a packed array of 'count' elements of 'elem' bytes,
 *   at 'offset' (a multiple of 4), 'sum' = libfile_sum() of its bytes.
 *
 * A reader trusts nothing but a header whose magic, version, sum and
 * stamp (commit id, entry count and SORTKEY_VERSION of the database in
 * RAM: tagcache_ui_commitid(), tagcache_ui_count()) all match, and then
 * a section whose sum matches. FAT's rename over an existing file is not
 * crash-atomic (firmware/common/file.c), so a torn file is possible and
 * is caught by the sums; the reader then falls back to scanning (and the
 * shell asks for a rebuild: tagcache_ui_precompute_request()).
 *
 * Sections (cat = enum lib_cat of library.h, col = enum libfile_col):
 *   LIB_ARTISTS       ROWS, COUNT (music tracks of the artist),
 *                     ALBUMS (#47: each artist's albums, below)
 *   LIB_ALBUMS        ROWS, COUNT (tracks), KEY, MTIME, TRACKS (#49)
 *   LIB_SONGS         ROWS
 *   LIB_GENRES        ROWS
 *   LIB_COMPOSERS     ROWS
 *   LIB_COMPILATIONS  ROWS, COUNT, KEY, MTIME
 *   LIB_AUDIOBOOKS    ROWS, COUNT (tracks), KEY, MTIME, TRACKS (P6 L2)
 *   LIB_PODCASTS      ROWS, COUNT (episodes), KEY, MTIME
 *   LIBFILE_CAT_ALBUMTAB  col 0: struct libfile_album[], every album,
 *                     book and show (compilations are albums), sorted by
 *                     'rep' (master index order: folder order on disk)
 *   LIBFILE_CAT_PANELSET  col 0: struct libfile_album[], at most
 *                     LIBFILE_PANELS_MAX music albums with a key: the
 *                     main menu's panel art set (H1), chosen at every
 *                     precompute: the most recently played albums, the
 *                     newest (tag_mtime), then random ones (seeded by the
 *                     commit id). Absent in files from before H1 (no
 *                     panel art until the next precompute).
 *                     col 1 (uint32[]): [0] how many of col 0's entries
 *                     (its last ones) are books (#58); then (#100, no
 *                     new section: LIBFILE_MAX_SEC is nearly full) the
 *                     pool order, LIBFILE_POOL_TAG, a count n and n x
 *                     struct libfile_pool: the recently played and newest
 *                     (pinned), then random ones by rendezvous hashing
 *                     with a FIXED seed (stable across commits: the art
 *                     builder redoes few covers), up to LIBFILE_POOL_MUSIC
 *                     music albums, LIBFILE_POOL_BOOKS books and the
 *                     videos' share. A reader of [0] alone (8b) is
 *                     unaffected; a file without the pool is rewritten
 *                     once.
 *   LIBFILE_CAT_BOOKS_AUTHOR  ROWS, COUNT, KEY (#59): the LIB_AUDIOBOOKS
 *                     rows, counts and keys again, ordered by author
 *                     (album artist, else artist), then title: what
 *                     lib_open(LIB_AUDIOBOOKS) lists under Sort By
 *                     Author. Added (LIBFILE_VERSION stays 1); the
 *                     LIB_AUDIOBOOKS sections stay in title order (Search,
 *                     lib_book_tracks() and the album table read them).
 *                     A current file without it is rewritten once.
 *   Phase 14a, video builds only (HAVE_BCMFW_VIDEO, LIBFILE_VERSION 2;
 *   docs/phase14a-brief.md 3.3), 8 more sections, 38 in all:
 *   LIB_MOVIES        ROWS, KEY (title order, sort keys as Songs)
 *   LIB_TVSHOWS       ROWS, COUNT (episodes), KEY, MTIME, TRACKS (each
 *                     show's episodes ordered season, episode, title;
 *                     seasons derived at open from the show's slice)
 *   LIBFILE_CAT_VSTATS  col 0: struct libfile_vstats
 *   The album table gains a row per movie and per show
 *   (LIBFILE_ALB_VIDEO, LIBFILE_ALB_TV). VIDEO rows are in no music
 *   section; video podcast episodes stay in LIB_PODCASTS.
 * ROWS are master indices (int32) in list order: exactly what lib_open()
 * of that category, unfiltered, lists. COUNT is uint32, KEY the
 * artcache_key() (uint64) of the row's grouping strings (0 = none), MTIME
 * the newest tag_mtime of the group's tracks (uint32). */

#include <stdint.h>
#include <stddef.h>

#define LIBFILE_NAME        "database_lib.dat"
#define LIBFILE_TEMP        "database_lib.tmp"
#define LIBFILE_MAGIC       0x424c5049u     /* "IPLB" */
#ifdef HAVE_BCMFW_VIDEO
/* Phase 14a (docs/phase14a-brief.md 3.3): 8 more sections (38 of 48).
 * A video build writes version 2 and a non-video build version 1: a unit
 * switched between builds rewrites the file once each way (accepted). */
#define LIBFILE_VERSION     2
#define LIBFILE_MAX_SEC     48
#define LIBFILE_HDR_BYTES   800     /* 32 + 48 x 16 */
#else
#define LIBFILE_VERSION     1
#define LIBFILE_MAX_SEC     32
#define LIBFILE_HDR_BYTES   544     /* 32 + 32 x 16 */
#endif
/* #47: the grouping rules the rows were built with, in the header's
 * 'reserved' word (readers that predate it ignore it): 1 = case folded
 * through sortkey.h ui_casefold_cmp(), group-key order. A current file
 * with another value is rewritten once, in the background
 * (pre_current()); its lists stay readable meanwhile. Not a
 * LIBFILE_VERSION change (that would fail the #85 generation check). */
#define LIBFILE_GROUPING    1
#define LIBFILE_CAT_ALBUMTAB 0x7f
#define LIBFILE_CAT_PANELSET 0x7e
#define LIBFILE_PANELS_MAX  32
#define LIBFILE_POOL_TAG    0x4c4f4f50u     /* "POOL" */
#define LIBFILE_POOL_MUSIC  100             /* Q9 */
#define LIBFILE_POOL_BOOKS  20
#define LIBFILE_POOL_MAX    128             /* all kinds */
#define LIBFILE_CAT_STATS   0x7d
/* #59: the books again, in author order (an ADDED category; see below) */
#define LIBFILE_CAT_BOOKS_AUTHOR 0x7c
#ifdef HAVE_BCMFW_VIDEO
/* Phase 14a: the video totals (struct libfile_vstats, below) */
#define LIBFILE_CAT_VSTATS  0x7b
#endif

enum libfile_col { LIBFILE_ROWS, LIBFILE_COUNT, LIBFILE_KEY, LIBFILE_MTIME,
                   LIBFILE_TRACKS, LIBFILE_ALBUMS };

/* #49 (Phase 5, Revision 1 item 6): cat LIB_ALBUMS, col LIBFILE_TRACKS,
 * elem 4, an ADDED section (LIBFILE_VERSION stays 1: a reader that does
 * not know it skips it; a file from before Phase 5 lacks it, and the
 * reader then falls back to lib_open(LIB_SONGS, album filter)). Layout,
 * for the A rows of the LIB_ALBUMS ROWS section and T tracks in total:
 *   uint32 start[A + 1]   start[i] = element index (from the section
 *                         start) of album i's first track; start[A] =
 *                         A + 1 + T (the end)
 *   int32  idx[T]         master indices, album by album in ROWS order,
 *                         each album's music tracks in its list order
 *                         (disc, track: what lib_open(LIB_SONGS, album
 *                         filter) lists)
 * count = A + 1 + T. Album i's tracks: start[i + 1] - start[i] (equals
 * its COUNT), read as one 8-byte pair then one slice: two small reads,
 * no scan. The section's sum covers all of it, as for any section.
 * P6 fix L2: LIB_AUDIOBOOKS gets the same section (each book's files in
 * its list order, the books builder's file list: lib_book_tracks()). */
#define LIBFILE_TRACKS_HDR(albums)   ((uint32_t)(albums) + 1u)

/* #47: cat LIB_ARTISTS, col LIBFILE_ALBUMS, elem 4, an ADDED section
 * (LIBFILE_VERSION stays: a version change would also fail the #85
 * generation check, tagcache.c ui_generation_validate(), and rebuild the
 * database; a current file with an album table but without this section
 * is rewritten once instead, pre_current()).
 * What lib_open(LIB_ALBUMS, artist filter) lists for each of the R rows
 * of the LIB_ARTISTS ROWS section, so that opening an artist reads its
 * albums in one range instead of scanning the RAM cache and searching
 * the album table row by row. Layout:
 *   uint32 start[R + 1]   start[i] = element index (from the section
 *                         start) of artist i's first record; start[R] =
 *                         the end (R + 1 + 4 x records)
 *   struct libfile_artalb records, artist by artist in ROWS order, each
 *                         artist's albums in their LIB_ALBUMS list order
 * Artist i: one 8-byte read of start[i], start[i + 1], then one read of
 * its records. Read in pieces, so not checked against the section sum:
 * the reader checks each record against the RAM cache instead. A block
 * without room for the pass writes it empty (count 0); the reader then
 * scans as before. */
struct libfile_artalb {         /* 16 bytes */
    int32_t  rep;               /* the album row's representative */
    uint32_t count;             /* its tracks (the album's COUNT) */
    uint32_t key_lo, key_hi;    /* its KEY */
};
#define LIBFILE_ARTALB_HDR(artists)  ((uint32_t)(artists) + 1u)
#define LIBFILE_ARTALB_WORDS         4u

struct libfile_sec {            /* 16 bytes */
    uint8_t  cat, col;
    uint16_t elem;              /* bytes per element */
    uint32_t count;             /* elements */
    uint32_t offset;            /* from the file start */
    uint32_t sum;               /* libfile_sum() of the section's bytes */
};

struct libfile_hdr {
    uint32_t magic, version;
    uint32_t sortkey_version;   /* SORTKEY_VERSION */
    int32_t  commitid;          /* tagcache_ui_commitid() */
    int32_t  entries;           /* tagcache_ui_count() */
    uint32_t nsec;
    uint32_t sum;               /* libfile_sum() of the header, sum = 0 */
    uint32_t reserved;          /* #47: LIBFILE_GROUPING (0 before) */
    struct libfile_sec sec[LIBFILE_MAX_SEC];
};
#define LIBFILE_HDR_SIZE    sizeof(struct libfile_hdr)  /* LIBFILE_HDR_BYTES */

/* One album, book or show (LIBFILE_CAT_ALBUMTAB): 24 bytes. */
struct libfile_album {
    uint32_t key_lo, key_hi;    /* artcache_key(), split: 4-byte aligned */
    int32_t  rep;               /* representative: master index of the
                                 * group's first track (folder order) */
    uint32_t count;             /* tracks */
    uint32_t mtime;             /* newest tag_mtime of its tracks */
    uint32_t flags;             /* LIBFILE_ALB_* */
};
/* #100: an entry of the pool order (LIBFILE_CAT_PANELSET col 1) */
struct libfile_pool {
    struct libfile_album a;
    uint32_t pinned;            /* 1: recently played or newest */
};
#define LIBFILE_ALB_BOOK     0x01
#define LIBFILE_ALB_PODCAST  0x02
#define LIBFILE_ALB_DIRKEY   0x04   /* untagged album: key from the folder */
#ifdef HAVE_BCMFW_VIDEO
/* Phase 14a (3.3, 3.5): a movie (key from its path: its own group, as an
 * untagged book) or a TV show (VIDEO | TV, key from the show string). The
 * art cache's ARTCACHE_PSET_VIDEOS selects rows with LIBFILE_ALB_VIDEO. */
#define LIBFILE_ALB_VIDEO    0x08
#define LIBFILE_ALB_TV       0x10
#endif

/* P11 (docs/phase11-brief.md 4.3): LIBFILE_CAT_STATS, col 0, one element:
 * the library's totals for About, from the RAM database at precompute
 * time (no file reads). An ADDED section (LIBFILE_VERSION stays 1); a
 * current file without it (written before P11) is rewritten once. Every
 * class counts toward audio_mib (songs, episodes, book files). */
struct libfile_stats {          /* 16 bytes */
    int32_t  songs, episodes, books;    /* tracks (files) of each class */
    uint32_t audio_mib;         /* sum(bitrate kbit/s x length ms) >> 23 */
};

#ifdef HAVE_BCMFW_VIDEO
/* Phase 14a (docs/phase14a-brief.md 3.3, 3.9): LIBFILE_CAT_VSTATS, col 0,
 * one element, from the RAM database at precompute time (V3, library.c).
 * videos = movies + tv: About's Videos row (owner T5, 2026-10-12). Video
 * podcast episodes count under Podcasts only (libfile_stats.episodes),
 * never in both rows; vpodcasts is kept separately for logs. video_mib
 * uses the audio_mib formula over EVERY VIDEO row, video podcasts
 * included (storage: the capacity bar's Video segment; the mp4 bitrate
 * comes from the media data size, so it is about the file size);
 * libfile_stats.audio_mib then excludes VIDEO rows. A video build's file
 * always has it. */
struct libfile_vstats {         /* 20 bytes */
    int32_t  videos;            /* movies + tv (About page 2 Videos) */
    int32_t  tv, movies;        /* files of each kind */
    int32_t  vpodcasts;         /* video podcast episodes (logs only) */
    uint32_t video_mib;         /* sum(bitrate kbit/s x length ms) >> 23 */
};
#endif

/* Checksum: Fletcher-like over 32-bit words (~1 cycle/byte on the
 * PP5022; the files are a few hundred KB and read whole). Feed whole
 * sections (multiples of 4 bytes) through libfile_sum_add(), possibly in
 * pieces; libfile_sum_end() gives the value. */
struct libfile_sum_state { uint32_t a, b; };

static inline void libfile_sum_init(struct libfile_sum_state *s)
{
    s->a = 1;
    s->b = 0;
}

static inline void libfile_sum_add(struct libfile_sum_state *s,
                                   const void *p, size_t bytes)
{
    const uint32_t *w = (const uint32_t *)p;
    uint32_t a = s->a, b = s->b;
    size_t n = bytes / 4;

    while (n--)
    {
        a += *w++;
        b += a;
    }
    s->a = a;
    s->b = b;
}

static inline uint32_t libfile_sum_end(const struct libfile_sum_state *s)
{
    return s->a ^ (s->b * 0x9e3779b1u);
}

#endif /* _IPODUI_LIBFILE_H_ */
