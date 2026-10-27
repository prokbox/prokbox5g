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
 * iPod UI shell: album art cache on-disk format
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
#ifndef _IPODUI_ARTFMT_H_
#define _IPODUI_ARTFMT_H_

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "artkey.h"
#include "libfile.h"

/* The files of .rockbox/artcache/ (docs/design/artcache.md §4, as built in
 * Phase 4b) and the album table the builder reads. Pure C over plain
 * open/read/write/lseek/close, so the host tests (tests/artcache/) run it
 * on real files; artcache.c adds the thread, the decoding and the policy.
 * All integers are little-endian (the native order of the target and of
 * every host we build on). Every CRC here is the zlib/PNG CRC-32
 * (Python: zlib.crc32(data)); on target crc_32r(data, len, ~0) ^ ~0.
 *
 * VERSION     supporting marker; actual file headers establish compatibility.
 *             A missing or unreadable marker never authorizes cache deletion.
 *
 * index.dat   header (512 B, struct artidx_hdr) + records (32 B,
 *             struct artcache_rec) sorted by key, unique, from byte 512,
 *             so 4 KB pages of 128 records are sector-aligned. Written only
 *             as index.tmp, then renamed over index.dat; 'generation' + 1
 *             each time. A reader trusts neither the rename nor the name:
 *             it checks the magic, the version, the header CRC and the
 *             records' CRC. Publication retains index.prev, which readers
 *             can validate after interruption. Temporary I/O failures defer
 *             work rather than proving that a file is missing or corrupt.
 *             An album with no record is PENDING.
 *
 * thumbs.pak  header (512 B, struct artpak_hdr: thumbnail w, h, slot size),
 *             then slots of slot_bytes (w*h*2 + 16 rounded up to 512) from
 *             byte 512: RGB565 pixels, row-major, no padding, then a
 *             struct artpak_tag naming the key the slot holds. Slots are
 *             written before the index that points at them (append, or
 *             reuse of a slot no committed record points at), so a record
 *             only ever points at a complete slot; slots past the
 *             index's n_slots (a cut append) are overwritten later. A
 *             reader checks the tag's key, so an older index made current
 *             again by a cut rename can never show another album's art.
 *
 * panels.pak  the main menu's panel art (H1): header (1 KB, struct
 *             artpanel_hdr: panel w, h, slot size, slots in the file, and
 *             what each of the ARTPANEL_SLOTS slots holds), then slots of
 *             artpak_slot_bytes(w, h) from byte 1 KB: RGB565 pixels, then
 *             a struct artpak_tag, zero padded (as thumbs.pak). Only for
 *             the panel set (LIBFILE_CAT_PANELSET, <= 32 albums), only
 *             built while audio is stopped. A slot's entry goes to state 0
 *             (header written) before the slot is overwritten, and gets
 *             its state after the slot is complete, so the header never
 *             names a slot being written; a reader also checks the tag.
 *             A slot is reused or appended at the file's end (n_slots).
 *
 * XX/XXXXXXXX.ART  bundles for the larger sizes (§4): from Phase 5 the
 *             builder writes one ARTCACHE_NOWPLAYING image (fitted, see
 *             below) per album with art, and per chapter image of a book
 *             (chapter keys, docs/design/chapter-art.md), with
 *             artbundle_write(). struct artbundle_hdr is their header.
 */

/* --- size table (artcache.md §2) ---------------------------------------- */
enum artcache_size {
    ARTCACHE_THUMB = 0,      /* list rows; in thumbs.pak */
    ARTCACHE_COVERFLOW,      /* Phase 7; bundles */
    ARTCACHE_NOWPLAYING,     /* Phase 5; bundles */
    ARTCACHE_PANEL,          /* panel set only; panels.pak (H1) */
    ARTCACHE_NUM_SIZES
};

struct artcache_dim { uint16_t w, h; };
/* The single place the sizes live (artfmt.c). Changing the thumbnail
 * size makes the next build start a new thumbs.pak (its header records
 * the size) and rebuild every thumbnail. */
extern const struct artcache_dim artcache_sizes[ARTCACHE_NUM_SIZES];

/* Fit sizes (Phase 5, Revision 1 item 3; owner photo 1222): for
 * these the table gives a BOX, not the image size. The bundle's image is
 * the source scaled so its longer side fills the box with the aspect
 * kept (never cropped, never stretched, upscaled if smaller): a
 * 16:9 source becomes 142 x 80, 2:3 95 x 142, 4:1 142 x 36. The bundle's
 * struct artbundle_img w, h are that fitted size, and the reader returns
 * them in bm->width/height; a reader's buffer must hold the whole box
 * (w * h * 2 of the table). The image's place in the box is
 * artcache_fit_rect(): centred both ways (tall art centred horizontally,
 * frozen default). ARTCACHE_NOWPLAYING only. */
static inline bool artcache_size_fits(enum artcache_size size)
{
    return size == ARTCACHE_NOWPLAYING;
}

struct artcache_fit {
    uint16_t w, h;              /* fitted image, 1..box */
    uint16_t x, y;              /* its top-left inside the box */
};

/* src_w x src_h (0 = unknown: the whole box) into box_w x box_h,
 * rounded to the nearest pixel (halves up). Integer maths, any thread. */
static inline void artcache_fit_rect(unsigned src_w, unsigned src_h,
                                     unsigned box_w, unsigned box_h,
                                     struct artcache_fit *f)
{
    uint32_t w = box_w, h = box_h;

    if (src_w && src_h)
    {
        if ((uint32_t)src_w * box_h >= (uint32_t)src_h * box_w)
            h = ((uint32_t)box_w * src_h * 2 + src_w) / (2u * src_w);
        else
            w = ((uint32_t)box_h * src_w * 2 + src_h) / (2u * src_h);
    }
    if (w < 1)
        w = 1;
    if (h < 1)
        h = 1;
    if (w > box_w)
        w = box_w;
    if (h > box_h)
        h = box_h;
    f->w = (uint16_t)w;
    f->h = (uint16_t)h;
    f->x = (uint16_t)((box_w - w) / 2);
    f->y = (uint16_t)((box_h - h) / 2);
}

/* --- index.dat ---------------------------------------------------------- */
#define ARTIDX_MAGIC      0x58415049u   /* "IPAX" */
#define ARTIDX_VERSION    1
#define ARTIDX_HDR_BYTES  512
#define ARTIDX_PAGE_BYTES 4096
#define ARTIDX_PAGE_RECS  (ARTIDX_PAGE_BYTES / (int)sizeof(struct artcache_rec))
#define ARTIDX_MAX_PAGES  512           /* 65,536 albums */
#define ARTIDX_MAX_RECS   (ARTIDX_MAX_PAGES * ARTIDX_PAGE_RECS)

struct artidx_hdr {             /* first 32 bytes of the 512-byte header */
    uint32_t magic, version;
    uint32_t generation;        /* + 1 per commit */
    uint32_t count;             /* records */
    uint32_t n_slots;           /* thumbs.pak slots in use (append point) */
    uint32_t rec_crc;           /* CRC-32 of the count * 32 record bytes */
    uint32_t sweep_commit;      /* album table commit_id of the last pass
                                   that re-checked image-file sources */
    uint32_t hdr_crc;           /* CRC-32 of the 28 bytes above */
};

enum artrec_state {             /* no record = PENDING */
    ARTREC_OK = 1,              /* thumb in 'slot' */
    ARTREC_NOART = 2,           /* looked: no art. Note glyph, no retry */
    ARTREC_FAILED = 3,          /* art we can't decode on the device
                                   (progressive JPEG, PNG): note glyph
                                   until the host tool (7b) builds it */
};

enum artsrc_kind { ARTSRC_NONE = 0, ARTSRC_FILE = 1, ARTSRC_EMBEDDED = 2 };

#define ARTREC_NO_SLOT 0xffffffffu

struct artcache_rec {           /* 32 bytes */
    uint64_t key;
    uint32_t slot;              /* thumbs.pak slot, ARTREC_NO_SLOT if none */
    uint8_t  state;             /* enum artrec_state */
    uint8_t  src_kind;          /* enum artsrc_kind */
    uint8_t  src_type;          /* AA_TYPE_JPG / BMP / PNG (metadata.h) */
    uint8_t  flags;             /* ARTREC_F_* (0 before Phase 5) */
    uint32_t mtime;             /* album's max tag_mtime when built */
    uint32_t src_size;          /* source image bytes */
    uint32_t src_crc;           /* CRC-32 of its first 16 KB */
    uint32_t src_hash;          /* artkey_fnv32() of the path it came from
                                   (the audio file for embedded art) */
};

/* Chapter art (Phase 5, docs/design/chapter-art.md; no format change).
 * A chapter record (ARTREC_F_CHAPTER, state OK) has the chapter key
 * (artcache_key_chapter()) and reuses the fields: slot + src_hash = the
 * low and high halves of its BUNDLE key (its own key, or, for chapters
 * sharing one image (same file position), the first such chapter's key:
 * one bundle, several records); src_size + src_crc = the book's key;
 * mtime = the book's max_mtime; src_kind EMBEDDED, src_type the image's.
 * It owns no thumbs.pak slot. A chapter without a usable image has no
 * record (PENDING: Now Playing shows the cover). The book's own record
 * gets ARTREC_F_CHAPTERS once its chapter pass is done for its mtime (a
 * rewrite for another mtime clears it, so the pass runs again). */
#define ARTREC_F_CHAPTER    0x01
#define ARTREC_F_CHAPTERS   0x02
/* QW2 (docs/phase11-brief.md §3.9, checklist item 13): bits 2-7 of an
 * album record's flags tag the Now Playing bundle it owns as COMPOSED for
 * one layout: artrec_np_tag_for(NP_LAYOUT_HASH), 1..63; 0 = a fitted
 * (version 1) bundle, or none. The builder's upgrade predicate is this
 * tag against the current layout's, read from the index in RAM: no
 * bundle is opened to find out. The bundle itself carries the full
 * 16-bit hash (artbundle_img.reserved), which the reader checks. A
 * book's tag covers its chapter bundles too: re-composing the book clears
 * ARTREC_F_CHAPTERS, so its chapter pass (stopped only) runs again. */
#define ARTREC_F_NPTAG_SHIFT 2
#define ARTREC_F_NPTAG_MASK  0xfc
static inline unsigned artrec_np_tag(const struct artcache_rec *r)
{
    return (r->flags & ARTREC_F_NPTAG_MASK) >> ARTREC_F_NPTAG_SHIFT;
}
static inline unsigned artrec_np_tag_for(unsigned layout_hash)
{
    return layout_hash % 63u + 1u;
}

static inline bool artrec_is_chapter(const struct artcache_rec *r)
{
    return (r->flags & ARTREC_F_CHAPTER) != 0;
}
/* The bundle a record's NOWPLAYING image is read from. */
static inline artcache_key_t artrec_bundle_key(const struct artcache_rec *r)
{
    return artrec_is_chapter(r) ? (uint64_t)r->src_hash << 32 | r->slot
                                : r->key;
}
static inline artcache_key_t artrec_book_key(const struct artcache_rec *r)
{
    return (uint64_t)r->src_crc << 32 | r->src_size;
}
/* #90: an album record's source signature, with no format change: the
 * source's kind, path hash (src_hash), size (src_size) and a CRC-32
 * (src_crc). Two styles, told apart by a spare bit of src_type (whose
 * low nibble is the AA_TYPE_*, artrec_src_type()):
 *   ARTREC_T_SIG2 set: the CRC covers the first 8 KB and the last 8 KB
 *     of the source (the whole of it up to 16 KB). Every album build
 *     since the #90 review writes it, and the lazy fill.
 *   clear: the CRC of the first 16 KB (every record written before):
 *     still trusted and compared in its own style, so no record
 *     mismatches when a build with SIG2 first reads the index.
 * A pass that finds the album's tags changed (another max_mtime) re-reads
 * only the signature of the source the build would try first, in the
 * record's style, and keeps the record (thumbnail, bundle, panel) when it
 * is the same. Known limits: no spare field holds the source file's
 * mtime, so a same-size edit inside the unread middle of a source larger
 * than 16 KB (SIG2), or past the first 16 KB (old style: a bottom-up BMP
 * edited in its top rows, an embedded JPEG changed past 16 KB), is not
 * seen until the album is rebuilt (Rebuild Cache, or its source changes
 * size). A record without a valid signature (CRC 0, or size 0) is trusted
 * as it is; its signature is filled lazily, a few per maintenance window,
 * and a record the fill cannot sign is marked ARTREC_T_UNSIGNED (tried:
 * never again; it stays trusted). Chapter records reuse these fields for
 * their book's key: never a signature. */
#define ARTREC_T_MASK      0x0f
#define ARTREC_T_SIG2      0x80
#define ARTREC_T_UNSIGNED  0x40
static inline unsigned artrec_src_type(const struct artcache_rec *r)
{
    return r->src_type & ARTREC_T_MASK;
}
static inline bool artrec_sig_valid(const struct artcache_rec *r)
{
    return r->state == ARTREC_OK && !artrec_is_chapter(r)
           && (r->src_kind == ARTSRC_FILE || r->src_kind == ARTSRC_EMBEDDED)
           && r->src_size != 0 && r->src_crc != 0;
}
/* A record that should carry a signature and does not (lazy fill) */
static inline bool artrec_sig_missing(const struct artcache_rec *r)
{
    return r->state == ARTREC_OK && !artrec_is_chapter(r)
           && (r->src_kind == ARTSRC_FILE || r->src_kind == ARTSRC_EMBEDDED)
           && !(r->src_type & ARTREC_T_UNSIGNED) && !artrec_sig_valid(r);
}
/* 'now' (the source found today: kind, hash, size, crc in old's style)
 * is the source
 * 'old' was built from: the build would make the same images. */
static inline bool artrec_sig_same(const struct artcache_rec *old,
                                   const struct artcache_rec *now)
{
    return artrec_sig_valid(old) && now->src_kind == old->src_kind
           && (now->src_type & ARTREC_T_SIG2) == (old->src_type & ARTREC_T_SIG2)
           && now->src_hash == old->src_hash
           && now->src_size == old->src_size && now->src_crc == old->src_crc;
}
/* Dropping the record removes the bundle file (it names it). */
static inline bool artrec_owns_bundle(const struct artcache_rec *r)
{
    return r->state == ARTREC_OK && artrec_bundle_key(r) == r->key;
}
/* The GC's tag of a book whose chapter records are current: its key and
 * mtime in one 64-bit value (a chapter record of another mtime is
 * stale). */
static inline uint64_t artchap_book_tag(artcache_key_t book, uint32_t mtime)
{
    return book ^ ((uint64_t)mtime * 0x9e3779b97f4a7c15ull);
}
void artrec_chapter_init(struct artcache_rec *r, artcache_key_t key,
                         artcache_key_t bundle, artcache_key_t book,
                         uint32_t mtime, uint8_t src_type);

/* Chapter i's image: 0 none, 1 usable (its file position in *pos, its
 * AA_TYPE_* in *src_type), -1 present but unusable (PNG, or its decode
 * failed: the builder marks every chapter of a failed image so). */
typedef int (*artchap_img_fn)(void *ctx, int i, uint32_t *pos,
                              uint8_t *src_type);
/* The first chapter k <= i whose image (usable or not) is at 'pos': the
 * owner of i's group. O(i); called once per group change. */
int artchap_owner(artchap_img_fn img, void *ctx, int i, uint32_t pos);
/* The records of one book's chapters, in chapter order, into out (room
 * entries; count <= room always fits): a chapter with a usable image gets
 * a record; all chapters with images at one position form a group
 * (adjacent or not: the builder points byte-identical copies, found by
 * size + CRC, at the first copy's position) whose bundle is its first
 * chapter's key (artchap_owner()): the builder decodes that owner only,
 * with the same grouping, and marks the whole group -1 if it fails, so
 * no record ever names a bundle that was not written. Returns the
 * number written. */
int artchap_plan(artcache_key_t book, uint32_t mtime, int count,
                 artchap_img_fn img, void *ctx, struct artcache_rec *out,
                 int room);
/* Sorts n records by key, one per key (state 0 last: a delete wins
 * over nothing else). Returns the new n. For artidx_write_merged(). */
int artrec_sort_unique(struct artcache_rec *r, int n);
/* The cap: an index of idx_count records may take n_new more chapter
 * records while it stays within ARTIDX_MAX_RECS minus the album table's
 * n_albums (album records always fit). */
static inline bool artchap_fits(uint32_t idx_count, uint32_t n_albums,
                                uint32_t n_new)
{
    uint32_t room = n_albums >= ARTIDX_MAX_RECS ? 0
                  : ARTIDX_MAX_RECS - n_albums;

    return idx_count <= room && n_new <= room - idx_count;
}

/* --- thumbs.pak --------------------------------------------------------- */
#define ARTPAK_MAGIC      0x50415049u   /* "IPAP" */
#define ARTPAK_VERSION    2           /* 2: 55x55 thumbs */
#define ARTPAK_HDR_BYTES  512
#define ARTPAK_TAG_MAGIC  0x53545049u   /* "IPTS" */

struct artpak_hdr {
    uint32_t magic, version;
    uint16_t w, h;
    uint32_t slot_bytes;
    uint32_t hdr_crc;           /* CRC-32 of the 16 bytes above */
};

struct artpak_tag {             /* right after the pixels of a slot */
    uint64_t key;
    uint32_t pix_crc;           /* CRC-32 of the w*h*2 pixel bytes */
    uint32_t magic;             /* ARTPAK_TAG_MAGIC */
};

static inline uint32_t artpak_slot_bytes(unsigned w, unsigned h)
{
    return (w * h * 2 + sizeof(struct artpak_tag) + 511) & ~511u;
}
static inline uint32_t artpak_slot_offset(uint32_t slot_bytes, uint32_t slot)
{
    return ARTPAK_HDR_BYTES + slot * slot_bytes;
}

/* --- panels.pak (H1) ---------------------------------------------------- */
#define ARTPANEL_MAGIC     0x4e504149u  /* "IAPN" */
/* Stays 1 (#92 review M1): the regroup (artcache.c, inside a maintenance
 * window) changes only which slot holds which cover, so an 8b build reads
 * a regrouped file as it is (it refuses any other version, and would show
 * no panels). Grouped (each set's covers in contiguous slots, entry
 * order: the drift's bank refresh is one read) is a property of the slot
 * order alone, artpanel_grouped(). Builds after a regroup may break the
 * grouping (a new cover takes the lowest free slot); the next window
 * regroups. */
#define ARTPANEL_VERSION   1
#define ARTPANEL_HDR_BYTES 1024
#define ARTPANEL_SLOTS     LIBFILE_PANELS_MAX

struct artpanel_ent {           /* 24 bytes */
    uint64_t key;               /* album (0: slot free) */
    uint32_t mtime;             /* album's max tag_mtime when built */
    uint32_t src_crc, src_size; /* the source, as in its index record */
    uint8_t  state;             /* 0 none (free / being written), else
                                   enum artrec_state: OK = pixels in
                                   'slot'; NOART / FAILED: none (no retry
                                   until the source changes) */
    uint8_t  slot;              /* OK: the slot (< n_slots) */
    uint8_t  pad[2];            /* [0]: the entry's set (artcache.c
                                   PENT_SET, #58); [1]: 0, or with
                                   HAVE_BCMFW_VIDEO the content width
                                   (ARTPANEL_CW) */
};
#ifdef HAVE_BCMFW_VIDEO
/* Phase 14a (docs/phase14a-brief.md 3.5): the width in pixels of the
 * picture inside an OK slot, centred, full slot height: 0 = the whole
 * slot (every square cover, and every file written before 14a), else a
 * portrait poster fitted to the slot's height (200 for a 2:3 poster in a
 * 300x300 slot). Stored halved in pad[1] (pad[0] is taken by the set), so
 * widths are even; ARTPANEL_VERSION stays 1. Chosen by the source's
 * aspect, not by "video" (podcast show art reuses it). Written by the art
 * cache builder, read by the panel (V4). */
#define ARTPANEL_CW(e)          ((int)(e)->pad[1] * 2)
#define ARTPANEL_SET_CW(e, w)   ((e)->pad[1] = (uint8_t)((w) / 2))
#endif

struct artpanel_hdr {           /* 792 bytes, padded to 1 KB on disk */
    uint32_t magic, version;
    uint16_t w, h;
    uint32_t slot_bytes;
    uint32_t n_slots;           /* slots present in the file (append
                                   point); an entry per album of the set,
                                   a slot per entry in state OK */
    uint32_t hdr_crc;           /* CRC-32 of the header, this field 0 */
    struct artpanel_ent ent[ARTPANEL_SLOTS];
};

static inline uint32_t artpanel_slot_offset(uint32_t slot_bytes, uint32_t s)
{
    return ARTPANEL_HDR_BYTES + s * slot_bytes;
}
/* The set of an entry (pad[0]: artcache.c PENT_SET, #58) */
#define ARTPANEL_ENT_SET(e)     ((e)->pad[0])
/* #92: every set's OK entries, in entry order, in consecutive ascending
 * slots (any version: the slot order alone) */
bool artpanel_grouped(const struct artpanel_hdr *h);
/* The regroup's plan: new_slot[k] for every OK entry k (sets in
 * ascending order, then entry order; 0xff for the others). Returns the
 * slots used (the new n_slots). */
int artpanel_regroup_plan(const struct artpanel_hdr *h,
                          uint8_t new_slot[ARTPANEL_SLOTS]);
/* #92: entries ent[] (n of them, OK, of one set) whose slots are
 * consecutive and ascending: one read covers them all */
bool artpanel_run(const struct artpanel_hdr *h, const int *ent, int n);
/* An empty header for w x h panels */
void artpanel_hdr_init(struct artpanel_hdr *h, unsigned w, unsigned h_);
void artpanel_hdr_seal(struct artpanel_hdr *h);     /* sets hdr_crc */
bool artpanel_hdr_ok(const struct artpanel_hdr *h, unsigned w, unsigned h_);
/* Reads entry e's slot of panels.pak 'path' (n pixel bytes into pix,
 * then its tag). True only for a complete slot whose tag names e->key and
 * whose read no writer overlapped: the builder bumps *seq before it
 * rewrites any slot (the entry goes to state 0 first), so *seq still
 * equal to seq0 (the value the header was read under) after the read
 * means the pixels are the ones the tag was written with (H1). */
bool artpanel_read(const char *path, uint32_t slot_bytes,
                   const struct artpanel_ent *e, void *pix, size_t n,
                   const volatile int *seq, int seq0);

/* --- the panel pool (#100): panelpool.pak + panelpool.idx -------------- *
 * panels.pak stays the default set (LIBFILE_CAT_PANELSET col 0, <= 32
 * covers) that every build reads, 8b included (#92 review M1: rollback).
 * The pool's other covers (the precompute's pool order, col 1, minus the
 * default set) live in panelpool.pak, which only a build with the pool
 * knows: the same slot layout as panels.pak (ARTPANEL_HDR_BYTES unused at
 * the front, then slots of slot_bytes, pixels then the tag), written IN
 * PLACE, only into slots the published index does not name (then fsync,
 * then the index). panelpool.idx (safefile_replace(), a few KB) holds:
 *   - first ARTPANEL_HDR_BYTES: the SHOWN set, an artpanel_hdr with magic
 *     ARTSHOWN_MAGIC whose entries name a panels.pak slot (slot <
 *     ARTPOOL_SLOT0) or a panelpool.pak slot (slot - ARTPOOL_SLOT0): the
 *     UI's readers read only this 1 KB, as they read panels.pak's header;
 *   - then struct artpool_hdr: panelpool.pak's slot table, the pinned
 *     keys (recently played and newest, from the precompute), the shown
 *     set's random picks (the next rotation avoids them) and the time of
 *     the last rotation.
 * A rotation (artpool_pick()) re-chooses the shown set from the covers
 * both files hold: no image work, one index write. */
#define ARTSHOWN_MAGIC    0x53504149u  /* "IAPS" */
#define ARTPOOL_MAGIC     0x4c504149u  /* "IAPL" */
#define ARTPOOL_VERSION   1
#define ARTPOOL_ENTS      128   /* covers of panelpool.pak (its slots) */
#define ARTPOOL_SLOT0     ARTPANEL_SLOTS    /* shown: pool slot + this */
#define ARTPOOL_PINNED    ARTPANEL_SLOTS
#define ARTPOOL_RAND      ARTPANEL_SLOTS
/* Music keeps this many random places whatever the books take (the
 * default set's random share with no books: 32 - 12 played - 10 newest;
 * the newest give way first) */
#define ARTPOOL_MUSIC_RAND 10
/* Q10: of the book set's 8, at most this many random */
#define ARTPOOL_BOOK_RAND 3
#define ARTPOOL_BOOKS     8     /* library.c PANEL_BOOKS */
#define ARTPOOL_VIDEOS    8     /* library.c PANEL_VIDEOS */

struct artpool_hdr {
    uint32_t magic, version;
    uint32_t n_slots;           /* slots in panelpool.pak (append point) */
    uint32_t rot_time;          /* RTC seconds at the last rotation (0:
                                   none; units without an RTC: 0) */
    uint32_t rot_count;         /* rotations so far (the picks' seed) */
    uint16_t n_pinned, n_rand;
    uint32_t crc;               /* CRC-32 of this struct, this field 0 */
    struct artpanel_ent ent[ARTPOOL_ENTS];  /* as panels.pak's: pad[0]
                                   the set, pad[1] the content width */
    uint64_t pinned[ARTPOOL_PINNED];        /* priority order */
    uint64_t rand[ARTPOOL_RAND];            /* the shown random picks */
};

struct artpool_idx {            /* panelpool.idx, as it is on disk */
    union {
        struct artpanel_hdr h;
        uint8_t bytes[ARTPANEL_HDR_BYTES];
    } shown;
    struct artpool_hdr pool;
};

static inline bool artshown_pool(const struct artpanel_ent *e)
{
    return e->slot >= ARTPOOL_SLOT0;
}
void artpool_init(struct artpool_idx *x, unsigned w, unsigned h_);
void artpool_seal(struct artpool_idx *x);
/* The pool part (and the slots its entries name < n_slots) */
bool artpool_ok(const struct artpool_hdr *p);
/* The shown block, as the UI's readers check it */
bool artshown_ok(const struct artpanel_hdr *h, unsigned w, unsigned h_);
/* The lowest slot of panelpool.pak no OK entry names (n_slots: append) */
uint32_t artpool_free_slot(const struct artpool_hdr *p);
/* Chooses the shown set from the OK entries of panels.pak's header 'ph'
 * (may be NULL) and of the pool: per set, the pinned keys (x->pool.pinned
 * order) first, leaving ARTPOOL_MUSIC_RAND (music) or ARTPOOL_BOOK_RAND
 * (books, of ARTPOOL_BOOKS) places to random picks while there are
 * others; music has the rest of the 32. 'rotate': the random
 * picks avoid the previous ones (x->pool.rand) while others are left;
 * else (a refresh after a build) they keep the previous ones first.
 * 'seed' drives the random choice. Writes x->shown (sealed) and
 * x->pool.rand / n_rand; returns the shown count. Pure: no I/O. */
int artpool_pick(struct artpool_idx *x, const struct artpanel_hdr *ph,
                 uint32_t seed, bool rotate);

/* --- npcover.pak (np-sharp, owner 2026-10-24) -------------------------- *
 * The Now Playing row's sharp cover for a playing album that no panel set
 * holds (panels.pak, panelpool.pak): panels.pak's layout (a 1 KB front,
 * then 300 x 300 slots, pixels then the tag) with its own magic and at
 * most ARTNPC_SLOTS slots. Entries in most-recent-first order: a new
 * render goes to ent[0] and the oldest gives up its slot. Written in
 * place (the slot's old tag cleared first, then pixels, tag, fsync, then
 * the header and fsync), so a reader checks the tag against the entry
 * (artpanel_read()). */
#define ARTNPC_MAGIC      0x43504149u  /* "IAPC" */
#define ARTNPC_SLOTS      2            /* 1 KB + 2 x 180,224 B */

/* An empty npcover.pak header for w x h panels */
void artnpc_init(struct artpanel_hdr *h, unsigned w, unsigned h_);
/* Sound: magic, size, CRC, n_slots <= ARTNPC_SLOTS, OK entries only in
 * the first ARTNPC_SLOTS, each naming its own slot < n_slots */
bool artnpc_ok(const struct artpanel_hdr *h, unsigned w, unsigned h_);
/* The OK entry of 'key', or -1 */
int artnpc_find(const struct artpanel_hdr *h, uint64_t key);
/* The slot a new render takes: one no OK entry names (an append while
 * n_slots < ARTNPC_SLOTS), else the oldest entry's */
uint32_t artnpc_slot(const struct artpanel_hdr *h);
/* e (OK, its slot from artnpc_slot()) becomes ent[0]: the entries of
 * the same key or the same slot are dropped, the rest move down, the
 * oldest past ARTNPC_SLOTS falls off; n_slots covers the slot; sealed */
void artnpc_put(struct artpanel_hdr *h, const struct artpanel_ent *e);

/* --- bundles (Phase 5: ARTCACHE_NOWPLAYING, album and chapter keys) ----- */
#define ARTBUNDLE_MAGIC   0x43415049u   /* "IPAC" */
#define ARTBUNDLE_VERSION 1         /* what artbundle_write() writes */
#define ARTBUNDLE_MAX_IMG 4
/* QW2 (docs/phase11-brief.md sections 3.9, 4.8; Rev 1 L-2): a per-bundle
 * version in the existing artbundle_hdr.version, no new header field.
 * Readers accept both indefinitely; the global "IPAC 3" is never bumped.
 * The composed NP image carries ARTBUNDLE_FMT_NPCOMPOSED in
 * artbundle_img.format and the 16-bit NP_LAYOUT_HASH in its reserved. */
#define ARTBUNDLE_VERSION_FITTED   1    /* = ARTBUNDLE_VERSION today */
#define ARTBUNDLE_VERSION_COMPOSED 2
#define ARTBUNDLE_FMT_NPCOMPOSED   1    /* artbundle_img.format */

struct artbundle_img {          /* 12 bytes */
    uint8_t  size_id;           /* enum artcache_size */
    uint8_t  format;            /* 0 = RGB565 native */
    uint16_t w, h;
    uint16_t reserved;
    uint32_t offset;            /* from file start, multiple of 512 */
};

struct artbundle_hdr {          /* padded to 512 bytes on disk */
    uint32_t magic, version;
    uint64_t key;
    uint8_t  probe, n_img, src_kind, src_type;
    uint32_t src_size, src_offset, src_crc, src_hash;
    uint32_t builder;           /* 0 device, 1 host tool; version above */
    struct artbundle_img img[ARTBUNDLE_MAX_IMG];
    /* then "albumartist\x1Falbum\0" (<= 400 bytes) for collision checks */
};

/* "XX/XXXXXXXX.ART" relative to the cache directory (16 bytes + NUL). */
void artbundle_name(artcache_key_t key, char out[17]);
#define ARTBUNDLE_HDR_BYTES 512   /* the header on disk; pixels follow */
/* One image (size 'size', w x h RGB565 at 'pix') as the device builds a
 * bundle: a zero header, the pixels at ARTBUNDLE_HDR_BYTES, then the
 * header, so a cut write leaves no valid header. 'hdrbuf': 512 B of
 * scratch. The directory must exist. Writes path.tmp, flushes/closes and
 * reads it back before publication; retains path.prev and never truncates
 * path. False leaves the old bundle readable, including after rollback fails.
 * hdrbuf is scratch and its contents are unspecified on return. */
bool artbundle_write(const char *path, artcache_key_t key,
                     enum artcache_size size, unsigned w, unsigned h,
                     const void *pix, void *hdrbuf);
/* The image of 'size' from bundle 'path' if its header names 'key': at
 * most box_w x box_h (a fit size's image is smaller on one side), into
 * buf; its size in *w, *h. 1 read, 0 no file / no such image / another
 * key, -1 present but unreadable or too big. Falls back to path.prev when
 * the primary cannot supply the requested image. */
int artbundle_read(const char *path, artcache_key_t key,
                   enum artcache_size size, unsigned box_w, unsigned box_h,
                   void *buf, size_t bufsize, uint16_t *w, uint16_t *h);
/* (Versions 1 and 2 are both read; only a format 0 image is returned.) */

/* QW2: one image 'im' (size_id, format, w, h, reserved; offset is set
 * here) in a bundle of 'version', as artbundle_write() writes one. */
bool artbundle_write_img(const char *path, artcache_key_t key,
                         uint32_t version, const struct artbundle_img *im,
                         const void *pix, void *hdrbuf);
/* Same, using the caller's larger existing scratch for bulk read-back
 * validation. scratch_bytes >= 512; adds no allocation or permanent RAM. */
bool artbundle_write_img_buffered(const char *path, artcache_key_t key,
                                  uint32_t version,
                                  const struct artbundle_img *im,
                                  const void *pix, void *scratch,
                                  size_t scratch_bytes);
/* QW2: the image of 'size' from bundle 'path' in ONE read(): header and
 * pixels land together in buf (bufsize >= ARTBUNDLE_HDR_BYTES + the
 * pixels; the read asks for bufsize bytes and takes what the file has),
 * the pixels at buf + ARTBUNDLE_HDR_BYTES, any format (0 fitted,
 * ARTBUNDLE_FMT_NPCOMPOSED); its entry in *im. buf must be 4-aligned.
 * 1 found, 0 no file / no such image / another key, -1 present but
 * unreadable, too big or cut. */
int artbundle_read_one(const char *path, artcache_key_t key,
                       enum artcache_size size, void *buf, size_t bufsize,
                       struct artbundle_img *im);

/* --- album table (input; written by library.c, read by the builder) ----
 *
 * The LIBFILE_CAT_ALBUMTAB section of the library precompute file
 * (libfile.h: <db dir>/LIBFILE_NAME): one struct libfile_album (24 B) per
 * album, audiobook and podcast show, sorted by representative master
 * index (folder order on disk, which suits the builder's reads). Keys may
 * repeat (two groups with the same strings) or be 0 (no key: skipped).
 * artfmt depends only on libfile.h, never on library.h (Decision 7).
 *
 * The builder's view of a checked table: the file header's fixed part
 * (whose sum covers every section's sum, so it identifies the file) and
 * where the section is. */
struct artalb_hdr {
    struct libfile_hdr lib;     /* as read and checked */
    uint32_t offset;            /* of the album section in the file */
    uint32_t n_albums;          /* its records (including key 0 ones) */
};

/* A stack copy of a header's fixed part (offsetof(struct libfile_hdr,
 * sec): 32 B), all the art readers compare (artalb_read(),
 * artalb_panels(), artcache.c table_find()). Phase 14a (M8): the video
 * build's 800 B header would cost the art thread 800 B a frame for those
 * 32; the default build keeps the whole struct (its objects unchanged). */
#ifdef HAVE_BCMFW_VIDEO
#define ARTFMT_LIBHDR_FIXED(name) \
    uint32_t name[offsetof(struct libfile_hdr, sec) / sizeof(uint32_t)]
#else
#define ARTFMT_LIBHDR_FIXED(name) struct libfile_hdr name
#endif

/* The stamp of the database in RAM the table must match. */
struct artalb_stamp {
    int32_t  commitid;          /* tagcache_ui_commitid() */
    int32_t  entries;           /* tagcache_ui_count() */
    uint32_t sortkey_version;   /* SORTKEY_VERSION */
};

struct artalb_rec {             /* the builder's in-memory form, 24 bytes */
    uint64_t key;               /* artcache_key() of the grouping strings */
    int32_t  rep_idx;           /* master index entry of a track of it */
    uint32_t max_mtime;         /* max tag_mtime over the album's tracks */
    uint32_t flags;             /* LIBFILE_ALB_* */
    uint32_t count;             /* tracks */
};

/* --- CRC ---------------------------------------------------------------- */
/* Running CRC-32: start with ARTFMT_CRC_INIT, finish with artfmt_crc_end() */
#define ARTFMT_CRC_INIT 0xffffffffu
uint32_t artfmt_crc(uint32_t crc, const void *p, size_t n);
static inline uint32_t artfmt_crc_end(uint32_t crc) { return ~crc; }

/* --- headers ------------------------------------------------------------ */
void artidx_hdr_seal(struct artidx_hdr *h);     /* sets hdr_crc */
bool artidx_hdr_ok(const struct artidx_hdr *h);
void artpak_hdr_init(struct artpak_hdr *h, unsigned w, unsigned h_);
bool artpak_hdr_ok(const struct artpak_hdr *h, unsigned w, unsigned h_);

/* --- index reader ------------------------------------------------------- *
 * Validates a whole index.dat once at open (one sequential read; the
 * records' CRC and their order), keeps the first key of every 4 KB page
 * ("fences", 8 B per 128 albums) and one page. A lookup is a binary search
 * of the fences in RAM plus at most one 4 KB read: O(log n), one read per
 * page miss. The struct lives in the caller's buffer (UI: its reserved
 * read block; builder: its work block). */
struct artidx_reader {
    int fd;                     /* -1 when closed */
    struct artidx_hdr hdr;      /* valid while fd >= 0 */
    int page;                   /* page in 'recs', -1 none */
    int n_pages;
    uint64_t fence[ARTIDX_MAX_PAGES];
    struct artcache_rec recs[ARTIDX_PAGE_BYTES / 32];
};

enum artidx_open_result {
    ARTIDX_OPENED = 0,          /* valid */
    ARTIDX_MISSING = 1,         /* no file */
    ARTIDX_CORRUPT = -1,        /* exists but invalid (torn, bad CRC...) */
    ARTIDX_IO_ERROR = -2,       /* unavailable; never permission to rebuild */
};

/* Caller flushes, closes and validates tmp before calling, and validates
 * primary before allowing it to replace prev. Paths must be distinct and on
 * the same filesystem. No unlink fallback: even a failed rollback leaves the
 * previous file available by name. A failed publication retains tmp. A
 * primary that can't be opened for a reason other than "not found" is not
 * missing: nothing is renamed, ARTFILE_RETAINED (the caller's capped retry),
 * logged once per boot. */
enum artfile_publish_result {
    ARTFILE_PUBLISHED = 0,
    ARTFILE_RETAINED = -1,       /* failed; primary restored/unchanged */
    ARTFILE_RECOVER_PREV = -2,   /* failed; serve prev, rollback failed */
};
enum artfile_publish_result artfile_publish(const char *path, const char *tmp,
                                           const char *prev);
/* tmp takes path's name, never by a rename onto an existing name (that can
 * leave two entries named path on the device): path goes first. Only for
 * a path that is missing, damaged or regenerable, where the caller's
 * recovery finds tmp (or another copy) if a cut lands between the two.
 * 0, or -1 with errno. */
int artfile_replace(const char *tmp, const char *path);
/* #91 H1: before tmp is opened with O_TRUNC, a residue tmp goes, or only
 * its name when a cut inside an earlier tmp -> path rename left it on
 * path's chain (truncating it would free path's data). false: an I/O
 * error, nothing written. */
bool artfile_tmp_clear(const char *tmp, const char *path);
/* Once per boot, before the first pass: tmp or prev left as a second name
 * on path's chain (a cut inside a publish's rename) loses that name only;
 * different files are kept (prev is the previous generation). */
void artfile_recover(const char *path, const char *tmp, const char *prev);

#ifdef ARTFMT_HOST
/* Deterministic real-file fault injection; zero mask disables it. Bit zero
 * fails the next operation, bit one the following one, etc. */
enum artfmt_test_op {
    ARTFMT_TEST_OPEN, ARTFMT_TEST_READ, ARTFMT_TEST_WRITE,
    ARTFMT_TEST_FLUSH, ARTFMT_TEST_RENAME, ARTFMT_TEST_CLOSE,
    ARTFMT_TEST_SHORT_READ, ARTFMT_TEST_SHORT_WRITE,
    ARTFMT_TEST_PROBE,          /* artfile_publish()'s existence probe */
    ARTFMT_TEST_NOPS
};
void artfmt_test_fail(enum artfmt_test_op op, uint64_t mask, int error);
void artfmt_test_reset(void);
#endif

void artidx_init(struct artidx_reader *r);
typedef void (*artidx_visit_fn)(void *ctx, const struct artcache_rec *rec);
enum artidx_open_result artidx_open(struct artidx_reader *r, const char *path);
#ifndef ARTFMT_HOST
/* p19-art-yield: called (with its source line in the simulator, else 0)
 * between the steps of a whole-file walk; the art builder sets it
 * (artcache.c, art_pace_at()) */
extern void (*artfmt_pace)(int line);
#endif
/* Same; calls visit(ctx, rec) for every record of a valid file (before
 * the CRC is known: the caller discards what it gathered on failure). */
enum artidx_open_result artidx_open_ex(struct artidx_reader *r,
                                       const char *path,
                                       artidx_visit_fn visit, void *ctx);
void artidx_close(struct artidx_reader *r);
/* Copies the record for 'key' into *out. False if absent or on I/O error
 * (*io_err set to true then, if given). */
bool artidx_lookup(struct artidx_reader *r, artcache_key_t key,
                   struct artcache_rec *out, bool *io_err);
/* Record 'i' (0..count-1), through the page cache. */
bool artidx_get(struct artidx_reader *r, uint32_t i, struct artcache_rec *out);

/* --- index writer ------------------------------------------------------- *
 * Writes path (index.tmp) = merge of the open reader 'old' (may be closed:
 * empty) with 'add' (n_add records sorted by key; same key replaces the
 * old record; state 0 deletes it), dropping old records for which
 * keep(ctx, rec, i) returns false (NULL: keep all; i is the record's
 * position in 'old', 0..count-1). 'page' is a 4 KB work buffer. Returns
 * the number of records written, < 0 on error. The caller renames the
 * file. 'hdr_tmpl' gives generation and n_slots. */
typedef bool (*artidx_keep_fn)(void *ctx, const struct artcache_rec *rec,
                               uint32_t i);
int artidx_write_merged(const char *path, struct artidx_reader *old,
                        const struct artcache_rec *add, int n_add,
                        artidx_keep_fn keep, void *ctx,
                        const struct artidx_hdr *hdr_tmpl, void *page);

/* --- GC ----------------------------------------------------------------- *
 * Sets bit i of 'live' (idx->hdr.count bits, cleared first) for every
 * record i of 'idx' whose key is in the album table, for a table of any
 * size: the table (folder order) is read in chunks of up to 'cap' keys
 * into the buffer keys(ctx) returns, each chunk sorted and merged with one
 * sequential walk of the index (one 4 KB read per 128 records). keys() is
 * called again after every read (the caller's buffer may move while a
 * read yields; the sort does no I/O). 'tbuf' holds tbuf_n (<= 256) table
 * records. False on an I/O error or a replaced table: keep everything. */
typedef uint64_t *(*artidx_keys_fn)(void *ctx);
bool artidx_gc_mark(struct artidx_reader *idx, const char *tpath,
                    const struct artalb_hdr *thdr, struct artalb_rec *tbuf,
                    int tbuf_n, artidx_keys_fn keys, void *ctx, uint32_t cap,
                    uint32_t *live);

/* The GC's second pass (chapter art): after artidx_gc_mark(), sets the
 * live bit of every chapter record whose book record is live, has
 * ARTREC_F_CHAPTERS and the same mtime. Two sequential walks of the
 * index: the tags (artchap_book_tag()) of those books into the buffer
 * keys(ctx) returns (re-fetched after every read, as above; at most
 * 'cap'), sorted; then each chapter record's tag is looked up. More such
 * books than 'cap': every chapter record is kept. False on an I/O
 * error (keep everything). */
bool artidx_gc_mark_chapters(struct artidx_reader *idx, artidx_keys_fn keys,
                             void *ctx, uint32_t cap, uint32_t *live);

/* --- album table -------------------------------------------------------- *
 * Validates the precompute file at 'path' against the database stamp
 * (magic, version, header sum, commit id, entries, SORTKEY_VERSION), finds
 * its album section and checks that section's sum and every rep_idx with
 * one sequential read through 'page' (4 KB). On success *hdr describes it.
 * ARTIDX_MISSING: no file, or one for another database (not current yet:
 * library.c writes a new one and requests a build); ARTIDX_CORRUPT: torn
 * or damaged (the library rewrites it when asked). */
enum artidx_open_result artalb_check(const char *path,
                                     const struct artalb_stamp *want,
                                     struct artalb_hdr *hdr, void *page);
/* Reads records [first, first + n) (n <= 256) into out; checks that the
 * file header still equals expect's (the file was not replaced meanwhile).
 * Returns the number read, < 0 on error or replacement. Opens and closes
 * the file each time (no fd is held across a wait). */
int artalb_read(const char *path, const struct artalb_hdr *expect,
                uint32_t first, int n, struct artalb_rec *out);
/* The panel set (LIBFILE_CAT_PANELSET) of the same file: up to 'max'
 * records into out, in the file's order. Checks the header as
 * artalb_read() does, the section's sum and every rep_idx. Returns the
 * number read (0: the file has no panel set), < 0 on error or
 * replacement. */
int artalb_panels(const char *path, const struct artalb_hdr *expect,
                  struct artalb_rec *out, int max);
/* #100: the pool order (LIBFILE_CAT_PANELSET col 1 after the books
 * count): its entries [first, first + max) into out (pinned[j]: 1 for
 * recently played or newest; may be NULL), the whole section read and its
 * sum checked each call (a few KB). *total: the pool's size. Returns the
 * number copied (0: the file has no pool), < 0 on error. */
int artalb_pool(const char *path, const struct artalb_hdr *expect,
                int first, int max, struct artalb_rec *out, uint8_t *pinned,
                int *total);

#endif /* _IPODUI_ARTFMT_H_ */
