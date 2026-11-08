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
 * iPod UI shell: the Apple fallback's pure half (#137): the music
 * database writer and verifier for Apple firmware 1.3, the link verify
 * rule, the park cycle's plan and the link table's records. Host-tested
 * in tests/applefb; no file I/O, no allocation. Written from format
 * descriptions and our own silicon tests (docs/research/itunes-mode.md);
 * no Apple code.
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
#ifndef IPODUI_AFB_CORE_H
#define IPODUI_AFB_CORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* -- paths (the Apple side's, and ours in the hidden folder) ------------ */
#define AFB_MUSIC_DIR   "/iPod_Control/Music"
#define AFB_LINK_DIR    "/iPod_Control/Music/PKB"
#define AFB_ITUNES_DIR  "/iPod_Control/iTunes"
#define AFB_ITDB        AFB_ITUNES_DIR "/iTunesDB"
#define AFB_PLAYCOUNTS  AFB_ITUNES_DIR "/Play Counts"
#define AFB_LOC_MAX     260             /* a location, UTF-8 */

#define AFB_MAC_EPOCH   2082844800u     /* 1904-01-01 to 1970-01-01, s */
#define ITDB_VERSION    0x13            /* the one version proven [silicon] */

/* -- what Apple firmware 1.3 plays --------------------------------------- */
enum afb_kind { AFB_K_NONE = 0, AFB_K_MP3, AFB_K_M4A, AFB_K_M4B, AFB_K_WAV,
                AFB_K_AIF, AFB_K__N };
/* by the name's extension (MP3, M4A/AAC/ALAC, M4B, WAV, AIF/AIFF) */
enum afb_kind afb_kind_of(const char *path);
/* the file type word (as stored, little-endian) and the kind text */
uint32_t afb_ftype(enum afb_kind k);
const char *afb_kind_text(enum afb_kind k);
/* 1 audio, 8 audiobook */
uint32_t afb_media(enum afb_kind k);
/* "P0000042.MP3": the link's 8.3 name for link number no */
void afb_link_name(char *buf, size_t len, uint32_t no, enum afb_kind k);
/* "/a/b c.mp3" -> ":a:b c.mp3" (the location string); false: too long */
bool afb_location(char *buf, size_t len, const char *path);
/* true: the path lies in our link folder (never indexed, never linked) */
bool afb_in_link_dir(const char *path);
/* true: the path is under iPod_Control/Music (Apple plays it in place) */
bool afb_in_music_dir(const char *path);

/* -- the database writer (streamed) ------------------------------------- */
struct afb_out {
    void *ctx;
    /* append n bytes; < 0 error */
    int (*write)(void *ctx, const void *p, uint32_t n);
    /* overwrite n bytes at off (already written); < 0 error */
    int (*patch)(void *ctx, uint32_t off, const void *p, uint32_t n);
};

struct itdb_track {
    const char *title;          /* never empty (the file name stands in) */
    const char *location;       /* ":iPod_Control:Music:PKB:P0000001.MP3" */
    const char *album, *artist, *genre;     /* "" or NULL: no string */
    uint64_t dbid;
    uint32_t size, ms, track, year, kbps, sr;
    uint8_t  vbr;
    uint8_t  kind;              /* enum afb_kind */
};

struct itdb_w {
    const struct afb_out *o;
    uint32_t pos, n, first_id, ts;
    uint64_t pl_id;
    int err;
};

/* header, the track set's heads; ids are first_id + i in order */
int itdb_begin(struct itdb_w *w, const struct afb_out *o, uint32_t ts_mac,
               uint64_t db_id, uint64_t pl_id, uint32_t first_id);
int itdb_add(struct itdb_w *w, const struct itdb_track *t);
/* the master playlist (named plname) with every track, then the patched
 * lengths and counts; the database's length, or < 0 */
long itdb_end(struct itdb_w *w, const char *plname);

/* -- the verifier (re-parse) -------------------------------------------- */
struct afb_in {
    void *ctx;
    /* read n bytes at off; < 0 error (short reads are errors) */
    int (*read)(void *ctx, uint32_t off, void *p, uint32_t n);
};

struct itdb_stats {
    uint32_t version, tracks, items, playlists;
    uint32_t min_id, max_id;
};
enum {
    ITDB_OK = 0, ITDB_E_IO = -1, ITDB_E_MAGIC = -2, ITDB_E_LEN = -3,
    ITDB_E_TAG = -4, ITDB_E_NEST = -5, ITDB_E_COUNT = -6, ITDB_E_ID = -7,
    ITDB_E_VERSION = -8,
};
/* Every chunk's lengths nest exactly in a file of len bytes, the header
 * says len, the counts agree, every playlist item names a track id. Any
 * version is read; only ITDB_VERSION passes when strict. */
int itdb_verify(const struct afb_in *in, uint32_t len, bool strict,
                struct itdb_stats *st);

/* -- the link verify rule (docs/research/apple-fallback-links.md 5) ----- */
enum afb_verdict { AFB_V_OK = 0, AFB_V_ORPHAN, AFB_V_EMPTIED, AFB_V_STALE,
                   AFB_V_FREED, AFB_V_DAMAGED, AFB_V_LINK_GONE, AFB_V__N };
struct afb_facts {
    bool orig_exists;
    uint32_t orig_fst, orig_size;
    bool orig_healthy;          /* chain allocated, ends at ceil(size/cl) */
    bool orig_eq_runs;          /* and is exactly the recorded runs */
    bool runs_free;             /* every recorded cluster is free now */
    bool runs_known;            /* the record holds its runs */
    bool link_exists;
    uint32_t link_fst, link_size;
    uint32_t rec_fst, rec_size;
};
enum afb_verdict afb_classify(const struct afb_facts *f);
const char *afb_verdict_name(enum afb_verdict v);

/* -- the park cycle (state in the hidden folder; links in AFB_LINK_DIR) - */
enum afb_state { AFB_ST_NONE = 0, AFB_ST_UNPARKING = 1, AFB_ST_LIVE = 2 };
enum afb_when { AFB_AT_BOOT = 0, AFB_AT_USB = 1, AFB_AT_OWED = 2 };
enum {
    AFB_DO_VERIFY   = 1 << 0,   /* classify every record; repair what may be */
    AFB_DO_PARK     = 1 << 1,   /* drop the link folder (no chain freed) */
    AFB_DO_UNPARK   = 1 << 2,   /* links again from the originals, table, DB */
    AFB_DO_TEARDOWN = 1 << 3,   /* switch off: state NONE after the park */
};
struct afb_situation {
    bool on;                    /* the developer switch */
    uint8_t state;              /* enum afb_state, as the state file says */
    bool links_present;         /* the link folder exists */
    uint8_t when;               /* enum afb_when */
    bool fp_same;               /* the volume's fingerprint is as recorded */
};
/* what to do now, AFB_DO_* bits in order verify, park, unpark */
unsigned afb_plan(const struct afb_situation *s);

/* -- records (the link table; one per linked song) ---------------------- */
#define AFB_MAXRUNS     8       /* more fragments: not repairable (FREED) */
#define AFB_TBL_MAGIC   0x54424641u     /* "AFBT" */
#define AFB_ST_MAGIC    0x53424641u     /* "AFBS" */
#define AFB_FMT         1

struct afb_tbl_hdr {
    uint32_t magic, fmt, count, rsv;
};
struct afb_rec {
    uint32_t fst, size, linkno;
    uint16_t pathlen;           /* path bytes after the runs, no NUL */
    uint8_t  nruns;             /* 0..AFB_MAXRUNS; 0xff: not recorded */
    uint8_t  kind;              /* enum afb_kind */
    uint32_t runs[2 * AFB_MAXRUNS];
};
/* bytes the record takes in the table (header + runs + path) */
uint32_t afb_rec_bytes(const struct afb_rec *r);
/* encode into buf (afb_rec_bytes(r) bytes) */
void afb_rec_put(uint8_t *buf, const struct afb_rec *r, const char *path);
/* decode from avail bytes; the record's length or 0 (bad, or short:
 * *need says how many it takes); *path points into buf */
uint32_t afb_rec_get(const uint8_t *buf, uint32_t avail, struct afb_rec *r,
                     const char **path, uint32_t *need);

/* the state file */
struct afb_stfile {
    uint32_t magic, fmt;
    uint32_t state;             /* enum afb_state */
    uint32_t count;             /* links made */
    uint32_t fp_free, fp_next;  /* FSInfo when the links were made */
    uint32_t sig;               /* the song set's signature (DB unchanged) */
    uint32_t db_len;            /* the DB we wrote last (0 none) */
};

/* FNV-1a, for the song set's signature */
uint32_t afb_hash(uint32_t h, const void *p, size_t n);
#define AFB_HASH0 2166136261u

#endif /* IPODUI_AFB_CORE_H */
