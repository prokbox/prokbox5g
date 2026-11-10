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

/* syncart.c: see syncart.h. #225 S3, docs/design/ipod-control-support.md
 * §5 (format facts §5.1 from public documentation only). */
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#ifdef SYNCART_HOST                     /* tests/syncart: plain POSIX */
#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
int safefile_replace(const char *tmp, const char *dest, const char *prev);
#ifndef ROCKBOX_DIR
#error "tests/syncart/run.sh must pass the system folder path"
#endif
#else
#include "config.h"
#include "file.h"
#include "safefile.h"
#include "dir.h"
#endif
#include "artfmt.h"
#include "afb_core.h"
#include "artdb_core.h"
#include "syncart.h"

#define MAP_DIR     ROCKBOX_DIR "/artcache"
#define MAP_FILE    MAP_DIR "/syncart.map"
#define MAP_TMP     MAP_DIR "/syncart.tmp"
#define MAP_PREV    MAP_DIR "/syncart.prv"
#define EDGE_BYTES  8192                /* input CRC: first and last 8 KB */
#define WIN_BYTES   16384               /* streamed reads of the inputs */
#define BLOOM_BITS  (256 * 1024)        /* every song's id, k = 2: ~15 %
                                           false hits at 65,536 songs */
#define BLOOM_BYTES (BLOOM_BITS / 8)
#define PATH_MAX_   260
#ifndef MIN
#define MIN(a, b)   ((a) < (b) ? (a) : (b))
#endif

const char *syncart_root = "";

/* the song databases a cover database may belong to, in the order tried
 * (§5.2): the live one (a sync app's, or our developer fallback's,
 * whose covers then match it too), then the sync app's that the
 * developer fallback kept */
static const char *const song_dbs[] = {
    AFB_ITDB,
    ROCKBOX_DIR "/afb/iTunesDB.orig",
};
#define N_SONG_DBS ((int)(sizeof(song_dbs) / sizeof(song_dbs[0])))

static void full(char *buf, size_t len, const char *path)
{
    snprintf(buf, len, "%s%s", syncart_root, path);
}

uint64_t syncart_hash(const char *path)
{
    uint64_t h = 0xcbf29ce484222325ull;

    for (; *path; path++)
    {
        unsigned char c = *path;

        if (c >= 'a' && c <= 'z')
            c -= 'a' - 'A';
        h ^= c;
        h *= 0x100000001b3ull;
    }
    return h ? h : 1;
}

static bool prefix_ci(const char *s, const char *p)
{
    for (; *p; s++, p++)
    {
        unsigned char a = *s, b = *p;

        if (a >= 'a' && a <= 'z')
            a -= 'a' - 'A';
        if (b >= 'a' && b <= 'z')
            b -= 'a' - 'A';
        if (a != b)
            return false;
    }
    return true;
}

bool syncart_synced(const char *path)
{
    return prefix_ci(path, AFB_MUSIC_DIR "/")
           && !prefix_ci(path, AFB_LINK_DIR "/");
}

/* -- inputs' identity ----------------------------------------------------- */
static long file_len(const char *path)
{
    char p[PATH_MAX_];
    int fd;
    off_t n;

    full(p, sizeof(p), path);
    fd = open(p, O_RDONLY);
    if (fd < 0)
        return -1;
    n = lseek(fd, 0, SEEK_END);
    close(fd);
    return n < 0 || n > 0x7fffffffL ? -1 : (long)n;
}

/* CRC of the first and last EDGE_BYTES (all of it up to 2 x); the
 * caller's scratch, at least EDGE_BYTES */
static int edge_crc(const char *path, uint32_t *size, uint32_t *crc,
                    uint8_t *scratch)
{
    char p[PATH_MAX_];
    uint32_t c = ARTFMT_CRC_INIT;
    off_t len;
    int fd;

    full(p, sizeof(p), path);
    fd = open(p, O_RDONLY);
    if (fd < 0)
        return 0;                       /* none */
    len = lseek(fd, 0, SEEK_END);
    if (len < 0 || len > 0x7fffffffL)
        goto bad;
    for (int part = 0; part < 2; part++)
    {
        off_t at = part ? (len > 2 * EDGE_BYTES ? len - EDGE_BYTES
                                                : MIN((off_t)EDGE_BYTES, len))
                        : 0;
        off_t n = part ? len - at : MIN((off_t)EDGE_BYTES, len);

        if (n <= 0)
            continue;
        if (lseek(fd, at, SEEK_SET) != at
            || read(fd, scratch, n) != (ssize_t)n)
            goto bad;
        c = artfmt_crc(c, scratch, n);
    }
    close(fd);
    *size = (uint32_t)len;
    *crc = artfmt_crc_end(c);
    return 1;
bad:
    close(fd);
    return -1;
}

static uint32_t hdr_crc(const uint8_t *hdr)
{
    uint32_t c = artfmt_crc(ARTFMT_CRC_INIT, hdr,
                            offsetof(struct syncart_hdr, hdr_crc));

    c = artfmt_crc(c, hdr + sizeof(struct syncart_hdr),
                   SYNCART_MAX_FILES * SYNCART_NAME_MAX);
    return artfmt_crc_end(c);
}

/* the map's header block (512 B) into blk; false: none or bad */
static bool map_head(uint8_t *blk, struct syncart_hdr *h)
{
    char p[PATH_MAX_];
    int fd;
    bool ok;

    full(p, sizeof(p), MAP_FILE);
    fd = open(p, O_RDONLY);
    if (fd < 0)
        return false;
    ok = read(fd, blk, SYNCART_HDR_BYTES) == SYNCART_HDR_BYTES;
    if (ok)
    {
        off_t len = lseek(fd, 0, SEEK_END);

        memcpy(h, blk, sizeof(*h));
        ok = h->magic == SYNCART_MAGIC && h->version == SYNCART_VERSION
             && h->hdr_crc == hdr_crc(blk) && h->nfile <= SYNCART_MAX_FILES
             && len >= 0 && (uint64_t)len == SYNCART_HDR_BYTES
                + (uint64_t)h->n * sizeof(struct syncart_ent);
    }
    close(fd);
    return ok;
}

bool syncart_header(struct syncart_hdr *h, void *scratch, size_t len)
{
    return len >= SYNCART_HDR_BYTES && map_head(scratch, h);
}

static void map_remove(void)
{
    char p[PATH_MAX_];

    full(p, sizeof(p), MAP_FILE);
    remove(p);
    syncart_forget();
}

int syncart_check(uint32_t lib_commit, void *scratch, size_t len)
{
    uint8_t *edge = scratch;
    struct syncart_hdr h;
    uint32_t asz = 0, acrc = 0, dsz = 0, dcrc = 0;
    int rc, have_db = 0;

    if (len < EDGE_BYTES)
        return SYNCART_NOROOM;
    if (file_len(ARTDB_FILE) < 0)
    {
        if (file_len(MAP_FILE) >= 0)
            map_remove();
        return SYNCART_NONE;
    }
    for (int i = 0; i < N_SONG_DBS; i++)
        if (file_len(song_dbs[i]) >= 0)
            have_db = 1;
    if (!have_db)
    {
        if (file_len(MAP_FILE) >= 0)
            map_remove();
        return SYNCART_NONE;
    }
    if (!map_head(edge, &h) || h.db_which >= (uint32_t)N_SONG_DBS)
        return SYNCART_STALE_DBS;
    rc = edge_crc(ARTDB_FILE, &asz, &acrc, edge);
    if (rc > 0)
        rc = edge_crc(song_dbs[h.db_which], &dsz, &dcrc, edge);
    if (rc < 0)
        return SYNCART_ERR;
    if (rc == 0 || asz != h.art_size || acrc != h.art_crc
        || dsz != h.db_size || dcrc != h.db_crc)
        return SYNCART_STALE_DBS;
    return h.lib_commit != lib_commit ? SYNCART_STALE : SYNCART_SAME;
}

/* -- the build ------------------------------------------------------------ */
struct bent {                           /* 40 B, the build's form */
    uint64_t path_hash;                 /* the join with the song database */
    uint64_t key;                       /* the album's art key */
    uint64_t dbid;                      /* 0: no song found */
    uint32_t offset, size;
    uint8_t  slot_w, slot_h;
    uint8_t  file;                      /* 0xff: no image */
    uint8_t  rsv;
    int16_t  pad_v, pad_h;
};

struct win {
    int fd;
    uint8_t *buf;
    uint32_t at, n;
};

static int win_read(void *ctx, uint32_t off, void *p, uint32_t n)
{
    struct win *w = ctx;

    if (n > WIN_BYTES)
    {
        /* larger than the window: straight through */
        if (lseek(w->fd, off, SEEK_SET) != (off_t)off
            || read(w->fd, p, n) != (ssize_t)n)
            return -1;
        return 0;
    }
    if (off < w->at || off + n > w->at + w->n)
    {
        ssize_t got;

        if (lseek(w->fd, off, SEEK_SET) != (off_t)off)
            return -1;
        got = read(w->fd, w->buf, WIN_BYTES);
        if (got < (ssize_t)n)
            return -1;
        w->at = off;
        w->n = got;
    }
    memcpy(p, w->buf + (off - w->at), n);
    return 0;
}

struct build {
    struct bent *e;
    int n;
    uint8_t *bloom;
    bool (*pace)(void);
    uint32_t images, matched, songs, bad_len;
    uint32_t nfile;
    char files[SYNCART_MAX_FILES][SYNCART_NAME_MAX];
    long flen[SYNCART_MAX_FILES];
    bool stopped;
};

static inline uint32_t bh1(uint64_t id)
{
    return (uint32_t)(id ^ (id >> 29)) % BLOOM_BITS;
}
static inline uint32_t bh2(uint64_t id)
{
    return (uint32_t)((id * 0x9e3779b97f4a7c15ull) >> 40) % BLOOM_BITS;
}
static void bloom_add(uint8_t *b, uint64_t id)
{
    uint32_t a = bh1(id), c = bh2(id);

    b[a >> 3] |= 1u << (a & 7);
    b[c >> 3] |= 1u << (c & 7);
}
static bool bloom_has(const uint8_t *b, uint64_t id)
{
    uint32_t a = bh1(id), c = bh2(id);

    return (b[a >> 3] & (1u << (a & 7))) && (b[c >> 3] & (1u << (c & 7)));
}

static int cmp_hash(const void *a, const void *b)
{
    uint64_t x = ((const struct bent *)a)->path_hash;
    uint64_t y = ((const struct bent *)b)->path_hash;

    return x < y ? -1 : x > y;
}

static int cmp_key(const void *a, const void *b)
{
    uint64_t x = ((const struct bent *)a)->key;
    uint64_t y = ((const struct bent *)b)->key;

    return x < y ? -1 : x > y;
}

static int cmp_dbid(const void *a, const void *b)
{
    uint64_t x = ((const struct bent *)a)->dbid;
    uint64_t y = ((const struct bent *)b)->dbid;

    return x < y ? -1 : x > y;
}

static struct bent *find_hash(struct build *b, uint64_t h)
{
    int lo = 0, hi = b->n - 1;

    while (lo <= hi)
    {
        int mid = (lo + hi) / 2;

        if (b->e[mid].path_hash == h)
            return &b->e[mid];
        if (b->e[mid].path_hash < h)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return NULL;
}

/* the first entry with this id (several albums may share a song: never;
 * one song may stand for one album only, but be safe) */
static int find_dbid(struct build *b, uint64_t id)
{
    int lo = 0, hi = b->n;

    while (lo < hi)
    {
        int mid = (lo + hi) / 2;

        if (b->e[mid].dbid < id)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo < b->n && b->e[lo].dbid == id ? lo : -1;
}

static int song_cb(void *ctx, const struct artdb_song *s)
{
    struct build *b = ctx;
    struct bent *e;

    if ((++b->songs & 255) == 0 && b->pace && !b->pace())
    {
        b->stopped = true;
        return ARTDB_E_STOP;
    }
    if (!s->dbid)
        return 0;
    bloom_add(b->bloom, s->dbid);
    if (!syncart_synced(s->path))
        return 0;
    e = find_hash(b, syncart_hash(s->path));
    if (e)
        e->dbid = s->dbid;
    return 0;
}

static int file_index(struct build *b, const char *name)
{
    char p[PATH_MAX_];
    uint32_t i;

    for (i = 0; i < b->nfile; i++)
        if (!strcmp(b->files[i], name))
            return i;
    if (b->nfile >= SYNCART_MAX_FILES)
        return -1;
    strncpy(b->files[i], name, SYNCART_NAME_MAX - 1);
    b->files[i][SYNCART_NAME_MAX - 1] = '\0';
    snprintf(p, sizeof(p), ARTDB_DIR "/%s", name);
    b->flen[i] = file_len(p);
    b->nfile++;
    return i;
}

static int img_cb(void *ctx, const struct artdb_img *im)
{
    struct build *b = ctx;
    int k, f;

    if ((++b->images & 255) == 0 && b->pace && !b->pace())
    {
        b->stopped = true;
        return ARTDB_E_STOP;
    }
    if (bloom_has(b->bloom, im->dbid))
        b->matched++;
    k = find_dbid(b, im->dbid);
    if (k < 0 || !im->dbid)
        return 0;
    if (!artdb_img_ok(im))
        return 0;
    for (; k < b->n && b->e[k].dbid == im->dbid; k++)
    {
        struct bent *e = &b->e[k];

        /* the largest image wins (the 200 px one over the 100 px) */
        if (e->file != 0xff && (uint32_t)e->slot_w * e->slot_h
                               >= (uint32_t)im->slot_w * im->slot_h)
            continue;
        f = file_index(b, im->file);
        if (f < 0)
            return 0;
        if (b->flen[f] < 0 || (uint64_t)im->offset + im->size
                              > (uint64_t)b->flen[f])
        {
            b->bad_len++;               /* past the file's end: none */
            continue;
        }
        e->file = f;
        e->offset = im->offset;
        e->size = im->size;
        e->slot_w = im->slot_w;           /* <= ARTDB_MAX_SIDE (240) */
        e->slot_h = im->slot_h;
        e->pad_v = im->pad_v;
        e->pad_h = im->pad_h;
    }
    return 0;
}

static int stream(const char *path, uint8_t *winbuf, struct build *b,
                  bool songs)
{
    char p[PATH_MAX_];
    struct win w = { .buf = winbuf };
    struct afb_in in = { .ctx = &w, .read = win_read };
    off_t len;
    int rc;

    full(p, sizeof(p), path);
    w.fd = open(p, O_RDONLY);
    if (w.fd < 0)
        return ARTDB_E_IO;
    len = lseek(w.fd, 0, SEEK_END);
    if (len < 0 || len > 0x7fffffffL)
        rc = ARTDB_E_IO;
    else if (songs)
        rc = artdb_songs(&in, (uint32_t)len, song_cb, b);
    else
        rc = artdb_read(&in, (uint32_t)len, img_cb, b, NULL);
    close(w.fd);
    return rc;
}

size_t syncart_need(int n)
{
    return (size_t)(n > 0 ? n : 0) * sizeof(struct bent) + BLOOM_BYTES
           + WIN_BYTES + sizeof(struct build) + 16;
}

static int write_map(const uint8_t *hdrblk, struct build *b, uint32_t nout)
{
    char tmp[PATH_MAX_], dst[PATH_MAX_], prev[PATH_MAX_];
    int fd, i;
    bool ok;

    full(tmp, sizeof(tmp), MAP_TMP);
    full(dst, sizeof(dst), MAP_FILE);
    full(prev, sizeof(prev), MAP_PREV);
    fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0)
        return SYNCART_ERR;
    ok = write(fd, hdrblk, SYNCART_HDR_BYTES) == SYNCART_HDR_BYTES;
    for (i = 0; ok && i < b->n; i++)
    {
        const struct bent *e = &b->e[i];
        struct syncart_ent o;

        if (e->file == 0xff)
            continue;
        o.key = e->key;
        o.file = e->file;
        o.slot_w = e->slot_w;
        o.slot_h = e->slot_h;
        o.offset = e->offset;
        o.size = e->size;
        o.pad_v = e->pad_v;
        o.pad_h = e->pad_h;
        ok = write(fd, &o, sizeof(o)) == sizeof(o);
    }
    (void)nout;
    if (close(fd) < 0)
        ok = false;
    if (!ok || safefile_replace(tmp, dst, prev) != 0)
    {
        remove(tmp);
        return SYNCART_ERR;
    }
    syncart_forget();
    return SYNCART_BUILT;
}

int syncart_build(uint32_t lib_commit, int n,
                  bool (*path_of)(void *ctx, int i, char *buf, size_t len,
                                  uint64_t *key),
                  void *ctx, void *blk, size_t len, bool (*pace)(void),
                  struct syncart_hdr *st)
{
    uint8_t *base = (uint8_t *)(((uintptr_t)blk + 7) & ~(uintptr_t)7);
    struct build *b;
    uint8_t *winbuf, *hdrblk;
    struct syncart_hdr h;
    char path[PATH_MAX_];
    uint32_t asz = 0, acrc = 0, nout;
    int i, m, which = -1, last = 0, rc;
    bool bad = false;
    uint64_t key;

    if (len < syncart_need(n) || n < 0)
        return SYNCART_NOROOM;
    b = (struct build *)base;
    memset(b, 0, sizeof(*b));
    b->e = (struct bent *)(base + ((sizeof(*b) + 7) & ~(size_t)7));
    b->bloom = (uint8_t *)(b->e + n);
    winbuf = b->bloom + BLOOM_BYTES;
    hdrblk = winbuf;                    /* the header block, at the end */
    b->pace = pace;

    /* 1: the representatives under Music, by path hash */
    for (i = m = 0; i < n; i++)
    {
        if ((i & 63) == 63 && pace && !pace())
            return SYNCART_STOPPED;
        key = 0;
        if (!path_of(ctx, i, path, sizeof(path), &key) || !key
            || !syncart_synced(path))
            continue;
        memset(&b->e[m], 0, sizeof(b->e[m]));
        b->e[m].path_hash = syncart_hash(path);
        b->e[m].key = key;
        b->e[m].file = 0xff;
        m++;
    }
    qsort(b->e, m, sizeof(*b->e), cmp_hash);
    for (i = 0, b->n = 0; i < m; i++)   /* unique */
        if (!b->n || b->e[b->n - 1].path_hash != b->e[i].path_hash)
            b->e[b->n++] = b->e[i];
    memset(&h, 0, sizeof(h));
    h.albums = b->n;

    /* 2: each candidate song database until one matches the covers */
    rc = edge_crc(ARTDB_FILE, &asz, &acrc, winbuf);
    if (rc <= 0)
        return rc < 0 ? SYNCART_ERR : SYNCART_NONE;
    for (i = 0; i < N_SONG_DBS; i++)
    {
        int r;

        if (file_len(song_dbs[i]) < 0)
            continue;
        last = i;
        if (pace && !pace())
            return SYNCART_STOPPED;
        memset(b->bloom, 0, BLOOM_BYTES);
        for (m = 0; m < b->n; m++)
        {
            b->e[m].dbid = 0;
            b->e[m].file = 0xff;
        }
        b->images = b->matched = b->songs = 0;
        b->nfile = 0;
        qsort(b->e, b->n, sizeof(*b->e), cmp_hash);
        r = stream(song_dbs[i], winbuf, b, true);
        if (b->stopped)
            return SYNCART_STOPPED;
        if (r < 0)
            continue;                   /* unreadable: the next one */
        qsort(b->e, b->n, sizeof(*b->e), cmp_dbid);
        r = stream(ARTDB_FILE, winbuf, b, false);
        if (b->stopped)
            return SYNCART_STOPPED;
        if (r == ARTDB_E_IO)
            return SYNCART_ERR;
        if (r < 0)
        {
            bad = true;                 /* malformed: kept as such */
            h.images = h.matched = 0;
            break;
        }
        h.images = b->images;
        h.matched = b->matched;
        /* the match-rate check (§5.2, §11): fewer than half of the
         * covers name a song of this database: not its covers */
        if (b->images && 2 * (uint64_t)b->matched >= b->images)
        {
            which = i;
            break;
        }
    }
    if (which < 0 || bad)
    {
        /* none matched (or no covers): kept as such, entries none */
        if (h.images == 0 && h.matched == 0 && i == N_SONG_DBS)
            which = 0;
        for (m = 0; m < b->n; m++)
            b->e[m].file = 0xff;
        b->nfile = 0;
    }
    qsort(b->e, b->n, sizeof(*b->e), cmp_key);
    for (m = 0, nout = 0; m < b->n; m++)
        nout += b->e[m].file != 0xff;  /* keys: one album each */

    /* 3: the header (the song database's identity) and the entries */
    h.magic = SYNCART_MAGIC;
    h.version = SYNCART_VERSION;
    h.status = bad ? SYNCART_ST_BAD
             : which >= 0 && h.images && 2 * (uint64_t)h.matched >= h.images
               ? SYNCART_ST_OK : (h.images ? SYNCART_ST_MISMATCH
                                           : SYNCART_ST_OK);
    h.n = nout;
    h.nfile = b->nfile;
    h.lib_commit = lib_commit;
    h.art_size = asz;
    h.art_crc = acrc;
    h.db_which = which >= 0 ? which : last;
    rc = edge_crc(song_dbs[h.db_which], &h.db_size, &h.db_crc, winbuf);
    if (rc < 0)
        return SYNCART_ERR;
    memset(hdrblk, 0, SYNCART_HDR_BYTES);
    for (m = 0; m < (int)b->nfile; m++)
        memcpy(hdrblk + sizeof(h) + m * SYNCART_NAME_MAX, b->files[m],
               SYNCART_NAME_MAX);
    memcpy(hdrblk, &h, sizeof(h));
    h.hdr_crc = hdr_crc(hdrblk);
    memcpy(hdrblk, &h, sizeof(h));
    if (st)
        *st = h;
    {
        char d[PATH_MAX_];

        full(d, sizeof(d), MAP_DIR);
        mkdir(d
#ifdef SYNCART_HOST
              , 0777
#endif
              );
    }
    return write_map(hdrblk, b, nout);
}

void syncart_row_rgb(const uint8_t *row, int w, uint8_t *out)
{
    for (int x = 0; x < w; x++, row += 2, out += 4)
    {
        unsigned v = row[0] | ((unsigned)row[1] << 8);
        unsigned r = (v >> 11) & 0x1f, g = (v >> 5) & 0x3f, b = v & 0x1f;

        out[0] = (b << 3) | (b >> 2);
        out[1] = (g << 2) | (g >> 4);
        out[2] = (r << 3) | (r >> 2);
        out[3] = 0xff;
    }
}

/* -- lookups ---------------------------------------------------------------- */
static struct {
    bool tried, ok;
    uint32_t n, nfile;
} cache;

void syncart_forget(void)
{
    cache.tried = false;
}

#define LK_SPAN 32                      /* entries a probe reads (768 B) */

bool syncart_lookup(uint64_t want, struct syncart_ent *out,
                    char *file, size_t file_len_)
{
    struct syncart_ent span[LK_SPAN];
    char p[PATH_MAX_], name[SYNCART_NAME_MAX];
    long lo, hi;
    int fd, probes = 0;
    bool found = false;

    if (!want)
        return false;
    if (!cache.tried)
    {
        uint8_t blk[SYNCART_HDR_BYTES];
        struct syncart_hdr h;

        cache.tried = true;
        cache.ok = map_head(blk, &h) && h.status == SYNCART_ST_OK && h.n;
        cache.n = cache.ok ? h.n : 0;
        cache.nfile = cache.ok ? h.nfile : 0;
    }
    if (!cache.ok)
        return false;
    full(p, sizeof(p), MAP_FILE);
    fd = open(p, O_RDONLY);
    if (fd < 0)
        return false;
    lo = 0;
    hi = (long)cache.n - 1;
    /* interpolation: the hashes are uniform, so the first probe lands
     * within a few entries; each probe reads LK_SPAN entries around it */
    while (lo <= hi && probes++ < 24)
    {
        long guess, at, cnt, k;

        if (probes == 1)
            guess = (long)(((want >> 32) * (uint64_t)cache.n) >> 32);
        else
            guess = (lo + hi) / 2;
        if (guess < lo)
            guess = lo;
        if (guess > hi)
            guess = hi;
        at = guess - LK_SPAN / 2;
        if (at < lo)
            at = lo;
        cnt = MIN((long)LK_SPAN, hi - at + 1);
        if (lseek(fd, SYNCART_HDR_BYTES + at * (long)sizeof(*span),
                  SEEK_SET) < 0
            || read(fd, span, cnt * sizeof(*span))
               != (ssize_t)(cnt * sizeof(*span)))
            break;
        if (want < span[0].key)
            hi = at - 1;
        else if (want > span[cnt - 1].key)
            lo = at + cnt;
        else
        {
            for (k = 0; k < cnt; k++)
                if (span[k].key == want)
                {
                    *out = span[k];
                    found = true;
                }
            break;
        }
    }
    if (found)
    {
        /* the file's name from the table, checked as on build */
        found = out->file < cache.nfile
                && lseek(fd, sizeof(struct syncart_hdr)
                             + out->file * SYNCART_NAME_MAX, SEEK_SET) >= 0
                && read(fd, name, sizeof(name)) == sizeof(name);
        name[SYNCART_NAME_MAX - 1] = '\0';
        if (found)
        {
            struct artdb_img im;

            memset(&im, 0, sizeof(im));
            im.w = out->slot_w;         /* the slot, no padding: the */
            im.h = out->slot_h;         /* same geometry check as build */
            im.size = out->size;
            strcpy(im.file, name);
            found = artdb_img_ok(&im);
        }
        if (found && file)
            snprintf(file, file_len_, "%s" ARTDB_DIR "/%s", syncart_root,
                     name);
    }
    close(fd);
    return found;
}
