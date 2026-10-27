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

/* Chapter (audiobook) metadata.
 *
 * get_metadata() only records whether a file is an audiobook and how many
 * chapters it has (mp3entry.is_audiobook / mp3entry.chapter_count); it never
 * fills a chapter table, so it stays cheap and thread-safe for the tagcache
 * scanner. Whoever needs the actual chapter list (playback) calls
 * metadata_read_chapters() with its own buffer.
 */

#ifndef _METADATA_CHAPTERS_H
#define _METADATA_CHAPTERS_H

#include <stdbool.h>
#include <stdint.h>

/* HAVE_METADATA_EXTRAS (P6 Rev 1 item 16): defined here, explicitly, as in
 * metadata.h. The shell build and the host tests (warble) get the extras;
 * the stock build is byte-identical. Every guard in lib/rbcodec/metadata
 * and in the chapter callers uses this name. */
#ifndef HAVE_METADATA_EXTRAS
#if defined(HAVE_IPOD_UI) || defined(WARBLE)
#define HAVE_METADATA_EXTRAS
#endif
#endif

#define AB_MAX_CHAPTER_NAME  64        /* a title, incl. the NUL (63 B) */

#ifdef HAVE_METADATA_EXTRAS
/* ---- The packed chapter table (P6 §3.1, #48 option 2) ----------------
 * Same type name as stock, so mp3entry.audiobook and every caller keep
 * compiling; the layout is variable-length:
 *
 *   struct audiobook_chapters  16 B header
 *   struct ab_ch_entry[cap]     8 B each, sorted by start_ms (stable)
 *   char pool[pool_size]        titles, UTF-8, each NUL-terminated (D2);
 *                               pool[0] is always the empty title
 *
 * Size it with AB_CH_SIZE(cap, pool); sizeof() is the header only.
 * Native byte order (books.pak stores tables as they are in RAM; the sim
 * and the target are both little-endian, the pak is never moved between
 * them). Playback's slots and the parse transients use the default
 * AB_CH_TABLE_SIZE = 16 + 1,024 x 8 + 24,576 = 32,784 B (was 69,640 B).
 * Write a table with: ab_ch_init() once (fixes cap and pool), then per
 * parse ab_ch_reset(), ab_ch_expect(n), and for i = 0, 1, ...
 * ab_ch_set_start() + decode into ab_ch_title_buf() + ab_ch_commit(), or
 * ab_ch_put(); then chapter_count = n and ab_ch_sort() if unsorted.
 * The writer of a shared table brackets all of it with the seqlock
 * (np_playback.h, per slot). */
#ifndef AB_MAX_CHAPTERS
#define AB_MAX_CHAPTERS    1024        /* default entry cap (#48: 648 seen) */
#endif
#ifndef AB_CH_POOL
#define AB_CH_POOL        24576        /* default title pool */
#endif

struct ab_ch_entry {
    uint32_t start_ms;                 /* from the start of the file (in a
                                          books.pak blob: of the book) */
    uint16_t title_off;                /* into the pool; 0 = "" */
    uint8_t  title_len;                /* bytes, excl. the NUL (<= 63) */
    uint8_t  flags;                    /* AB_CH_F_* */
};
#define AB_CH_F_CUT      0x01          /* title shortened for the pool */
#define AB_CH_F_FILE     0x02          /* books.pak: a file without
                                          chapters, entered as itself */

struct audiobook_chapters {
    int16_t  chapter_count;            /* entries in use */
    int16_t  curr_chapter_idx;         /* maintained by apps/ */
    uint16_t cap;                      /* entries allocated */
    uint16_t pool_size;                /* pool bytes allocated */
    uint16_t pool_used;                /* >= 1 (pool[0] = "") */
    uint16_t cut;                      /* titles shortened (traced) */
    uint16_t expect;                   /* entries announced by the parser
                                          (fair share of the pool) */
    uint16_t spare;                    /* 0 */
};

#define AB_CH_HDR_SIZE     16
#define AB_CH_SIZE(cap, pool) \
    (AB_CH_HDR_SIZE + (size_t)(cap) * sizeof(struct ab_ch_entry) \
     + (size_t)(pool))
#define AB_CH_TABLE_SIZE   AB_CH_SIZE(AB_MAX_CHAPTERS, AB_CH_POOL)
/* A commit needs AB_MAX_CHAPTER_NAME bytes of room to decode in place:
 * the last AB_MAX_CHAPTER_NAME pool bytes are never handed out. */
#define AB_CH_POOL_MIN     (1 + AB_MAX_CHAPTER_NAME)

static inline struct ab_ch_entry *ab_ch_ent(const struct audiobook_chapters *ab)
{
    return (struct ab_ch_entry *)((char *)ab + AB_CH_HDR_SIZE);
}
static inline char *ab_ch_pool(const struct audiobook_chapters *ab)
{
    return (char *)(ab_ch_ent(ab) + ab->cap);
}

/* Accessors (the stock forms below expand to the old array code) */
#define ab_ch_cap(ab)              ((ab)->cap)
#define ab_ch_start(ab, i)         (ab_ch_ent(ab)[i].start_ms)
#define ab_ch_set_start(ab, i, ms) (ab_ch_ent(ab)[i].start_ms = (ms))
/* NUL-terminated, valid until the table is reset or re-sorted */
#define ab_ch_title_ptr(ab, i)     (ab_ch_pool(ab) + ab_ch_ent(ab)[i].title_off)
/* Where the parser decodes entry i's title (AB_MAX_CHAPTER_NAME bytes,
 * NUL-terminated by the decoder); valid until the next commit or put. */
#define ab_ch_title_buf(ab, i)     ((void)(i), ab_ch_pool(ab) + (ab)->pool_used)

/* Fixes cap and pool_size (pool >= AB_CH_POOL_MIN, <= 65,535; cap <=
 * 32,767) and resets. 'ab' is AB_CH_SIZE(cap, pool) bytes, 4-aligned. */
void ab_ch_init(struct audiobook_chapters *ab, int cap, int pool);
/* Empties the table, keeps cap and pool (chapter_count, curr, cut 0). */
void ab_ch_reset(struct audiobook_chapters *ab);
/* The parser will commit n entries (fair share: each title may use
 * (pool left) / (entries left), min 0, Rev 1 item 15). Optional: without
 * it the share is computed over cap. */
void ab_ch_expect(struct audiobook_chapters *ab, int n);
/* Stores the title just decoded into ab_ch_title_buf() for entry i (i ==
 * the number committed so far, i < cap): cut to the fair share on a UTF-8
 * boundary (counted in cut, AB_CH_F_CUT), NUL-terminated, pool advanced.
 * Returns the stored length. Does not change chapter_count. */
int  ab_ch_commit(struct audiobook_chapters *ab, int i);
/* set_start + copy (len bytes, no NUL needed, cut at 63) + commit */
int  ab_ch_put(struct audiobook_chapters *ab, int i, uint32_t start_ms,
               const char *title, int len);
/* Stable insertion sort of entries [0, chapter_count) by start_ms (8 B
 * moves; titles stay in place). Reversed 1,024: ~4.2 MB memmove (D3). */
void ab_ch_sort(struct audiobook_chapters *ab);
/* Index of the entry containing ms (the last start <= ms; 0 if ms is
 * before the first), -1 if empty. O(log n). Sorted tables only. */
int  ab_ch_find(const struct audiobook_chapters *ab, uint32_t ms);
/* Copies entry i's title (always terminated); returns its length. */
int  ab_ch_title(const struct audiobook_chapters *ab, int i,
                 char *buf, int size);

#else /* !HAVE_METADATA_EXTRAS: stock, the old array, byte-identical */

/* Chapter table capacity. playback carves one struct audiobook_chapters
 * (8 + AB_MAX_CHAPTERS * 68 bytes on ARM) out of the audio buffer. Stock
 * keeps 255 (the Nero 'chpl' limit). May be overridden with
 * -DAB_MAX_CHAPTERS=n; must be the same in every translation unit. */
#ifndef AB_MAX_CHAPTERS
#define AB_MAX_CHAPTERS     255
#endif

struct ab_chapter {
    char title[AB_MAX_CHAPTER_NAME];   /* UTF-8, NUL terminated, may be "" */
    unsigned long start_ms;
};

struct audiobook_chapters {
    int chapter_count;
    int curr_chapter_idx;              /* maintained by apps/ */
    struct ab_chapter chapters[AB_MAX_CHAPTERS];
};

#define AB_CH_TABLE_SIZE           sizeof(struct audiobook_chapters)
#define ab_ch_cap(ab)              AB_MAX_CHAPTERS
#define ab_ch_start(ab, i)         ((ab)->chapters[i].start_ms)
#define ab_ch_set_start(ab, i, ms) ((ab)->chapters[i].start_ms = (ms))
#define ab_ch_title_ptr(ab, i)     ((ab)->chapters[i].title)
#define ab_ch_title_buf(ab, i)     ((ab)->chapters[i].title)
#define ab_ch_reset(ab)            ((void)0)   /* callers zero the counts */
#define ab_ch_expect(ab, n)        ((void)0)
#define ab_ch_commit(ab, i)        ((void)0)

#endif /* HAVE_METADATA_EXTRAS */

struct mp3entry;

/* true if the file name has an audiobook extension (.m4b) */
bool metadata_is_audiobook_ext(const char *path);

/* Read the chapter table of an already-identified file (id3->codectype must
 * be set, i.e. get_metadata() succeeded) into the caller-supplied 'out'.
 * Uses only a few hundred bytes of stack and no static state, so it may be
 * called from any thread. The file position of fd is not preserved.
 * Returns the number of chapters read (0 if none or unsupported format),
 * or -1 on a read error. 'out' is always left in a consistent state.
 * Extras: 'out' was ab_ch_init()ed by its owner; at most ab_ch_cap(out)
 * entries are read (a smaller view table just holds fewer). */
int metadata_read_chapters(int fd, const struct mp3entry *id3,
                           struct audiobook_chapters *out);

/* MP4/M4B backend: Nero 'chpl' preferred, else QuickTime 'chap' text track.
 * With out == NULL only counts chapters (no titles read).
 * Returns the chapter count, or -1 on error. */
int mp4_read_chapters(int fd, struct audiobook_chapters *out);

#ifdef HAVE_METADATA_EXTRAS

/* ---- Coalesced reads (P6 fix 2) ----------------------------------------
 * The same, reading the atom walk, the sample tables and the titles in
 * large contiguous reads through windows carved from 'scratch' (the
 * caller's, never shared between threads; 4-aligned): no byte read twice,
 * no seek per sample, adjacent ranges coalesced. Measured (warble, the
 * 1,100-chapter text track of tests/fixtures/chapters_1100.m4b): 3,192
 * read() calls entry by entry; 55 with 2 KB of scratch, 41 with 4 KB, 7
 * with 16 KB. Less than AB_CH_SCRATCH_MIN (or NULL) reads entry by entry,
 * as metadata_read_chapters(). MP3 (ID3 CHAP) ignores the scratch. */
#define AB_CH_SCRATCH_MIN 512
int metadata_read_chapters_buf(int fd, const struct mp3entry *id3,
                               struct audiobook_chapters *out,
                               void *scratch, size_t scratch_size);
int mp4_read_chapters_buf(int fd, struct audiobook_chapters *out,
                          void *scratch, size_t scratch_size);

/* ---- Per-chapter artwork (#39) ----------------------------------------
 * Only the location of each chapter's image is recorded, never the image:
 * one 8-byte entry per chapter in a caller-sized array (5,184 B at 648
 * chapters), separate from the chapter table, so playback's chapter
 * buffer does not grow. Read at
 * index time by the art cache builder (docs/design/chapter-art.md). */
enum ab_art_type {
    AB_ART_NONE = 0,
    AB_ART_JPEG,
    AB_ART_PNG,
};

struct ab_chapter_art {
    uint32_t pos;                      /* file offset of the image data */
    uint32_t size : 24;                /* image bytes; 0: no image */
    uint32_t type : 8;                 /* enum ab_art_type */
};

/* Read the image location of each of the ab->chapter_count chapters
 * (sorted by start, as metadata_read_chapters() returns them for the same
 * file; only the starts are read, P6 §3.1) into art[0 .. count-1]; art[i]
 * belongs to entry i.
 * A chapter without an image of
 * its own gets size 0 (the caller falls back to the track's cover).
 * MP4: the QuickTime chapter image track (hdlr 'vide', JPEG or PNG
 * samples, referenced by tref 'chap'); each chapter gets the image shown
 * at its start time. MP3: APIC sub-frames of ID3v2 CHAP frames (not under
 * unsynchronisation, whose offsets don't map to the file).
 * Returns the number of chapters with an image, 0 if none, -1 on error.
 * Same threading rules as metadata_read_chapters(). */
int metadata_read_chapter_art(int fd, const struct mp3entry *id3,
                              const struct audiobook_chapters *ab,
                              struct ab_chapter_art *art);

int mp4_read_chapter_art(int fd, const struct audiobook_chapters *ab,
                         struct ab_chapter_art *art);

/* ---- ID3v2 CHAP backend ------------------------------------------------
 * Chapters from ID3v2.3/2.4 CHAP frames (TIT2 sub-frame titles in any ID3
 * text encoding), sorted by start time (stable). Returns the count, or -1
 * on error. id3_read_chapter_art() is the MP3 half of
 * metadata_read_chapter_art(). */
int id3_read_chapters(int fd, struct audiobook_chapters *out);
int id3_read_chapter_art(int fd, const struct audiobook_chapters *ab,
                         struct ab_chapter_art *art);

/* ---- Text decoding (shared by chapters and lyrics) --------------------
 * Values match the ID3v2 text encoding byte. */
enum meta_text_enc {
    META_TEXT_LATIN1  = 0,
    META_TEXT_UTF16   = 1,             /* BOM; without one: guessed */
    META_TEXT_UTF16BE = 2,
    META_TEXT_UTF8    = 3,
};

#define META_TEXT_NEWLINES  0x01       /* CR LF and CR become LF */
#define META_TEXT_STOP_NUL  0x02       /* stop at a NUL terminator */

/* Incremental conversion to UTF-8. The output is always NUL terminated and
 * never ends in a partial character; once it is full, 'truncated' is set
 * and the rest of the input is ignored. */
struct meta_text {
    char *dst;
    uint32_t size;                     /* bytes at dst, incl. the NUL */
    uint32_t len;                      /* bytes written, excl. the NUL */
    uint8_t enc;                       /* enum meta_text_enc */
    uint8_t flags;
    bool le;                           /* UTF-16 byte order (decided) */
    bool started;                      /* BOM handled */
    bool prev_cr;
    bool done;                         /* terminator seen or output full */
    bool truncated;
    uint16_t hi;                       /* pending high surrogate, or 0 */
};

void meta_text_init(struct meta_text *t, int enc, int flags,
                    char *dst, uint32_t size);
/* Feed n bytes; returns how many were consumed. Fewer than n are consumed
 * only when the decoder is done, or at the end of the input when an odd
 * UTF-16 byte is left (feed it again, with more input, next time). */
uint32_t meta_text_feed(struct meta_text *t, const uint8_t *src, uint32_t n);

/* One-shot: convert src[0..n) and return the length written. */
uint32_t meta_text_to_utf8(int enc, int flags, const uint8_t *src,
                           uint32_t n, char *dst, uint32_t size);

#endif /* HAVE_METADATA_EXTRAS */

#endif /* _METADATA_CHAPTERS_H */
