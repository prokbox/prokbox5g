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

/* iPod UI shell: the library glyph file, writer and checker (0.9.0, #235)
 *
 * The contract and the file layout are in libglyph.h; the font form in
 * firmware/export/font_sparse.h. Called by the library's commit
 * precompute on the tagcache worker (package F); checked and read by the
 * draw side (libglyph_fill.c). No allocation and no static buffer: the
 * caller's work buffer holds everything (two threads may run a writer).
 *
 * Built for the target and, with LIBGLYPH_HOST, by tests/p090 on the host
 * (POSIX file calls; the test supplies safefile_replace() and the font
 * path). */
#include <string.h>
#include <stdio.h>
#ifdef LIBGLYPH_HOST
#include <fcntl.h>
#include <unistd.h>
#define LGLOG(...) do { } while (0)
extern const char *libglyph_font_path;
#undef LIBGLYPH_FONT
#define LIBGLYPH_FONT libglyph_font_path
#else
#include "config.h"
#include "file.h"
#include "rbpaths.h"
#include "ipodui/uilog.h"
/* rare events (a stale file, a failed write): the shell log, which
 * reaches ipodui.log on the device */
#define LGLOG(...) ui_log("libglyph", __VA_ARGS__)
#endif
#include "font_sparse.h"
#include "safefile.h"
#include "libglyph.h"

#ifndef O_BINARY
#define O_BINARY 0
#endif

/* work buffer: the seen bitmap, the font's index, the page */
#define W_SEEN(w)   ((w)->work)
#define W_MAP(w)    ((w)->work + FONT_SPARSE_BITMAP)
#define W_CNT(w)    ((const uint16_t *)(W_MAP(w) + FONT_SPARSE_BITMAP)) /* align-ok: 2 x 5,460 B into a buffer libglyph_begin() refuses unless 4-aligned */
#define W_PAGE(w)   ((w)->work + FONT_SPARSE_BITMAP + FONT_SPARSE_INDEX)

static bool write_all(int fd, const void *p, size_t n)
{
    return write(fd, p, n) == (ssize_t)n;
}

static bool read_at(int fd, long at, void *p, size_t n)
{
    return lseek(fd, at, SEEK_SET) == at && read(fd, p, n) == (ssize_t)n;
}

/* the sparse font's sub-header after its RB12 header; false: not one */
static bool font_sub(int fd, struct font_sparse_sub *sub)
{
    uint8_t h[FONT_SPARSE_SUBOFF];

    return read_at(fd, 0, h, sizeof(h)) && !memcmp(h, "RB12", 4)
        && ((h[10] | h[11] << 8) & FONT_SPARSE_DEPTH)
        && read(fd, sub, sizeof(*sub)) == (ssize_t)sizeof(*sub)
        && sub->magic == FONT_SPARSE_MAGIC && sub->stride > 1
        && sub->stride + 2 <= LIBGLYPH_PAGE;
}

static uint32_t hdr_sum(const struct libglyph_hdr *h)
{
    struct libglyph_hdr c = *h;

    c.sum = 0;
    return font_sparse_crc(0, &c, sizeof(c));
}

/* "<dir>/<LIBGLYPH_NAME><suffix>" into buf (in the work buffer) */
static char *name_in(char *buf, size_t len, const char *dir,
                     const char *suffix)
{
    snprintf(buf, len, "%s/%s%s", dir, LIBGLYPH_NAME, suffix);
    return buf;
}

static long file_bytes(uint32_t count, uint32_t per_page)
{
    long pages = per_page ? (long)((count + per_page - 1) / per_page) : 0;
    return LIBGLYPH_PAGE * (1 + pages) + (long)((count * 2 + 3) & ~3u);
}

enum libglyph_status libglyph_begin(struct libglyph_writer *w,
                                    const char *dir,
                                    const struct libglyph_stamp *stamp,
                                    void *work, size_t len)
{
    struct font_sparse_sub sub;
    char *tmp;

    if (!w || !dir || !stamp || !work || len < LIBGLYPH_WORK_MIN
        || ((uintptr_t)work & 3) || strlen(dir) >= sizeof(w->dir))
        return LIBGLYPH_ERROR;
    memset(w, 0, sizeof(*w));
    w->fd = w->font_fd = -1;
    w->stamp = *stamp;
    strcpy(w->dir, dir);
    w->work = work;
    w->work_len = len;

    w->font_fd = open(LIBGLYPH_FONT, O_RDONLY | O_BINARY);
    if (w->font_fd < 0 || !font_sub(w->font_fd, &sub)
        || !read_at(w->font_fd, FONT_SPARSE_MAPOFF, W_MAP(w),
                    FONT_SPARSE_INDEX))
    {
        LGLOG("no list font (%d)", w->font_fd);
        goto fail;
    }
    w->stride = sub.stride;
    w->font_id = sub.font_id;
    w->per_page = LIBGLYPH_PAGE / (sub.stride + 2);

    tmp = name_in((char *)W_PAGE(w), LIBGLYPH_PAGE, dir, ".tmp");
    w->fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_BINARY, 0666);
    if (w->fd < 0)
    {
        LGLOG("open %s failed", tmp);
        goto fail;
    }
    memset(W_SEEN(w), 0, FONT_SPARSE_BITMAP);
    memset(W_PAGE(w), 0, LIBGLYPH_PAGE);
    if (!write_all(w->fd, W_PAGE(w), LIBGLYPH_PAGE))   /* the header page */
        w->failed = true;
    return LIBGLYPH_OK;
fail:
    if (w->font_fd >= 0)
        close(w->font_fd);
    w->font_fd = -1;
    w->work = NULL;             /* add()/end() refuse: nothing to drop */
    return LIBGLYPH_ERROR;
}

/* the next code point of a UTF-8 string (malformed bytes: U+FFFD) */
static uint32_t utf8_next(const unsigned char **ps)
{
    const unsigned char *s = *ps;
    uint32_t c = *s++;
    int more = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;

    if (c >= 0x80 && !more)
        c = 0xFFFD;
    else if (more)
        c &= 0x3F >> more;
    while (more--)
    {
        if ((*s & 0xC0) != 0x80)
        {
            c = 0xFFFD;
            break;
        }
        c = c << 6 | (*s++ & 0x3F);
    }
    *ps = s;
    return c;
}

static void page_flush(struct libglyph_writer *w)
{
    if (!w->chunk_used)
        return;
    if (!write_all(w->fd, W_PAGE(w), LIBGLYPH_PAGE))
        w->failed = true;
    memset(W_PAGE(w), 0, LIBGLYPH_PAGE);
    w->chunk_used = 0;
}

enum libglyph_status libglyph_add(struct libglyph_writer *w,
                                  const char *utf8)
{
    const unsigned char *s = (const unsigned char *)utf8;

    if (!w || !w->work)
        return LIBGLYPH_ERROR;
    if (!s || w->failed)
        return LIBGLYPH_OK;
    while (*s)
    {
        uint32_t c;
        int bit, n;
        uint8_t *page;

        if (*s < 0xE3)          /* below U+3000: one compare a byte */
        {
            s++;
            continue;
        }
        c = utf8_next(&s);
        bit = font_sparse_bit(c);
        if (!font_sparse_has(W_MAP(w), bit) || font_sparse_has(W_SEEN(w), bit))
            continue;
        W_SEEN(w)[bit >> 3] |= 1 << (bit & 7);
        if (w->count >= 0xFFFF)
            continue;           /* never: the cut holds ~18.4k */
        n = font_sparse_rank(W_MAP(w), W_CNT(w), bit);
        page = W_PAGE(w);
        ((uint16_t *)page)[w->chunk_used] = (uint16_t)c;
        if (!read_at(w->font_fd, FONT_SPARSE_DATA + (long)n * w->stride,
                     page + 2 * w->per_page + w->chunk_used * w->stride,
                     w->stride))
        {
            LGLOG("font read failed");
            w->failed = true;
            return LIBGLYPH_ERROR;
        }
        w->count++;
        if (++w->chunk_used == w->per_page)
            page_flush(w);
    }
    return LIBGLYPH_OK;
}

/* the code-point table: each page's head read back, appended at the end
 * (the seen bitmap and the font index are free by now: 11 KB of room) */
static bool write_table(struct libglyph_writer *w)
{
    uint8_t *acc = w->work;
    const uint32_t room = (FONT_SPARSE_BITMAP + FONT_SPARSE_INDEX) & ~3u;
    uint32_t pages = (w->count + w->per_page - 1) / w->per_page;
    uint32_t used = 0, p, done = 0;
    long tail = LIBGLYPH_PAGE * (1 + (long)pages);

    for (p = 0; p < pages; p++)
    {
        uint32_t n = w->count - done < w->per_page
                   ? w->count - done : w->per_page;
        if (used + 2 * n > room)
        {
            if (lseek(w->fd, tail, SEEK_SET) != tail
                || !write_all(w->fd, acc, used))
                return false;
            tail += used;
            used = 0;
        }
        if (!read_at(w->fd, LIBGLYPH_PAGE * (1 + (long)p), acc + used, 2 * n))
            return false;
        used += 2 * n;
        done += n;
    }
    while (used & 3)
        acc[used++] = 0;
    return lseek(w->fd, tail, SEEK_SET) == tail && write_all(w->fd, acc, used);
}

int libglyph_end(struct libglyph_writer *w, bool commit)
{
    struct libglyph_hdr h;
    char *tmp, *dest, *prev;
    int ret = -1;

    if (!w || !w->work)
        return -1;
    if (w->font_fd >= 0)
        close(w->font_fd);
    w->font_fd = -1;
    if (commit && !w->failed)
    {
        page_flush(w);
        memset(&h, 0, sizeof(h));
        h.magic = LIBGLYPH_MAGIC;
        h.version = LIBGLYPH_VERSION;
        h.commitid = w->stamp.commitid;
        h.entries = w->stamp.entries;
        h.font_id = w->font_id;
        h.count = w->count;
        h.stride = w->stride;
        h.sum = hdr_sum(&h);
        /* the file's own fd reads the pages back: it was opened write-
         * only, so reopen it for the table pass */
        close(w->fd);
        tmp = name_in((char *)W_PAGE(w), 300, w->dir, ".tmp");
        w->fd = open(tmp, O_RDWR | O_BINARY);
        if (w->fd >= 0 && !w->failed && (!w->count || write_table(w))
            && lseek(w->fd, 0, SEEK_SET) == 0
            && write_all(w->fd, &h, sizeof(h))
            && lseek(w->fd, 0, SEEK_END) == file_bytes(h.count, w->per_page))
        {
            fsync(w->fd);
            close(w->fd);
            w->fd = -1;
            tmp = name_in((char *)W_PAGE(w), 300, w->dir, ".tmp");
            dest = name_in((char *)W_PAGE(w) + 300, 300, w->dir, "");
            prev = name_in((char *)W_PAGE(w) + 600, 300, w->dir, ".old");
            if (safefile_replace(tmp, dest, prev) == 0)
                ret = (int)w->count;
        }
        if (ret < 0)
            LGLOG("write failed, n=%lu", (unsigned long)w->count);
    }
    else
        ret = commit ? -1 : 0;
    if (w->fd >= 0)
        close(w->fd);
    w->fd = -1;
    if (ret < 0 || !commit)
        remove(name_in((char *)W_PAGE(w), 300, w->dir, ".tmp"));
    w->work = NULL;             /* the buffer is the caller's again */
    return ret;
}

enum libglyph_status libglyph_check(const char *dir,
                                    const struct libglyph_stamp *want,
                                    struct libglyph_hdr *hdr)
{
    char path[300];
    struct font_sparse_sub sub;
    const char *why = NULL;
    long size;
    int fd;

    if (!dir || !want || !hdr)
        return LIBGLYPH_ERROR;
    fd = open(name_in(path, sizeof(path), dir, ""), O_RDONLY | O_BINARY);
    if (fd < 0)
        return LIBGLYPH_MISSING;
    size = lseek(fd, 0, SEEK_END);
    if (!read_at(fd, 0, hdr, sizeof(*hdr)))
        why = "short";
    close(fd);
    if (!why && (hdr->magic != LIBGLYPH_MAGIC
                 || hdr->version != LIBGLYPH_VERSION))
        why = "magic";
    else if (!why && hdr->sum != hdr_sum(hdr))
        why = "sum";
    else if (!why && hdr->commitid != want->commitid)
        why = "commit";
    else if (!why && hdr->entries != want->entries)
        why = "entries";
    else if (!why && (hdr->stride < 2 || hdr->stride + 2 > LIBGLYPH_PAGE
                      || size != file_bytes(hdr->count,
                                            LIBGLYPH_PAGE / (hdr->stride + 2))))
        why = "size";
    if (!why)
    {
        fd = open(LIBGLYPH_FONT, O_RDONLY | O_BINARY);
        if (fd < 0 || !font_sub(fd, &sub) || sub.font_id != hdr->font_id
            || sub.stride != hdr->stride)
            why = "font";
        if (fd >= 0)
            close(fd);
    }
    if (why)
    {
        LGLOG("stale why=%s", why);
        return LIBGLYPH_STALE;
    }
    return LIBGLYPH_OK;
}

/* heapsort of the permutation by code point (no qsort context in C99) */
static void sift(const uint16_t *cps, uint16_t *perm, int i, int n)
{
    for (;;)
    {
        int c = 2 * i + 1;
        uint16_t t;

        if (c >= n)
            return;
        if (c + 1 < n && cps[perm[c + 1]] > cps[perm[c]])
            c++;
        if (cps[perm[i]] >= cps[perm[c]])
            return;
        t = perm[i];
        perm[i] = perm[c];
        perm[c] = t;
        i = c;
    }
}

void libglyph_map_sort(const uint16_t *cps, uint16_t *perm, int n)
{
    int i;

    for (i = 0; i < n; i++)
        perm[i] = (uint16_t)i;
    for (i = n / 2 - 1; i >= 0; i--)
        sift(cps, perm, i, n);
    for (i = n - 1; i > 0; i--)
    {
        uint16_t t = perm[0];
        perm[0] = perm[i];
        perm[i] = t;
        sift(cps, perm, 0, i);
    }
}

int libglyph_map_find(const uint16_t *cps, const uint16_t *perm, int n,
                      uint32_t cp)
{
    int lo = 0, hi = n - 1;

    while (lo <= hi)
    {
        int mid = (lo + hi) / 2;
        uint32_t v = cps[perm[mid]];

        if (v == cp)
            return perm[mid];
        if (v < cp)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return -1;
}
