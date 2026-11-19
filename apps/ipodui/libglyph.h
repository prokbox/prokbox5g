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

/* iPod UI shell: the library glyph file (0.9.0, #235; contract from P0)
 *
 * Owner: package G (the CJK text drawing). Caller: package F (the
 * library's commit precompute). This header is the contract both build
 * against; apps/ipodui/libglyph.c is the shared stub until G lands its
 * writer and reader (docs/process.md, "contracts as real code").
 *
 * What it is (docs/design/cjk-text-and-sort-tags.md 5.3, on
 * p20-cjk-brief-rev): with every commit that precomputes the list files,
 * the list font's glyphs that the library's strings use are written, in
 * order of first appearance in the Artists, Albums and Songs lists, as
 * fixed-stride records of the companion font's sparse form, so scrolling
 * a CJK list reads forward in bulk (one 4 KB read per ~25 rows) and a
 * hard-drive unit never spins up per glyph. The file lives in the
 * candidate generation's folder (LIBGLYPH_NAME next to LIBFILE_NAME), is
 * written through safefile_replace() before the publish, and is published
 * or dropped with its generation.
 *
 * Writer rules (F's side):
 *  - one writer per commit, on the thread that writes the library file
 *    (the tagcache worker); never the UI or audio thread;
 *  - the caller supplies the writer state and its work buffer (a movable
 *    block it already holds for the precompute, refetched by the caller
 *    across yields as usual): libglyph allocates nothing and has no
 *    static buffers (two threads may parse at once, CLAUDE.md rule 4);
 *  - strings go in in list order: every Artists row, then Albums, then
 *    Songs (display string, UTF-8, as drawn); a string with no code point
 *    from U+3000 up costs one scan and nothing else;
 *  - libglyph_end(w, true) writes the header last and renames into place;
 *    libglyph_end(w, false) (USB, a power-off, a cancelled commit) leaves
 *    nothing behind but a .tmp the next begin overwrites;
 *  - a failure is logged and never stops the commit: lists draw from the
 *    glyph cache and the font file without it.
 *
 * Reader rules (G's side): libglyph_check() validates against the library
 * in RAM (commit id and entry count, as libfile's stamp) and the loaded
 * companion font (font id). Any mismatch (commit, entries, font, checksum,
 * size against count x stride, a short read) is LIBGLYPH_STALE: logged
 * "libglyph stale why=", the file is ignored until the next commit
 * rewrites it, never shown.
 *
 * Layout (package G; little-endian):
 *   0            struct libglyph_hdr, zero-padded to LIBGLYPH_PAGE;
 *   PAGE x (1+p) page p: per_page = PAGE / (stride + 2) code points
 *                (uint16, 0 past the end), then their per_page records
 *                (the list companion's records as they are in the font:
 *                1 width byte + the cell, firmware/export/font_sparse.h);
 *   PAGE x (1+pages)  the code-point table: count x uint16 in record
 *                order (the reader's rank map), padded to 4 bytes.
 * The reader keeps the table and a sorted permutation (4 B a glyph,
 * capped) and on a list font miss reads the glyph's whole page (one
 * aligned 4 KB read) into the glyph cache: the next ~20 rows' glyphs.
 *
 * The writer reads each new glyph's record from the list companion font
 * (LIBGLYPH_FONT): one small read per distinct glyph, at commit time
 * only; a library without CJK text costs one scan per string. The work
 * buffer holds the seen-bitmap, the font's index and the page. */
#ifndef _IPODUI_LIBGLYPH_H_
#define _IPODUI_LIBGLYPH_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LIBGLYPH_NAME     "database_glyph.dat"
#define LIBGLYPH_MAGIC    0x4c474c59u   /* "YLGL" little-endian: 'LGLY' */
#define LIBGLYPH_VERSION  1
/* the work buffer libglyph_begin() needs at least (G, 0.9.0): the seen
 * bitmap over the companion's windows (U+3000..U+D7A3, U+F900..U+FAFF,
 * U+FF00..U+FFEF: 5,460 B), the font's own index (bitmap and counts,
 * 5,804 B) and one 4 KB page */
#define LIBGLYPH_WORK_MIN (5460 + 5804 + 4096)
#define LIBGLYPH_PAGE     4096
/* the list companion the records come from (the 15 px list font's) */
#ifndef LIBGLYPH_FONT
#define LIBGLYPH_FONT     FONT_DIR "/14-ShellCJK-Bold.fnt"
#endif

/* The library the file is built for (libfile.h struct libfile_hdr's
 * commitid and entries) */
struct libglyph_stamp {
    int32_t commitid;
    int32_t entries;
};

/* On disk, little-endian, at offset 0; records follow at sizeof(hdr) */
struct libglyph_hdr {
    uint32_t magic;             /* LIBGLYPH_MAGIC */
    uint32_t version;           /* LIBGLYPH_VERSION */
    int32_t  commitid;          /* struct libglyph_stamp */
    int32_t  entries;
    uint32_t font_id;           /* CRC-32 of the list companion's header,
                                   bitmap and cut list (G computes it) */
    uint32_t count;             /* glyph records */
    uint32_t stride;            /* bytes per record: 1 width byte + cell */
    uint32_t flags;             /* 0 for now */
    uint32_t sum;               /* CRC-32 of the header with sum = 0 */
};

/* Writer state: the caller's (stack or its precompute block); fields are
 * G's, opaque to F */
struct libglyph_writer {
    int      fd;                /* the .tmp file, -1 when none */
    uint8_t *work;              /* caller's buffer, >= LIBGLYPH_WORK_MIN */
    size_t   work_len;
    uint32_t count;             /* distinct glyphs so far */
    uint32_t chunk_used;        /* records in the page being filled */
    struct libglyph_stamp stamp;
    int      font_fd;           /* LIBGLYPH_FONT, -1 when none */
    uint32_t stride;            /* the font's record bytes */
    uint32_t per_page;
    uint32_t font_id;
    bool     failed;            /* an I/O error: end() drops the file */
    char     dir[264];          /* the generation folder (DBGEN_PATH_MAX) */
};

enum libglyph_status {
    LIBGLYPH_OK = 0,
    LIBGLYPH_MISSING,           /* no file (or the stub) */
    LIBGLYPH_STALE,             /* a mismatch: ignored until rewritten */
    LIBGLYPH_ERROR              /* I/O or bad arguments */
};

/* F: start the file for the candidate in 'dir' (its generation folder).
 * 'work'/'len': the caller's buffer, at least LIBGLYPH_WORK_MIN bytes,
 * 4-byte aligned, untouched by anyone else until libglyph_end(). */
enum libglyph_status libglyph_begin(struct libglyph_writer *w,
                                    const char *dir,
                                    const struct libglyph_stamp *stamp,
                                    void *work, size_t len);
/* F: one list row's display string (UTF-8; NULL or "" is a no-op) */
enum libglyph_status libglyph_add(struct libglyph_writer *w,
                                  const char *utf8);
/* F: finish. commit = true writes the header and renames the file into
 * place; false drops it. Returns the distinct glyph count written (0: the
 * library holds no CJK text; F records that in the libfile stamp for the
 * full glyph tier, design 5.3), or < 0 on failure (logged, not fatal). */
int libglyph_end(struct libglyph_writer *w, bool commit);

/* G: validate the file in 'dir' against the library in RAM; on
 * LIBGLYPH_OK *hdr describes it. One header read. */
enum libglyph_status libglyph_check(const char *dir,
                                    const struct libglyph_stamp *want,
                                    struct libglyph_hdr *hdr);

/* G, the reader's rank map (pure, host-tested): perm[0..n) sorted so
 * cps[perm[i]] ascends; find returns the record number of cp, or -1 */
void libglyph_map_sort(const uint16_t *cps, uint16_t *perm, int n);
int  libglyph_map_find(const uint16_t *cps, const uint16_t *perm, int n,
                       uint32_t cp);

/* G, the draw side (target only, apps/ipodui/libglyph_fill.c): attach the
 * current library's file in 'dir' as the glyph source of the list
 * companion font comp_id (font_set_fill()), at most max_glyphs of its
 * table resident; detach with dir NULL. Returns the file's glyph count
 * (0: no CJK text, or no usable file), the allocation in *bytes. */
int libglyph_attach(const char *dir, const struct libglyph_stamp *want,
                    int comp_id, int max_glyphs, size_t *bytes);

#endif /* _IPODUI_LIBGLYPH_H_ */
