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
/* The On-The-Go store (docs/phase11-brief.md 3.2, otg.h): a playlist of
 * its own, separate from what plays (⚠️ H3).
 *
 * Files (OTG_DIR, hidden; the format is in otg.h):
 *   otg.m3u8  the paths, one per line. Only complete lines count: a last
 *             line without '\n' (a power cut mid-append) is dropped, and
 *             cut off the file at the next sync. Empty and '#' lines are
 *             skipped (a list edited over USB).
 *   otg.idx   header + one record per line: the master index the line
 *             resolved to and its check (otg_check()).
 * Identity of an entry is its path; the index is a cache, valid while
 * the header's count and sum match and its commit id is the database's
 * (else the list resolves at its next open: ONE pass over the database's
 * file name tag file, Rev 1 M-2). Every file is written on the UI thread
 * only (each call here is a user action), so there is no cross-thread
 * race. An append writes the lines, then the records, then the header
 * (Rev 1 L-3): a cut leaves count < lines, i.e. a resolve.
 *
 * Buffers: the list strip (anim.h, 180,000 B), claimed by each call for
 * its duration and put back before it returns (nothing draws meanwhile:
 * these are actions between frames; the list redraws in full after, as
 * the strip's next claimer finds it not kept). Layout below; the screen
 * arena and the stack hold none of it. Statics: 32 B.
 *
 * Playing (otg_play()) builds a fresh current playlist from the paths,
 * as library.c play_view() does, and never hands otg.m3u8 to the
 * playlist engine (Rev 1 H-2): editing, clearing or saving the store
 * never touches what plays or its resume state. */
#include "config.h"
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include "string-extra.h"
#include "system.h"
#include "kernel.h"
#include "file.h"
#include "dir.h"
#include "tagcache.h"
#include "playlist.h"
#include "lang.h"
#include "logf.h"
#include "debug.h"
#include "ipodui/anim.h"
#include "ipodui/libfile.h"
#include "ipodui/library.h"
#include "ipodui/lib_shuffle.h"
#include "ipodui/otg.h"
#include "ipodui/safefile.h"

#define OTG_LIST_PATH   OTG_DIR "/" OTG_LIST
#define OTG_LIST_TMP    OTG_DIR "/otg.tmp"
#define OTG_IDX_TMP     OTG_DIR "/otg.itmp"
#define OTG_LIST_OLD    OTG_DIR "/otg.old"      /* safefile prevs (#91) */
#define OTG_IDX_OLD     OTG_DIR "/otg.iold"
#ifndef OTG_PL_DIR                              /* host tests: their own */
#define OTG_PL_DIR      "/Playlists"
#endif
#define OTG_SAVE_TMP    OTG_PL_DIR "/.otgsave.tmp"
#define OTG_TCD_FMT     "%s/database_%d.tcd"   /* tagcache.c TAGCACHE_FILE_INDEX */

/* The strip, while a call holds it */
#define OTG_RBUF        32768                   /* reads (lines, tag file) */
#define OTG_SET_SLOTS   4096                    /* power of 2, >= 2 x OTG_MAX */
#define OTG_SET_OFF     OTG_RBUF                /* 32 KB: the resolve's set */
#define OTG_IDX_OFF     (OTG_SET_OFF + OTG_SET_SLOTS * 8)
#define OTG_IDX_BYTES   (sizeof(struct otg_idx_hdr) \
                         + OTG_MAX * sizeof(struct otg_rec))
#define OTG_WBUF_OFF    (OTG_IDX_OFF + ALIGN_UP(OTG_IDX_BYTES, 32))
#define OTG_WBUF        32768                   /* writes (lines) */
#define OTG_STRIP_NEED  (OTG_WBUF_OFF + OTG_WBUF)
typedef char otg_strip_check[OTG_STRIP_NEED <= UI_STRIP_SIZE ? 1 : -1];
typedef char otg_hdr_check[sizeof(struct otg_idx_hdr) == 20
                           && sizeof(struct otg_rec) == 8 ? 1 : -1];

#define OTG_YIELD_EVERY 4096                    /* tag file entries */
#define otg_mkdir(p)    mkdir(p)

static struct {
    int8_t  synced;             /* lines/size known this boot */
    int8_t  idx_ok;             /* otg.idx holds 'lines' good records */
    int16_t pad;
    int32_t lines;              /* entries in otg.m3u8 */
    int32_t size;               /* its bytes */
    int32_t commitid;           /* the idx header's */
    struct libfile_sum_state sum;   /* of the records (idx_ok) */
    int     owner;              /* our strip owner id */
} otg;

struct io {                     /* per call: what the trace reports */
    int reads, writes;
};

/* -- helpers -------------------------------------------------------------- */

static uint32_t hash_fnv(const char *s, int len)
{
    uint32_t h = 2166136261u;

    while (len--)
        h = (h ^ (unsigned char)*s++) * 16777619u;
    return h;
}

static uint32_t hash_x31(const char *s, int len)
{
    uint32_t h = 0;

    while (len--)
        h = h * 31 + (unsigned char)*s++;
    return h;
}

uint32_t otg_check(int32_t idx)
{
    const int32_t *s = tagcache_ui_seeks(idx, NULL);
    const char *t;

    if (!s)
        return 0;
    t = tagcache_ui_str(tag_title, s[tag_title]);
    return (t ? hash_fnv(t, strlen(t)) : 0)
        ^ ((uint32_t)s[tag_length] * 0x9e3779b1u);
}

static char *strip_take(void)
{
    bool kept;

    if (!otg.owner)
        otg.owner = ui_strip_new_owner();
    return ui_strip_claim(otg.owner, &kept);
}

static struct otg_idx_hdr *idx_hdr(char *sb)
{
    return (struct otg_idx_hdr *)(void *)(sb + OTG_IDX_OFF);
}

static struct otg_rec *idx_recs(char *sb)
{
    return (struct otg_rec *)(void *)(sb + OTG_IDX_OFF
                                      + sizeof(struct otg_idx_hdr));
}

static uint32_t recs_sum(const struct otg_rec *r, int n)
{
    struct libfile_sum_state s;

    libfile_sum_init(&s);
    libfile_sum_add(&s, r, (size_t)n * sizeof(*r));
    return libfile_sum_end(&s);
}

/* -- reading the list ----------------------------------------------------- */

/* Complete lines from a file, read in OTG_RBUF chunks into the strip. */
struct lr {
    int fd;
    char *buf;
    int len, pos;
    bool eof;
    int tail;                   /* bytes after the last '\n' at the end */
    long bytes;                 /* read so far */
    struct io *io;
};

static void lr_init(struct lr *r, int fd, char *buf, struct io *io)
{
    memset(r, 0, sizeof(*r));
    r->fd = fd;
    r->buf = buf;
    r->io = io;
}

/* The next entry line, NUL-terminated in the buffer (valid until the
 * next call), or NULL at the end. */
static char *lr_next(struct lr *r, int *len)
{
    for (;;)
    {
        char *s = r->buf + r->pos;
        char *nl = memchr(s, '\n', r->len - r->pos);
        int rest, n;
        ssize_t got;

        if (nl)
        {
            n = nl - s;
            r->pos += n + 1;
            *nl = '\0';
            if (n && s[n - 1] == '\r')
                s[--n] = '\0';
            if (n == 0 || s[0] == '#')
                continue;
            *len = n;
            return s;
        }
        if (r->eof)
        {
            r->tail = r->len - r->pos;  /* torn: dropped */
            return NULL;
        }
        rest = r->len - r->pos;
        if (rest == OTG_RBUF)
            rest = 0;                   /* a line longer than a path: drop */
        memmove(r->buf, s, rest);
        r->len = rest;
        r->pos = 0;
        got = read(r->fd, r->buf + rest, OTG_RBUF - rest);
        r->io->reads++;
        if (got < OTG_RBUF - rest)
            r->eof = true;      /* a short read is the end: no probe read */
        if (got > 0)
        {
            r->len += got;
            r->bytes += got;
        }
    }
}

/* Reads otg.idx into the strip (one read) and checks it: header,
 * records, sum, and the file holding exactly 'count' records. Returns
 * the record count, or -1. */
static int idx_load(char *sb, struct io *io)
{
    struct otg_idx_hdr *h = idx_hdr(sb);
    ssize_t got;
    int fd = open(OTG_IDX, O_RDONLY);

    if (fd < 0)
        return -1;
    got = read(fd, sb + OTG_IDX_OFF, OTG_IDX_BYTES);
    io->reads++;
    close(fd);
    if (got < (ssize_t)sizeof(*h) || h->magic != OTG_MAGIC
        || h->version != OTG_VERSION || h->count > OTG_MAX
        || got != (ssize_t)(sizeof(*h) + h->count * sizeof(struct otg_rec))
        || recs_sum(idx_recs(sb), h->count) != h->sum)
        return -1;
    return h->count;
}

/* Learns the list's lines and size and whether the index matches it.
 * Cuts a torn last line off the list. */
static void sync_files(char *sb, struct io *io)
{
    struct lr r;
    int fd, n = 0, len, k, tl = 0;
    char tail[256];
    static bool recovered;

    otg.lines = otg.size = 0;
    otg.idx_ok = 0;
    if (!recovered)
    {
        /* safe-replace recovery (#91), once per boot before the first
         * read: a cut replace is put back, temporaries go */
        recovered = true;
        safefile_recover(OTG_LIST_PATH, OTG_LIST_TMP, OTG_LIST_OLD);
        safefile_recover(OTG_IDX, OTG_IDX_TMP, OTG_IDX_OLD);
        /* a cut inside the save's rename can leave the temporary on the
         * new playlist's chain (#91 H1): checked against /Playlists */
        safefile_remove_in(OTG_SAVE_TMP, OTG_PL_DIR);
    }
    fd = open(OTG_LIST_PATH, O_RDWR);
    if (fd >= 0)
    {
        lr_init(&r, fd, sb, io);
        while (lr_next(&r, &len))
            n++;
        otg.size = r.bytes - r.tail;
        if (r.tail > 0 && r.tail < (int)sizeof(tail))
        {
            tl = r.tail;                /* keep it: the index is read next */
            memcpy(tail, r.buf + r.pos, tl);
            tail[tl] = '\0';
        }
    }
    k = idx_load(sb, io);
    if (fd >= 0)
    {
        if (r.tail)
        {
            /* A last line with no newline. Our own appends always end in
             * '\n' and the index follows them, so a cut one leaves the index
             * matching the complete lines and a partial path. Anything else
             * (a list edited over USB, the index behind it, or a name that
             * exists) is a real entry: end it with a newline, don't cut it. */
            bool keep = false;

            if (tl > 0)
            {
                keep = k != n || file_exists(tail);
            }
            UPDATE_TRACE("otg torn tail=%d keep=%d", r.tail, (int)keep);
            if (keep)
            {
                if (lseek(fd, 0, SEEK_END) >= 0 && write(fd, "\n", 1) == 1)
                {
                    otg.size += r.tail + 1;
                    n++;
                }
            }
            else
                ftruncate(fd, otg.size);
            io->writes++;
        }
        close(fd);
    }
    otg.lines = n;
    libfile_sum_init(&otg.sum);
    if (k == n && k >= 0)
    {
        otg.idx_ok = 1;
        otg.commitid = idx_hdr(sb)->commitid;
        libfile_sum_add(&otg.sum, idx_recs(sb), (size_t)k * 8);
    }
    else if (k < 0 && n == 0)
    {
        otg.idx_ok = 1;                 /* nothing yet: a fresh index */
        otg.commitid = tagcache_ui_commitid();
    }
    otg.synced = 1;
}

/* The list's size on disk, or 0 if there is none. */
static long list_size(void)
{
    int fd = open(OTG_LIST_PATH, O_RDONLY);
    long n;

    if (fd < 0)
        return 0;
    n = lseek(fd, 0, SEEK_END);
    close(fd);
    return n;
}

/* Claims the strip and makes sure what we know matches the files (a
 * list edited over USB, the first call of a boot). */
static char *begin(struct io *io)
{
    char *sb = strip_take();

    if (!sb)
    {
        UPDATE_TRACE("otg no strip%s", "");
        return NULL;
    }
    if (!otg.synced || list_size() != otg.size)
        sync_files(sb, io);
    return sb;
}

/* Writes the whole index (header + records in the strip) through a
 * temporary file and a rename. */
static bool idx_write_all(char *sb, int n, int32_t commitid, struct io *io)
{
    struct otg_idx_hdr *h = idx_hdr(sb);
    size_t bytes = sizeof(*h) + (size_t)n * sizeof(struct otg_rec);
    bool ok;
    int fd;

    h->magic = OTG_MAGIC;
    h->version = OTG_VERSION;
    h->count = n;
    h->sum = recs_sum(idx_recs(sb), n);
    h->commitid = commitid;
    fd = open(OTG_IDX_TMP, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0)
        return false;
    ok = write(fd, sb + OTG_IDX_OFF, bytes) == (ssize_t)bytes;
    io->writes++;
    /* never a rename onto an existing name (two otg.idx entries on the
     * device, crash-fat 1019; #91) */
    ok = close(fd) == 0 && ok
         && safefile_replace(OTG_IDX_TMP, OTG_IDX, OTG_IDX_OLD) == 0;
    if (ok)
    {
        otg.idx_ok = 1;
        otg.commitid = commitid;
        libfile_sum_init(&otg.sum);
        libfile_sum_add(&otg.sum, idx_recs(sb), (size_t)n * 8);
    }
    else
        safefile_remove(OTG_IDX_TMP, OTG_IDX);
    return ok;
}

int otg_count(void)
{
    if (!otg.synced)
    {
        struct io io = { 0, 0 };
        char *sb = begin(&io);

        if (!sb)
            return 0;
        ui_strip_put();
    }
    return otg.lines;
}

/* -- adding --------------------------------------------------------------- */

/* Appends the tracks of track view 'th' (all of them, or capped). */
static int append_view(int th)
{
    struct io io = { 0, 0 };
    char *sb, *wbuf;
    struct otg_rec *recs;
    int n = lib_count(th), old, k, i, sh, fd, added = 0, wl = 0;
    long bytes = 0;
    bool ok = true;

    sb = begin(&io);
    if (!sb)
        return -1;
    old = otg.lines;
    k = MIN(n, OTG_MAX - old);
    if (k <= 0)
    {
        ui_strip_put();
        UPDATE_TRACE("otg add n=%d reads=%d writes=%d full=1", 0, io.reads,
                     io.writes);
        return 0;
    }
    wbuf = sb + OTG_WBUF_OFF;
    recs = idx_recs(sb);
    sh = lib_scratch_open();
    if (sh < 0)
    {
        ui_strip_put();
        return -1;
    }
    fd = open(OTG_LIST_PATH, O_WRONLY | O_CREAT | O_APPEND, 0666);
    if (fd < 0)
    {
        otg_mkdir(OTG_DIR);
        fd = open(OTG_LIST_PATH, O_WRONLY | O_CREAT | O_APPEND, 0666);
    }
    if (fd < 0)
    {
        lib_scratch_close(sh);
        ui_strip_put();
        UPDATE_TRACE("otg io %s failed", "list");
        return -1;
    }
    for (i = 0; i < k && ok; i++)
    {
        int idx = lib_id(th, i);
        const char *path = idx >= 0 ? lib_scratch_path(sh, idx) : NULL;
        int len;

        io.reads++;             /* the file name lookup */
        if (!path)
            continue;
        len = strlen(path);
        if (wl + len + 1 > OTG_WBUF)
        {
            ok = write(fd, wbuf, wl) == wl;
            io.writes++;
            wl = 0;
        }
        memcpy(wbuf + wl, path, len);
        wbuf[wl + len] = '\n';
        wl += len + 1;
        bytes += len + 1;
        recs[added].idx = idx;
        recs[added].check = otg_check(idx);
        added++;
    }
    if (ok && wl)
    {
        ok = write(fd, wbuf, wl) == wl;
        io.writes++;
    }
    ok = close(fd) == 0 && ok;
    lib_scratch_close(sh);
    if (!ok)
    {
        /* the list is whatever reached the disk: learn it again */
        otg.synced = 0;
        ui_strip_put();
        UPDATE_TRACE("otg io %s failed", "append");
        return -1;
    }
    otg.lines += added;
    otg.size += bytes;

    /* the index: the records, then the header (a cut between them is a
     * count < lines: a resolve) */
    if (otg.idx_ok && added)
    {
        struct otg_idx_hdr h;
        off_t at = sizeof(h) + (off_t)old * sizeof(struct otg_rec);

        fd = open(OTG_IDX, O_RDWR);
        if (fd < 0 && old == 0)
        {
            fd = open(OTG_IDX, O_RDWR | O_CREAT | O_TRUNC, 0666);
            memset(&h, 0, sizeof(h));   /* count 0 until the records are in */
            h.magic = OTG_MAGIC;
            h.version = OTG_VERSION;
            h.sum = recs_sum(recs, 0);
            h.commitid = otg.commitid = tagcache_ui_commitid();
            ok = fd >= 0 && write(fd, &h, sizeof(h)) == sizeof(h);
            io.writes++;
        }
        ok = ok && fd >= 0 && lseek(fd, at, SEEK_SET) == at
            && write(fd, recs, added * sizeof(*recs))
               == (ssize_t)(added * sizeof(*recs));
        io.writes++;
        if (ok)
        {
            libfile_sum_add(&otg.sum, recs, added * sizeof(*recs));
            h.magic = OTG_MAGIC;
            h.version = OTG_VERSION;
            h.count = otg.lines;
            h.sum = libfile_sum_end(&otg.sum);
            h.commitid = otg.commitid;
            ok = lseek(fd, 0, SEEK_SET) == 0
                && write(fd, &h, sizeof(h)) == sizeof(h);
            io.writes++;
        }
        if (fd >= 0)
            ok = close(fd) == 0 && ok;
        if (!ok)
            otg.idx_ok = 0;             /* resolved at the next open */
    }
    else
        otg.idx_ok = 0;
    ui_strip_put();
    UPDATE_TRACE("otg add n=%d reads=%d writes=%d", added, io.reads,
                 io.writes);
    return added;
}

int otg_add_row(int view, int idx)
{
    int th = lib_row_tracks(view, idx), rc;

    if (th <= 0)
        return -1;
    rc = append_view(th);
    lib_close(th);
    return rc;
}

int otg_add_track(long tc_idx)
{
    /* a one-row view of the track: the same path as a row */
    int v = lib_otg_view_new(1, 0), rc;
    int32_t *rows;

    if (v <= 0 || tc_idx <= 0)
    {
        if (v > 0)
            lib_close(v);
        return -1;
    }
    rows = lib_view_rows(v);
    rows[0] = tc_idx - 1;
    rc = otg_add_row(v, 0);
    lib_close(v);
    return rc;
}

/* -- clearing, removing, saving ------------------------------------------ */

static bool clear_files(struct io *io)
{
    struct otg_idx_hdr h;
    bool ok;
    int fd = open(OTG_LIST_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0666);

    if (fd < 0)
    {
        otg_mkdir(OTG_DIR);
        fd = open(OTG_LIST_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    }
    ok = fd >= 0;
    if (fd >= 0)
        close(fd);
    io->writes++;
    memset(&h, 0, sizeof(h));
    h.magic = OTG_MAGIC;
    h.version = OTG_VERSION;
    h.sum = recs_sum((const struct otg_rec *)&h, 0);
    h.commitid = tagcache_ui_commitid();
    fd = open(OTG_IDX, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    ok = ok && fd >= 0 && write(fd, &h, sizeof(h)) == sizeof(h);
    io->writes++;
    if (fd >= 0)
        close(fd);
    otg.lines = otg.size = 0;
    otg.idx_ok = 1;
    otg.commitid = h.commitid;
    libfile_sum_init(&otg.sum);
    otg.synced = 1;
    return ok;
}

int otg_clear(void)
{
    struct io io = { 0, 0 };
    bool ok = clear_files(&io);

    UPDATE_TRACE("otg clear%s", ok ? "" : " failed");
    return ok ? 0 : -1;
}

/* Writes the list without entry 'pos' to a temporary file, renamed over
 * the list; then the index without its record. */
int otg_remove(int pos)
{
    struct io io = { 0, 0 };
    struct lr r;
    char *sb = begin(&io), *wbuf;
    int in, out, i = 0, len, wl = 0;
    long bytes = 0;
    bool ok = true, had_idx;

    if (!sb)
        return -1;
    if (pos < 0 || pos >= otg.lines)
    {
        ui_strip_put();
        return -1;
    }
    had_idx = otg.idx_ok && idx_load(sb, &io) == otg.lines;
    wbuf = sb + OTG_WBUF_OFF;
    in = open(OTG_LIST_PATH, O_RDONLY);
    out = open(OTG_LIST_TMP, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (in < 0 || out < 0)
        ok = false;
    else
    {
        char *line;

        lr_init(&r, in, sb, &io);
        while (ok && (line = lr_next(&r, &len)))
        {
            if (i++ == pos)
                continue;
            if (wl + len + 1 > OTG_WBUF)
            {
                ok = write(out, wbuf, wl) == wl;
                io.writes++;
                wl = 0;
            }
            memcpy(wbuf + wl, line, len);
            wbuf[wl + len] = '\n';
            wl += len + 1;
            bytes += len + 1;
        }
        if (ok && wl)
        {
            ok = write(out, wbuf, wl) == wl;
            io.writes++;
        }
    }
    if (in >= 0)
        close(in);
    if (out >= 0)
        ok = close(out) == 0 && ok;
    ok = ok && safefile_replace(OTG_LIST_TMP, OTG_LIST_PATH, OTG_LIST_OLD) == 0;
    if (!ok)
    {
        safefile_remove(OTG_LIST_TMP, OTG_LIST_PATH);
        otg.synced = 0;
        ui_strip_put();
        UPDATE_TRACE("otg io %s failed", "remove");
        return -1;
    }
    otg.lines = i - 1;
    otg.size = bytes;
    if (had_idx)
    {
        struct otg_rec *recs = idx_recs(sb);

        memmove(recs + pos, recs + pos + 1,
                (size_t)(otg.lines - pos) * sizeof(*recs));
        if (!idx_write_all(sb, otg.lines, otg.commitid, &io))
            otg.idx_ok = 0;
    }
    else
        otg.idx_ok = 0;
    ui_strip_put();
    UPDATE_TRACE("otg remove pos=%d n=%d reads=%d writes=%d", pos,
                 otg.lines, io.reads, io.writes);
    return 0;
}

/* The first N for which "New Playlist N" (the language's form) names no
 * playlist in /Playlists: one directory read. */
static int first_free_n(const char *fmt)
{
    const char *d = strstr(fmt, "%d");
    uint32_t used[8];                   /* N 0..255 */
    int pre, n, max = 0;
    DIR *dir;
    struct dirent *e;

    memset(used, 0, sizeof(used));
    if (!d)
        return 1;
    pre = d - fmt;
    dir = opendir(OTG_PL_DIR);
    while (dir && (e = readdir(dir)))
    {
        const char *name = e->d_name, *p = name + pre, *dot;

        if (strncasecmp(name, fmt, pre))
            continue;
        for (n = 0; *p >= '0' && *p <= '9' && n < 100000; p++)
            n = n * 10 + (*p - '0');
        dot = strrchr(name, '.');
        if (p == name + pre || !dot
            || strncasecmp(p, d + 2, dot - p) || strlen(d + 2) != (size_t)(dot - p))
            continue;
        if (n < 256)
            used[n / 32] |= 1u << (n % 32);
        max = MAX(max, n);
    }
    if (dir)
        closedir(dir);
    for (n = 1; n < 256; n++)
        if (!(used[n / 32] & (1u << (n % 32))))
            return n;
    return max + 1;
}

int otg_save(char *name, size_t len)
{
    struct io io = { 0, 0 };
    char path[MAX_PATH], title[64];
    const char *fmt = str(LANG_IPODUI_NEW_PLAYLIST_N);
    char *sb = begin(&io);
    int in, out, n, saved;
    bool ok = true, stale_ok;

    if (len)
        name[0] = '\0';
    if (!sb)
        return -1;
    saved = otg.lines;
    if (saved <= 0)
    {
        ui_strip_put();
        return -1;
    }
    otg_mkdir(OTG_PL_DIR);
    n = first_free_n(fmt);
    snprintf(title, sizeof(title), fmt, n);
    snprintf(path, sizeof(path), OTG_PL_DIR "/%s.m3u8", title);
    /* a byte copy, in strip-sized pieces */
    in = open(OTG_LIST_PATH, O_RDONLY);
    /* #91 M1: a stale temp may be the second name of a saved playlist
     * (a cut inside the rename below): only its name goes before the
     * truncation. One lookup when there is none (the normal case). An
     * undecided check (I/O error) leaves it: no truncation then, the save
     * fails and is retried later */
    stale_ok = safefile_remove_in(OTG_SAVE_TMP, OTG_PL_DIR);
    out = stale_ok ? open(OTG_SAVE_TMP, O_WRONLY | O_CREAT | O_TRUNC, 0666)
                   : -1;
    ok = in >= 0 && out >= 0;
    while (ok)
    {
        ssize_t got = read(in, sb, OTG_RBUF);

        io.reads++;
        if (got <= 0)
            break;
        ok = write(out, sb, got) == got;
        io.writes++;
    }
    if (in >= 0)
        close(in);
    if (out >= 0)
        ok = close(out) == 0 && ok;
    /* a free name (first_free_n()); never onto a playlist that exists */
    ok = ok && !file_exists(path) && rename(OTG_SAVE_TMP, path) == 0;
    if (!ok)
    {
        if (stale_ok)       /* else it may name another playlist's data */
            safefile_remove(OTG_SAVE_TMP, path);
        ui_strip_put();
        UPDATE_TRACE("otg io %s failed", "save");
        return -1;
    }
    clear_files(&io);           /* G12: the saved list is a playlist now */
    ui_strip_put();
    if (len)
        strlcpy(name, title, len);
    UPDATE_TRACE("otg save name=%s n=%d", title, saved);
    return saved;
}

/* -- playing -------------------------------------------------------------- */

int otg_play(int pos)
{
    struct io io = { 0, 0 };
    struct playlist_insert_context c;
    struct lr r;
    char *sb = begin(&io), *line;
    int fd, len, i = 0, added = 0, start = 0;

    if (!sb)
        return -1;
    fd = open(OTG_LIST_PATH, O_RDONLY);
    if (fd < 0 || otg.lines <= 0 || playlist_create(NULL, NULL) < 0)
    {
        if (fd >= 0)
            close(fd);
        ui_strip_put();
        UPDATE_TRACE("otg play fail n=%d", otg.lines);
        return -1;
    }
    /* our own context with progress off: the default one splashes */
    if (playlist_insert_context_create(NULL, &c, PLAYLIST_INSERT_LAST,
                                       false, false) < 0)
    {
        playlist_insert_context_release(&c);
        close(fd);
        ui_strip_put();
        return -1;
    }
    lr_init(&r, fd, sb, &io);
    while (added < OTG_MAX && (line = lr_next(&r, &len)))
    {
        if (i++ == pos)
            start = added;
        if (playlist_insert_context_add(&c, line) >= 0)
            added++;
    }
    playlist_insert_context_release(&c);
    close(fd);
    ui_strip_put();
    UPDATE_TRACE("otg play n=%d pos=%d reads=%d", added, pos, io.reads);
    if (added == 0)
        return -1;
    lib_shuffle_note_book(false);
    playlist_start(start, 0, 0);
    return 0;
}

/* -- the list view -------------------------------------------------------- */

/* The set of the list's path hashes: slot = hash, hash2:16 | line+1:16 */
static void set_put(uint32_t *set, uint32_t h1, uint32_t h2, int line)
{
    uint32_t i = h1 & (OTG_SET_SLOTS - 1);

    while (set[2 * i + 1])
        i = (i + 1) & (OTG_SET_SLOTS - 1);
    set[2 * i] = h1;
    set[2 * i + 1] = (h2 << 16) | (uint32_t)(line + 1);
}

/* One pass over the database's file name tag file: every entry whose
 * path hashes like an unresolved line resolves it (Rev 1 M-2). Returns
 * the lines still unresolved, or -1 (the database changed: aborted). */
static int resolve(char *sb, int n, struct io *io, long *bytes)
{
    uint32_t *set = (uint32_t *)(void *)(sb + OTG_SET_OFF);
    struct otg_rec *recs = idx_recs(sb);
    char path[MAX_PATH];
    struct lr r;
    char *line, *buf = sb;
    int fd, len, i, gen = tagcache_ui_generation(), left = 0, pos, have;
    int steps = 0;
    bool eof = false;

    /* the set: the list's lines */
    memset(set, 0, OTG_SET_SLOTS * 8);
    fd = open(OTG_LIST_PATH, O_RDONLY);
    if (fd < 0)
        return n;
    lr_init(&r, fd, sb, io);
    for (i = 0; i < n && (line = lr_next(&r, &len)); i++)
    {
        recs[i].idx = -1;
        recs[i].check = 0;
        set_put(set, hash_fnv(line, len), hash_x31(line, len) & 0xffff, i);
    }
    close(fd);
    *bytes = r.bytes;
    for (; i < n; i++)
        recs[i].idx = -1;

    /* the tag file: 12 B header, then { tag_length, idx_id, data } */
    snprintf(path, sizeof(path), OTG_TCD_FMT, tagcache_ui_db_path(),
             (int)tag_filename);
    fd = open(path, O_RDONLY);
    if (fd < 0)
    {
        UPDATE_TRACE("otg io %s failed", "tagfile");
        return n;
    }
    if (lseek(fd, 12, SEEK_SET) != 12)  /* the header */
        eof = true;
    pos = have = 0;
    for (;;)
    {
        int32_t hd[2];
        int tl;

        if (have - pos < 8 || (memcpy(hd, buf + pos, 8), have - pos < 8 + hd[0]))
        {
            ssize_t got;
            int rest = have - pos;

            if (eof)
                break;
            if (rest < 0)
                rest = 0;
            memmove(buf, buf + pos, rest);
            have = rest;
            pos = 0;
            got = read(fd, buf + have, OTG_RBUF - have);
            io->reads++;
            if (got < OTG_RBUF - have)
                eof = true;     /* a short read is the end: no probe read */
            if (got > 0)
            {
                have += got;
                *bytes += got;
            }
            /* a yield happened in read(): the database may be gone */
            if (tagcache_ui_generation() != gen || tagcache_ui_count() < 0)
            {
                close(fd);
                return -1;
            }
            continue;
        }
        tl = hd[0];
        if (tl <= 0 || tl > OTG_RBUF - 8)
            break;                      /* damaged: stop here */
        if (++steps == OTG_YIELD_EVERY)
        {
            steps = 0;
            yield();
            if (tagcache_ui_generation() != gen || tagcache_ui_count() < 0)
            {
                close(fd);
                return -1;
            }
        }
        {
            const char *s = buf + pos + 8;
            const char *z = memchr(s, '\0', tl);
            int sl = z ? z - s : tl;
            uint32_t h1 = hash_fnv(s, sl);
            uint32_t h2 = hash_x31(s, sl) & 0xffff;
            uint32_t k = h1 & (OTG_SET_SLOTS - 1);

            while (set[2 * k + 1])
            {
                if (set[2 * k] == h1 && (set[2 * k + 1] >> 16) == h2)
                {
                    int ln = (set[2 * k + 1] & 0xffff) - 1;
                    if (recs[ln].idx < 0 && tagcache_ui_seeks(hd[1], NULL))
                    {
                        recs[ln].idx = hd[1];
                        recs[ln].check = otg_check(hd[1]);
                    }
                }
                k = (k + 1) & (OTG_SET_SLOTS - 1);
            }
        }
        pos += 8 + tl;
    }
    close(fd);
    for (i = 0; i < n; i++)
        left += recs[i].idx < 0;
    return left;
}

/* The file name (no folder, no extension) of each unresolved line, into
 * the view's name area; one more read of the list. */
static void names_fill(char *sb, int view, struct io *io)
{
    struct otg_rec *recs = idx_recs(sb);
    struct lr r;
    size_t cap, used = 1;               /* offset 0: "" */
    char *line;
    int fd, len, i = 0;

    if (!lib_otg_names(view, &cap) || cap < 2)
        return;
    fd = open(OTG_LIST_PATH, O_RDONLY);
    if (fd < 0)
        return;
    lr_init(&r, fd, sb, io);
    while (i < lib_count(view) && (line = lr_next(&r, &len)))
    {
        char *names = lib_otg_names(view, &cap);
        const char *base = strrchr(line, '/');
        const char *dot;
        size_t bl;

        if (recs[i].idx >= 0)
        {
            i++;
            continue;
        }
        base = base ? base + 1 : line;
        dot = strrchr(base, '.');
        bl = dot && dot > base ? (size_t)(dot - base) : strlen(base);
        bl = MIN(bl, 63);
        if (used + bl + 1 > cap)
            break;
        memcpy(names + used, base, bl);
        names[used + bl] = '\0';
        lib_view_rows(view)[i] = LIB_OTG_UNRES | used;
        used += bl + 1;
        i++;
    }
    close(fd);
}

int otg_open_view(void)
{
    struct io io = { 0, 0 };
    unsigned long t0 UNUSED_ATTR = current_tick;
    struct otg_rec *recs;
    char *sb = begin(&io);
    int n, k, i, v, unres = 0, left = -2;
    long bytes = 0;
    bool ready = tagcache_ui_count() >= 0, stale;

    if (!sb)
        return -1;
    n = otg.lines;
    recs = idx_recs(sb);
    k = otg.idx_ok && n ? idx_load(sb, &io) : -1;
    stale = k != n || (k > 0 && idx_hdr(sb)->commitid
                       != tagcache_ui_commitid());
    if (k != n)
        for (i = 0; i < n; i++)
            recs[i].idx = -1;
    for (i = 0; i < n && ready && !stale; i++)
        if (recs[i].idx >= 0 && otg_check(recs[i].idx) != recs[i].check)
            stale = true;               /* moved within one commit */
    if (ready && n && stale)
    {
        int before = io.reads;          /* the resolve's own reads */
        cpu_boost(true);
        left = resolve(sb, n, &io, &bytes);
        cpu_boost(false);
        before = io.reads - before;
        if (left >= 0)
            idx_write_all(sb, n, tagcache_ui_commitid(), &io);
        else
            for (i = 0; i < n; i++)
                recs[i].idx = -1;
        UPDATE_TRACE("otg resolve n=%d unresolved=%d bytes=%lu reads=%d "
                     "us=%lu", n, left, (unsigned long)bytes, before,
                     (unsigned long)(current_tick - t0) * (1000000 / HZ));
    }
    for (i = 0; i < n; i++)
        if (!ready || recs[i].idx < 0)
        {
            recs[i].idx = -1;
            unres++;
        }
    v = lib_otg_view_new(n, unres ? (size_t)unres * 32 + 8 : 0);
    if (v <= 0 && unres)
        v = lib_otg_view_new(n, 0);     /* no room: names left empty */
    if (v <= 0)
    {
        ui_strip_put();
        return -1;
    }
    for (i = 0; i < n; i++)
        lib_view_rows(v)[i] = recs[i].idx >= 0 ? recs[i].idx
                                               : LIB_OTG_UNRES;
    if (unres)
        names_fill(sb, v, &io);
    ui_strip_put();
    UPDATE_TRACE("otg open n=%d unresolved=%d reads=%d writes=%d", n, unres,
                 io.reads, io.writes);
    return v;
}
