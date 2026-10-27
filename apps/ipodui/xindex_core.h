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
 * iPod UI shell: sync-time indexes for Notes, Contacts, Calendars (P12
 * contract, docs/phase12-brief.md 3.5). Pure, host-tested in tests/xindex;
 * format owner W1-N
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
#ifndef IPODUI_XINDEX_CORE_H
#define IPODUI_XINDEX_CORE_H

/* One file per source folder (Q12: /Notes, /Contacts, /Calendars), kept
 * in the hidden system folder (never named on screen). Built by the
 * MOD_XINDEX module on the storage worker, in the post-USB moment (#84)
 * and the boot window (#87), only while playback is stopped, inside one
 * maint_take() block of XIDX_BUILD_BLK; the module is pinned (mod_pin)
 * for the job. A boot the boot stamp trusts as clean skips the signature
 * check. Browsing reads one prepared blob: zero directory reads.
 *
 * File: struct xidx_hdr, then at rec_off the kind's body, then
 * str_bytes of NUL-terminated UTF-8 strings at str_off. Calendars: count
 * records of the kind's size, then a per-day bitmap of the window; every
 * *_off in a record is an offset into the string block. Notes and
 * Contacts are paged lists (below): rec_off holds the kind's table, the
 * page directories and the pages, and a record's string offsets are
 * relative to its page. Little-endian. */

#include <stdint.h>

#define XIDX_MAGIC      0x58444958u     /* "XIDX" */
#define XIDX_VERSION    2               /* 2: paged Notes (W3-K). A file of
                                           another version fails
                                           xidx_check(), so the builder's
                                           signature test rebuilds it and a
                                           trusted boot owes the build
                                           (xindex.c) */
#define XIDX_FILE_MAX   (64*1024)       /* Calendars */
#define XIDX_NOTES_FILE_MAX (96*1024)   /* Notes: the pages, then the full
                                           names of the rows whose label is
                                           cut (only the file is this big,
                                           never a reader's RAM) */
#define XIDX_CONTACTS_FILE_MAX (256*1024)   /* Contacts: about 2,000
                                           typical cards (xcontact_core.h);
                                           paged, so only the file is this
                                           big, never a reader's RAM */
#define XIDX_BUILD_BLK  (520*1024)      /* the maint_take() block (sync
                                           time, stopped or the paused
                                           window): Contacts' cards, pool,
                                           scratch and the index being
                                           built (screens/xindex.c C_*);
                                           Notes uses its first 96 KB */
#define XNOTE_SHOW_MAX  4096            /* a note shows its first 4 KB */
#define XCAL_MAX_INSTANCES 2048         /* expanded events; a cut is logged
                                           "xcal cut n=" */
#define XCAL_WINDOW_BACK_MON  1         /* [today - 1 month, */
#define XCAL_WINDOW_AHEAD_MON 12        /*  today + 12 months] */

enum xidx_kind { XIDX_NOTES = 1, XIDX_CONTACTS = 2, XIDX_CAL = 3 };

struct xidx_hdr {
    uint32_t magic; uint16_t version, kind;
    uint32_t sig;                       /* FNV-1a over the folder's
                                           (name, size, mtime) list */
    uint32_t count, rec_off, str_off, str_bytes;
    uint32_t crc;                       /* crc_32(file + rec_off, str_off +
                                           str_bytes - rec_off, ~0): the
                                           records, tables and strings
                                           (firmware/common/crc32.c) */
};
_Static_assert(sizeof(struct xidx_hdr) == 32, "xidx header layout");

struct xnote_rec {                      /* sorted by name (sortkey.c) */
    uint32_t name_off, path_off, size;
};
struct xcontact_rec {                   /* one row of a Contacts page */
    uint32_t card;                      /* the card: offset into the
                                           string block (its organisation,
                                           NUL, its items, a 0 byte) */
    uint16_t label;                     /* "Given Family", page-relative */
    uint16_t flags;                     /* XCONTACT_* */
};
#define XCONTACT_ORG_ONLY 0x0001        /* no person name: list the org */
struct xcal_rec {                       /* sorted by start */
    uint32_t start_rtc, end_rtc;        /* local RTC s since 2000 */
    uint32_t title_off, loc_off;
    uint16_t cal;                       /* index of its calendar file */
    uint16_t flags;                     /* XCAL_* */
};
#define XCAL_ALL_DAY  0x0001
#define XCAL_TODO     0x0002            /* a VTODO (To Do's) */
#define XCAL_DONE     0x0004
#define XCAL_ALARM    0x0008            /* feeds alarm_next() (W3-C) */
_Static_assert(sizeof(struct xnote_rec) == 12, "xnote_rec layout");
_Static_assert(sizeof(struct xcontact_rec) == 8, "xcontact_rec layout");
_Static_assert(sizeof(struct xcal_rec) == 20, "xcal_rec layout");

enum { XIDX_OK = 0, XIDX_ESHORT = -1, XIDX_EMAGIC = -2, XIDX_EKIND = -3,
       XIDX_EBOUNDS = -4, XIDX_ECRC = -5, XIDX_ECAP = -6 };

/* The bytes of one record of a kind; 0 for an unknown kind */
uint32_t xidx_rec_size(int kind);
/* The largest file of a kind */
uint32_t xidx_file_max(int kind);
/* Header checks against the file's size (magic, version, kind, every
 * offset and count inside the file, the caps: file_bytes <=
 * xidx_file_max(kind), Calendars count <= XCAL_MAX_INSTANCES). The CRC is
 * checked by xidx_check_body() once the file is read. */
int  xidx_check(const struct xidx_hdr *h, uint32_t file_bytes);
/* file = the whole file in memory; XIDX_OK or XIDX_ECRC */
int  xidx_check_body(const void *file, uint32_t file_bytes);
/* Sets magic, version and crc over the body of the whole file in memory
 * (the other fields set by the builder) */
void xidx_seal(void *file, uint32_t file_bytes);
/* The folder signature: sig = XIDX_SIG_INIT, then one xidx_sig_add()
 * per file in directory order (a rename, a resize, a touched mtime, an
 * added or removed file each change it). FNV-1a (prime 0x01000193) over
 * the name's bytes, a 0 byte, then size and mtime as 4 little-endian
 * bytes each. */
#define XIDX_SIG_INIT 0x811c9dc5u       /* FNV-1a offset basis */
uint32_t xidx_sig_add(uint32_t sig, const char *name, uint32_t size,
                      uint32_t mtime);

/* -- Paged lists (P12 W3-K) -------------------------------------------------
 * A list a screen browses (Notes' records, each Contacts order) is
 * stored as self-contained pages, so any window of rows is one open and
 * one range read (the Class A paged-read rule, brief 2.1). A page is
 * struct xidx_page, then rows records of the kind's size, then the
 * strings they name; every string offset in a page's record is relative
 * to the page's start, and the page's last byte is 0. The builder closes
 * a page when the next row would take it past the kind's target, but
 * never before it holds XIDX_PAGE_ROWS_MIN rows: a reader's window
 * (ui_theme list_rows + 2 rows: the scroll draws one past either end)
 * then always lies in at most 2 pages. A row's label is at most
 * XIDX_LABEL_MAX bytes with its NUL (a longer one is cut at a character
 * and ends in an ellipsis, xidx_label_put()), so a page of
 * XIDX_PAGE_ROWS_MIN rows stays small. Each list has a directory,
 * one struct xidx_pdir per page, in the header region; the lists' first
 * pages follow the directories, so entry reads the header, the
 * directories and the first pages in one read. Pages start on 4-byte
 * file offsets (the gap is padding). */
#define XIDX_PAGE_MAX   2048
#define XIDX_PAGE_ROWS_MIN 12           /* >= list_rows (9) + 2, plus one */
#define XIDX_LABEL_MAX  48              /* a row's label, with its NUL: the
                                           list's row is ~36 characters
                                           wide, so the cut seldom shows
                                           before the list's own */
/* The largest page of rows of rec_size bytes for a target */
#define XIDX_PAGE_LIMIT(target, rec_size) \
    ((target) > 4 + XIDX_PAGE_ROWS_MIN * ((rec_size) + XIDX_LABEL_MAX) \
     ? (target) : 4 + XIDX_PAGE_ROWS_MIN * ((rec_size) + XIDX_LABEL_MAX))
struct xidx_page { uint16_t rows, bytes; };
struct xidx_pdir {
    uint32_t off;                       /* file offset */
    uint16_t row0;                      /* the page's first row */
    uint16_t bytes;
};
_Static_assert(sizeof(struct xidx_page) == 4, "xidx_page layout");
_Static_assert(sizeof(struct xidx_pdir) == 8, "xidx_pdir layout");

/* -- the builder (xindex_build.c: MOD_XINDEX in a module build) -- */
/* The builder: a row's string bytes (with their NULs), and a row written
 * (rec: its record, str: where its strings go, str_rel: str's offset in
 * the page, for the record's offsets) */
typedef uint32_t (*xidx_row_len_fn)(void *ctx, uint32_t row);
typedef uint32_t (*xidx_row_put_fn)(void *ctx, uint32_t row, void *rec,
                                    char *str, uint32_t str_rel);
/* (put returns the bytes it wrote at str: len()'s for that row) */
/* Splits rows into pages (dir[].row0, .bytes; .off is the caller's):
 * a page closes past target once it holds XIDX_PAGE_ROWS_MIN rows. The
 * page count, or XIDX_ECAP past dir_max pages or a page past
 * XIDX_PAGE_MAX */
int  xidx_page_split(uint32_t rows, uint32_t rec_size, uint32_t target,
                     xidx_row_len_fn len, void *ctx, struct xidx_pdir *dir,
                     int dir_max);
/* The rows of page p (of npages, total rows) */
uint32_t xidx_page_rows(const struct xidx_pdir *dir, int npages, int p,
                        uint32_t total);
/* Writes page p (dir[p].bytes bytes) at dst */
void xidx_page_write(void *dst, const struct xidx_pdir *dir, int npages,
                     int p, uint32_t total, uint32_t rec_size,
                     xidx_row_put_fn put, void *ctx);
/* A label of the n bytes at s: as it is when n < XIDX_LABEL_MAX, else
 * cut at a UTF-8 character and ended with an ellipsis (U+2026); with its
 * NUL. out: NULL to count. Returns its bytes with the NUL. */
uint32_t xidx_label_put(char *out, const char *s, uint32_t n);
#define XIDX_ELLIPSIS "\xe2\x80\xa6"

/* -- the reader (resident) -- */
/* The reader: a directory read from the file (rows increasing from 0,
 * every page inside [lo, hi), each at most XIDX_PAGE_MAX); XIDX_OK or
 * XIDX_EBOUNDS */
int  xidx_dir_check(const struct xidx_pdir *dir, int npages, uint32_t total,
                    uint32_t lo, uint32_t hi);
/* The page holding row (binary search), or -1 */
int  xidx_dir_find(const struct xidx_pdir *dir, int npages, uint32_t row);
/* A page read from the file against its directory entry and row count */
int  xidx_page_check(const void *pg, const struct xidx_pdir *d,
                     uint32_t rows, uint32_t rec_size);
/* Record i of a checked page, and a string of it (NULL outside it) */
const void *xidx_page_rec(const void *pg, uint32_t i, uint32_t rec_size);
const char *xidx_page_str(const void *pg, uint32_t off);

/* A reader's cache of pages: slots packed in one buffer, the least
 * recently used evicted (the later slots move down). Pure: the screen
 * reads, the cache only places. */
#define XIDX_PC_SLOTS 16
struct xidx_pslot {
    uint32_t at;                        /* in buf */
    uint16_t bytes, page;
    uint8_t  list, age;
};
struct xidx_pcache {
    uint8_t *buf;
    uint32_t cap, used;
    uint8_t  n, clock;
    struct xidx_pslot s[XIDX_PC_SLOTS];
};
void xidx_pc_init(struct xidx_pcache *c, void *buf, uint32_t cap);
/* The cached page, or NULL; a hit counts as a use */
const void *xidx_pc_get(struct xidx_pcache *c, int list, int page);
/* Room for a page of bytes at the tail (evicting the oldest slots): the
 * place to read it into, or NULL when it cannot fit at all */
void *xidx_pc_room(struct xidx_pcache *c, uint32_t bytes);
/* The page now at the tail (read into xidx_pc_room()'s place, or already
 * there from the entry read) becomes a slot */
void xidx_pc_add(struct xidx_pcache *c, int list, int page, uint32_t bytes);
/* The cached page, or NULL, without counting a use */
const void *xidx_pc_peek(const struct xidx_pcache *c, int list, int page);

/* A reader's page-load rate (the manifest's pages = N a second): the
 * ticks of the last XIDX_RATE_N loads. xidx_rate_recent(): the loads
 * within the last per ticks of now. */
#define XIDX_RATE_N 8
struct xidx_rate {
    uint32_t t[XIDX_RATE_N];
    uint8_t  n, at;
};
unsigned xidx_rate_recent(const struct xidx_rate *r, uint32_t now,
                          uint32_t per);
void xidx_rate_add(struct xidx_rate *r, uint32_t now);

/* -- Notes (W1-N: the format's details, additive) ------------------------
 * Record 0 is the /Notes folder itself; every folder record (XNOTE_DIR)
 * names its children as one contiguous record range [first, first +
 * count). The root's entries come first (records 1..), folders and notes
 * mixed and sorted by display name (ui_sortkey), then each folder's notes
 * in the root's order of the folders. One level of folders, as stock;
 * deeper folders are not indexed.
 * name_off: the row's label in its page; the list shows it through
 * xnote_display_len(). path_off == name_off: the label is the file's own
 * name (with its ".txt"; the list drops the extension). Otherwise the
 * name is longer than a label (XIDX_LABEL_MAX): the label is the display
 * name, cut (xidx_label_put()), and path_off is XNOTE_PATH_FAR | the
 * file name's offset in the string block (read when the row opens). A
 * note's path is "/Notes/" [folder "/"] name. A note's size is its file
 * size (capped below XNOTE_DIR).
 * File: xidx_hdr (count = the records, rec_off 32, str_off = the pages'
 * end, str_bytes: the far names), struct xnote_tab at 32, its directory
 * (npage x struct xidx_pdir), then the pages in order (page 0 right
 * after the directory: entry is one read), then the string block.
 * Pages close at XNOTE_PAGE_BYTES (and XIDX_PAGE_ROWS_MIN). */
struct xnote_tab {
    uint16_t npage;                     /* pages of the one list */
    uint16_t pad;
    uint32_t hdr_end;                   /* the directory's end: page 0 */
};
_Static_assert(sizeof(struct xnote_tab) == 8, "xnote_tab layout");
#define XNOTE_PAGE_BYTES 512            /* ~16 typical rows */
#define XNOTE_PAGE_LIMIT XIDX_PAGE_LIMIT(XNOTE_PAGE_BYTES, 12)  /* 724 */
#define XNOTE_PDIR_MAX  ((XNOTE_MAX + XIDX_PAGE_ROWS_MIN) \
                         / XIDX_PAGE_ROWS_MIN)   /* pages (84): every
                                           page but the last holds
                                           XIDX_PAGE_ROWS_MIN rows */
#define XNOTE_HEAD_MAX  (32 + 8 + 8 * XNOTE_PDIR_MAX)  /* header, table,
                                           directory: a reader's cache
                                           holds it and two pages */
#define XNOTE_PATH_FAR  0x80000000u     /* path_off: in the string block */
#define XNOTE_ROOT_DIR  "/Notes"        /* Q12: the visible folder */
#define XNOTE_EXT       ".txt"
#define XNOTE_MAX       1000            /* entries indexed (notes + folders,
                                           the root not counted); more are
                                           cut, logged "xidx cut" */
#define XNOTE_POOL      (32*1024)       /* the builder's name pool */
#define XNOTE_NAME_MAX  255             /* FAT long names */
#define XNOTE_DIR       0x80000000u
#define XNOTE_DIR_SIZE(first, count) \
    (XNOTE_DIR | ((uint32_t)(first) << 15) | (uint32_t)(count))
#define XNOTE_FIRST(size)   (((size) >> 15) & 0x7fffu)
#define XNOTE_COUNT(size)   ((size) & 0x7fffu)

/* One gathered entry of the walk (the builder's block) */
#define XNOTE_PARENT_ROOT 0xffffu
struct xnote_src {
    uint32_t name;                      /* offset into the name pool */
    uint32_t size;                      /* file size; 0 for a folder */
    uint16_t parent;                    /* the folder's entry, or ROOT */
    uint16_t dir;                       /* 1: a folder */
};
/* Name order: <0, 0, >0 like strcmp, over the display names (a, alen) */
typedef int (*xnote_cmp_fn)(const char *a, int alen, const char *b,
                            int blen);
/* The bytes of name shown in a list: a note's name without ".txt" (any
 * case); a folder's whole name */
int  xnote_display_len(const char *name, int is_dir);
/* A file the Notes index takes: "*.txt" (any case), not hidden ('.') */
int  xnote_is_note(const char *name);
/* Lays out and seals a Notes index from n gathered entries (src[i].name
 * offsets into pool; folders before their notes is not required). sig:
 * the folder signature. scratch: XNOTE_SCRATCH(n) uint16_t. Returns the
 * file's bytes, or XIDX_ECAP (does not fit out_max), XIDX_EBOUNDS (a
 * child whose parent is not a root folder). */
#define XNOTE_SCRATCH(n) (4 * (n) + 4)
int32_t xnote_emit(void *out, uint32_t out_max, const struct xnote_src *src,
                   int n, const char *pool, uint32_t sig, xnote_cmp_fn cmp,
                   uint16_t *scratch);

#endif /* IPODUI_XINDEX_CORE_H */
