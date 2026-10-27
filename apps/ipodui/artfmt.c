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
 * iPod UI shell: album art cache on-disk format (see artfmt.h)
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
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <stdio.h>
#ifdef ARTFMT_HOST                      /* tests/artcache: plain POSIX */
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#else
#include "config.h"
#include "file.h"
#include "crc32.h"
#endif
#include "artfmt.h"
#ifndef ARTFMT_HOST
#include "uilog.h"
#include "safefile.h"
#endif

/* p19-art-yield: the art builder's pace point (artcache.c, art_pace_at())
 * between the steps of a walk over a whole file: a page read, a table
 * chunk. Only ever before a read or a write (one may block and switch
 * anyway), and never while a caller's unpinned buffer is held (the GC
 * key buffer is fetched again after each read). The host tests: none. */
#ifdef ARTFMT_HOST
#define ARTFMT_PACE() do { } while (0)
#else
void (*artfmt_pace)(int line);
static void fmt_pace(int line)
{
    if (artfmt_pace)
        artfmt_pace(line);
}
#ifdef SIMULATOR
#define ARTFMT_PACE() fmt_pace(__LINE__)    /* (the trace's point) */
#else
#define ARTFMT_PACE() fmt_pace(0)
#endif
#endif

/* #91 H1: a cut inside a rename can leave two names on one cluster chain
 * (path and prev, or tmp and path); removing or truncating either frees
 * the other's data. Every removal of a name whose partner may exist goes
 * through safefile.c, which drops only the second name. Host files never
 * share a chain: a plain removal there (safefile.c not linked). */
#ifdef ARTFMT_HOST
static bool art_remove(const char *path, const char *keep)
{
    (void)keep;
    return remove(path) == 0 || errno == ENOENT;
}
static int art_drop_alias(const char *path, const char *keep)
{
    (void)path; (void)keep;
    return 0;
}
#else
#define art_remove(path, keep)     safefile_remove(path, keep)
#define art_drop_alias(path, keep) safefile_drop_alias(path, keep)
#endif

#ifdef ARTFMT_HOST
static bool test_fail(enum artfmt_test_op op);
#endif
/* artfile_publish(): does the primary exist. 1 yes, 0 not found, -1 any
 * other open error (errno kept): not "missing", so no rename runs on a
 * guess (a rename onto a name that is there after all is what left
 * duplicate entries). Its own fault op in the host tests, outside the
 * open counters: a probe, not an I/O step under test. */
static int artfile_exists(const char *path)
{
    int fd;
#ifdef ARTFMT_HOST
    if (test_fail(ARTFMT_TEST_PROBE))
        fd = -1;
    else
#endif
    fd = open(path, O_RDONLY);
    if (fd < 0)
        return errno == ENOENT ? 0 : -1;
    close(fd);
    return 1;
}

#ifdef ARTFMT_HOST
static uint64_t test_fault[ARTFMT_TEST_NOPS];
static int test_errno[ARTFMT_TEST_NOPS];
void artfmt_test_fail(enum artfmt_test_op op, uint64_t mask, int error)
{
    test_fault[op] = mask;
    test_errno[op] = error;
}
void artfmt_test_reset(void)
{
    memset(test_fault, 0, sizeof(test_fault));
}
static bool test_fail(enum artfmt_test_op op)
{
    bool fail = (test_fault[op] & 1) != 0;
    test_fault[op] >>= 1;
    if (fail)
        errno = test_errno[op];
    return fail;
}
static int test_open(const char *p, int flags, ...)
{
    return test_fail(ARTFMT_TEST_OPEN) ? -1 : open(p, flags, 0666);
}
static ssize_t test_read(int fd, void *p, size_t n)
{
    if (test_fail(ARTFMT_TEST_READ))
        return -1;
    if (test_fail(ARTFMT_TEST_SHORT_READ))
        n /= 2;
    return read(fd, p, n);
}
static ssize_t test_write(int fd, const void *p, size_t n)
{
    if (test_fail(ARTFMT_TEST_WRITE))
        return -1;
    if (test_fail(ARTFMT_TEST_SHORT_WRITE))
        n /= 2;
    return write(fd, p, n);
}
static int test_flush(int fd)
{
    return test_fail(ARTFMT_TEST_FLUSH) ? -1 : fsync(fd);
}
static int test_rename(const char *a, const char *b)
{
    return test_fail(ARTFMT_TEST_RENAME) ? -1 : rename(a, b);
}
static int test_close(int fd)
{
    bool fail = test_fail(ARTFMT_TEST_CLOSE);
    int rc = close(fd); /* close the descriptor even when reporting failure */
    return fail ? -1 : rc;
}
#define open test_open
#define read test_read
#define write test_write
#define fsync test_flush
#define rename test_rename
#define close test_close
#endif

/* Thumbnail: 55x55, the two-line row's art box (owner photos
 * albums-list-twoline-b, artist-albums*; 4 rows in the 220 px body).
 * Panel: 300x300 for the 160x240 split-screen panel (x 160-319, full
 * height): a pan-only drift at up to 8 px/s over a ~7 s dwell travels up
 * to 56 px, and the tight axis is the height, 240 + 56 = 296 <= 300 (the
 * width leaves 140 px). Square, so any direction works; 600 B rows keep
 * blits word-aligned. The others follow artcache.md §2. */
const struct artcache_dim artcache_sizes[ARTCACHE_NUM_SIZES] = {
    [ARTCACHE_THUMB]      = {  55,  55 },
    [ARTCACHE_COVERFLOW]  = { 128, 128 },
    [ARTCACHE_NOWPLAYING] = { 142, 142 },   /* a box: artcache_size_fits() */
    [ARTCACHE_PANEL]      = { 300, 300 },
};

typedef char artpanel_hdr_size_check[
    sizeof(struct artpanel_hdr) == 792
    && sizeof(struct artpanel_hdr) <= ARTPANEL_HDR_BYTES
    && sizeof(struct artpanel_ent) == 24 ? 1 : -1];

/* --- CRC ---------------------------------------------------------------- */
uint32_t artfmt_crc(uint32_t crc, const void *p, size_t n)
{
#ifdef ARTFMT_HOST
    /* the same nibble table as firmware/common/crc32.c crc_32r() */
    static const uint32_t t[16] = {
        0x00000000, 0x1DB71064, 0x3B6E20C8, 0x26D930AC,
        0x76DC4190, 0x6B6B51F4, 0x4DB26158, 0x5005713C,
        0xEDB88320, 0xF00F9344, 0xD6D6A3E8, 0xCB61B38C,
        0x9B64C2B0, 0x86D3D2D4, 0xA00AE278, 0xBDBDF21C
    };
    const unsigned char *b = p;

    while (n--)
    {
        crc ^= *b++;
        crc = t[crc & 15] ^ (crc >> 4);
        crc = t[crc & 15] ^ (crc >> 4);
    }
    return crc;
#else
    return crc_32r(p, n, crc);
#endif
}

static uint32_t crc_of(const void *p, size_t n)
{
    return artfmt_crc_end(artfmt_crc(ARTFMT_CRC_INIT, p, n));
}

/* --- headers ------------------------------------------------------------ */
void artidx_hdr_seal(struct artidx_hdr *h)
{
    h->magic = ARTIDX_MAGIC;
    h->version = ARTIDX_VERSION;
    h->hdr_crc = crc_of(h, offsetof(struct artidx_hdr, hdr_crc));
}

bool artidx_hdr_ok(const struct artidx_hdr *h)
{
    return h->magic == ARTIDX_MAGIC && h->version == ARTIDX_VERSION
        && h->hdr_crc == crc_of(h, offsetof(struct artidx_hdr, hdr_crc))
        && h->count <= ARTIDX_MAX_RECS;
}

void artpak_hdr_init(struct artpak_hdr *h, unsigned w, unsigned h_)
{
    memset(h, 0, sizeof(*h));
    h->magic = ARTPAK_MAGIC;
    h->version = ARTPAK_VERSION;
    h->w = w;
    h->h = h_;
    h->slot_bytes = artpak_slot_bytes(w, h_);
    h->hdr_crc = crc_of(h, offsetof(struct artpak_hdr, hdr_crc));
}

bool artpak_hdr_ok(const struct artpak_hdr *h, unsigned w, unsigned h_)
{
    return h->magic == ARTPAK_MAGIC && h->version == ARTPAK_VERSION
        && h->w == w && h->h == h_
        && h->slot_bytes == artpak_slot_bytes(w, h_)
        && h->hdr_crc == crc_of(h, offsetof(struct artpak_hdr, hdr_crc));
}

void artpanel_hdr_init(struct artpanel_hdr *h, unsigned w, unsigned h_)
{
    memset(h, 0, sizeof(*h));
    h->magic = ARTPANEL_MAGIC;
    h->version = ARTPANEL_VERSION;
    h->w = w;
    h->h = h_;
    h->slot_bytes = artpak_slot_bytes(w, h_);
    artpanel_hdr_seal(h);
}

void artpanel_hdr_seal(struct artpanel_hdr *h)
{
    h->hdr_crc = 0;
    h->hdr_crc = crc_of(h, sizeof(*h));
}

bool artpanel_hdr_ok(const struct artpanel_hdr *h, unsigned w, unsigned h_)
{
    struct artpanel_hdr c;
    unsigned i;

    /* 2: a file from the #92 development builds (never shipped), the
     * same layout; the next save writes 1 */
    if (h->magic != ARTPANEL_MAGIC
        || (h->version != ARTPANEL_VERSION && h->version != 2)
        || h->w != w || h->h != h_
        || h->slot_bytes != artpak_slot_bytes(w, h_)
        || h->n_slots > ARTPANEL_SLOTS)
        return false;
    memcpy(&c, h, sizeof(c));
    c.hdr_crc = 0;
    if (crc_of(&c, sizeof(c)) != h->hdr_crc)
        return false;
    for (i = 0; i < ARTPANEL_SLOTS; i++)
        if (h->ent[i].state == ARTREC_OK && h->ent[i].slot >= h->n_slots)
            return false;       /* names a slot the file does not have */
    return true;
}

int artpanel_regroup_plan(const struct artpanel_hdr *h,
                          uint8_t new_slot[ARTPANEL_SLOTS])
{
    int k, set, n = 0;

    memset(new_slot, 0xff, ARTPANEL_SLOTS);
    for (set = 0; set < 256 && n < ARTPANEL_SLOTS; set++)
    {
        bool more = false;

        for (k = 0; k < ARTPANEL_SLOTS; k++)
        {
            const struct artpanel_ent *e = &h->ent[k];

            if (e->state != ARTREC_OK)
                continue;
            if (ARTPANEL_ENT_SET(e) == set)
                new_slot[k] = (uint8_t)n++;
            else if (ARTPANEL_ENT_SET(e) > set)
                more = true;
        }
        if (!more)
            break;
    }
    return n;
}

bool artpanel_grouped(const struct artpanel_hdr *h)
{
    uint8_t plan[ARTPANEL_SLOTS];
    int k;

    /* grouped = what the plan would write, up to a shift per set: each
     * set's entries consecutive and ascending */
    artpanel_regroup_plan(h, plan);
    for (k = 0; k < ARTPANEL_SLOTS; k++)
    {
        int j;

        if (plan[k] == 0xff)
            continue;
        /* the previous entry of the same set */
        for (j = k - 1; j >= 0; j--)
            if (plan[j] != 0xff && ARTPANEL_ENT_SET(&h->ent[j])
                                   == ARTPANEL_ENT_SET(&h->ent[k]))
                break;
        if (j >= 0 && h->ent[k].slot != h->ent[j].slot + 1)
            return false;
    }
    return true;
}

bool artpanel_run(const struct artpanel_hdr *h, const int *ent, int n)
{
    int i;

    for (i = 1; i < n; i++)
        if (h->ent[ent[i]].slot != h->ent[ent[i - 1]].slot + 1)
            return false;
    return n > 0;
}

/* --- the panel pool (#100) --------------------------------------------- */
void artpool_init(struct artpool_idx *x, unsigned w, unsigned h_)
{
    memset(x, 0, sizeof(*x));
    x->shown.h.magic = ARTSHOWN_MAGIC;
    x->shown.h.version = ARTPOOL_VERSION;
    x->shown.h.w = w;
    x->shown.h.h = h_;
    x->shown.h.slot_bytes = artpak_slot_bytes(w, h_);
    x->pool.magic = ARTPOOL_MAGIC;
    x->pool.version = ARTPOOL_VERSION;
    artpool_seal(x);
}

void artpool_seal(struct artpool_idx *x)
{
    x->shown.h.hdr_crc = 0;
    x->shown.h.hdr_crc = crc_of(&x->shown.h, sizeof(x->shown.h));
    x->pool.crc = 0;
    x->pool.crc = crc_of(&x->pool, sizeof(x->pool));
}

bool artpool_ok(const struct artpool_hdr *p)
{
    struct artpool_hdr c;
    unsigned i;

    if (p->magic != ARTPOOL_MAGIC || p->version != ARTPOOL_VERSION
        || p->n_slots > ARTPOOL_ENTS + 1 || p->n_pinned > ARTPOOL_PINNED
        || p->n_rand > ARTPOOL_RAND)
        return false;
    memcpy(&c, p, sizeof(c));
    c.crc = 0;
    if (crc_of(&c, sizeof(c)) != p->crc)
        return false;
    for (i = 0; i < ARTPOOL_ENTS; i++)
        if (p->ent[i].state == ARTREC_OK && p->ent[i].slot >= p->n_slots)
            return false;
    return true;
}

bool artshown_ok(const struct artpanel_hdr *h, unsigned w, unsigned h_)
{
    struct artpanel_hdr c;
    unsigned i;

    if (h->magic != ARTSHOWN_MAGIC || h->version != ARTPOOL_VERSION
        || h->w != w || h->h != h_
        || h->slot_bytes != artpak_slot_bytes(w, h_))
        return false;
    memcpy(&c, h, sizeof(c));
    c.hdr_crc = 0;
    if (crc_of(&c, sizeof(c)) != h->hdr_crc)
        return false;
    for (i = 0; i < ARTPANEL_SLOTS; i++)
        if (h->ent[i].state == ARTREC_OK
            && h->ent[i].slot >= ARTPOOL_SLOT0 + ARTPOOL_ENTS + 1)
            return false;
    return true;
}

uint32_t artpool_free_slot(const struct artpool_hdr *p)
{
    uint32_t used[(ARTPOOL_ENTS + 1 + 31) / 32] = { 0 };
    uint32_t s;
    unsigned i;

    for (i = 0; i < ARTPOOL_ENTS; i++)
        if (p->ent[i].state == ARTREC_OK && p->ent[i].slot <= ARTPOOL_ENTS)
            used[p->ent[i].slot / 32] |= 1u << (p->ent[i].slot % 32);
    for (s = 0; s < p->n_slots; s++)
        if (!(used[s / 32] & (1u << (s % 32))))
            return s;
    return p->n_slots;
}

void artnpc_init(struct artpanel_hdr *h, unsigned w, unsigned h_)
{
    artpanel_hdr_init(h, w, h_);
    h->magic = ARTNPC_MAGIC;
    artpanel_hdr_seal(h);
}

bool artnpc_ok(const struct artpanel_hdr *h, unsigned w, unsigned h_)
{
    struct artpanel_hdr c;
    uint32_t used = 0;
    unsigned i;

    if (h->magic != ARTNPC_MAGIC || h->version != ARTPANEL_VERSION
        || h->w != w || h->h != h_
        || h->slot_bytes != artpak_slot_bytes(w, h_)
        || h->n_slots > ARTNPC_SLOTS)
        return false;
    memcpy(&c, h, sizeof(c));
    c.hdr_crc = 0;
    if (crc_of(&c, sizeof(c)) != h->hdr_crc)
        return false;
    for (i = 0; i < ARTPANEL_SLOTS; i++)
    {
        if (h->ent[i].state != ARTREC_OK)
            continue;
        if (i >= ARTNPC_SLOTS || h->ent[i].slot >= h->n_slots
            || (used & (1u << h->ent[i].slot)))
            return false;
        used |= 1u << h->ent[i].slot;
    }
    return true;
}

int artnpc_find(const struct artpanel_hdr *h, uint64_t key)
{
    int i;

    for (i = 0; key && i < ARTNPC_SLOTS; i++)
        if (h->ent[i].state == ARTREC_OK && h->ent[i].key == key)
            return i;
    return -1;
}

uint32_t artnpc_slot(const struct artpanel_hdr *h)
{
    uint32_t used = 0, s;
    int i, last = -1;

    for (i = 0; i < ARTNPC_SLOTS; i++)
        if (h->ent[i].state == ARTREC_OK && h->ent[i].slot < ARTNPC_SLOTS)
        {
            used |= 1u << h->ent[i].slot;
            last = i;
        }
    for (s = 0; s < ARTNPC_SLOTS; s++)
        if (!(used & (1u << s)))
            return s;
    return h->ent[last].slot;       /* full: the oldest's (last >= 0) */
}

void artnpc_put(struct artpanel_hdr *h, const struct artpanel_ent *e)
{
    struct artpanel_ent keep[ARTNPC_SLOTS];
    int i, n = 0;

    for (i = 0; i < ARTNPC_SLOTS; i++)
        if (h->ent[i].state == ARTREC_OK && h->ent[i].key != e->key
            && h->ent[i].slot != e->slot)
            keep[n++] = h->ent[i];
    memset(h->ent, 0, sizeof(h->ent));
    h->ent[0] = *e;
    for (i = 0; i < n && i + 1 < ARTNPC_SLOTS; i++)
        h->ent[i + 1] = keep[i];
    if (h->n_slots <= e->slot)
        h->n_slots = e->slot + 1u;
    artpanel_hdr_seal(h);
}

/* The pick's candidates: 0..ARTPANEL_SLOTS-1 = panels.pak's entry k,
 * ARTPOOL_SLOT0 + k = the pool's entry k */
#define PC_N (ARTPANEL_SLOTS + ARTPOOL_ENTS)
struct pool_pick {
    const struct artpanel_hdr *ph;
    struct artpool_idx *x;
    uint8_t c[PC_N];
    int n;
    uint32_t used[(PC_N + 31) / 32];
    uint32_t rng;
    int out, nrand;
};

static const struct artpanel_ent *pc_ent(const struct pool_pick *p, int c)
{
    return c < ARTPOOL_SLOT0 ? &p->ph->ent[c]
                             : &p->x->pool.ent[c - ARTPOOL_SLOT0];
}

static bool pc_used(const struct pool_pick *p, int i)
{
    return (p->used[i / 32] >> (i % 32)) & 1;
}

static bool key_in(const uint64_t *keys, int n, uint64_t key)
{
    while (n-- > 0)
        if (keys[n] == key)
            return true;
    return false;
}

static uint32_t pick_rand(struct pool_pick *p)
{
    uint32_t v = p->rng;

    v ^= v << 13;
    v ^= v >> 17;
    v ^= v << 5;
    return p->rng = v;
}

/* Candidate i into the shown set */
static void pick_take(struct pool_pick *p, int i, bool rnd)
{
    const struct artpanel_ent *e = pc_ent(p, p->c[i]);
    struct artpanel_ent *o = &p->x->shown.h.ent[p->out++];

    *o = *e;
    if (p->c[i] >= ARTPOOL_SLOT0)
        o->slot = (uint8_t)(e->slot + ARTPOOL_SLOT0);
    p->used[i / 32] |= 1u << (i % 32);
    if (rnd && p->nrand < ARTPOOL_RAND)
        p->x->pool.rand[p->nrand++] = e->key;
}

/* Candidates of 'set' not yet taken: pinned (in the pinned list) or not */
static int pick_count(const struct pool_pick *p, int set, bool pinned)
{
    const struct artpool_hdr *h = &p->x->pool;
    int i, n = 0;

    for (i = 0; i < p->n; i++)
    {
        const struct artpanel_ent *e = pc_ent(p, p->c[i]);

        if (!pc_used(p, i) && ARTPANEL_ENT_SET(e) == set
            && key_in(h->pinned, h->n_pinned, e->key) == pinned)
            n++;
    }
    return n;
}

/* Fills up to 'quota' entries of 'set': pinned first (leaving 'reserve'
 * entries, or as many as there are random candidates, to random picks),
 * then random ones, then the pinned left out */
static void pick_set(struct pool_pick *p, int set, int quota, int reserve,
                     const uint64_t *prev, int nprev, bool rotate)
{
    const struct artpool_hdr *h = &p->x->pool;
    uint8_t ord[PC_N];
    int nr = pick_count(p, set, false), lim, taken = 0, i, j, no;

    lim = quota - (nr < reserve ? nr : reserve);
    for (j = 0; j < h->n_pinned && taken < lim; j++)
        for (i = 0; i < p->n; i++)
        {
            const struct artpanel_ent *e = pc_ent(p, p->c[i]);

            if (!pc_used(p, i) && ARTPANEL_ENT_SET(e) == set
                && e->key == h->pinned[j])
            {
                pick_take(p, i, false);
                taken++;
                break;
            }
        }
    /* random: refresh keeps the previous picks (their order) first;
     * rotate draws the fresh ones first, the previous last */
    no = 0;
    if (!rotate)
        for (j = 0; j < nprev; j++)
            for (i = 0; i < p->n; i++)
            {
                const struct artpanel_ent *e = pc_ent(p, p->c[i]);

                if (!pc_used(p, i) && ARTPANEL_ENT_SET(e) == set
                    && e->key == prev[j]
                    && !key_in(h->pinned, h->n_pinned, e->key))
                {
                    ord[no++] = (uint8_t)i;
                    break;
                }
            }
    {
        int pass;

        for (pass = 0; pass < 2; pass++)
        {
            int s0 = no;

            if (pass == 1 && !rotate)
                break;              /* refresh: the previous are in */
            for (i = 0; i < p->n; i++)
            {
                const struct artpanel_ent *e = pc_ent(p, p->c[i]);
                bool was = key_in(prev, nprev, e->key);

                if (pc_used(p, i) || ARTPANEL_ENT_SET(e) != set
                    || key_in(h->pinned, h->n_pinned, e->key)
                    || (rotate ? was != (pass == 1) : was))
                    continue;
                ord[no++] = (uint8_t)i;
            }
            /* shuffle this pass's run (Fisher-Yates) */
            for (j = no - 1; j > s0; j--)
            {
                int k = s0 + (int)(pick_rand(p) % (uint32_t)(j - s0 + 1));
                uint8_t t = ord[j];

                ord[j] = ord[k];
                ord[k] = t;
            }
        }
    }
    for (j = 0; j < no && taken < quota; j++)
    {
        pick_take(p, ord[j], true);
        taken++;
    }
    /* too few others: the pinned left out by the limit */
    for (j = 0; j < h->n_pinned && taken < quota; j++)
        for (i = 0; i < p->n; i++)
        {
            const struct artpanel_ent *e = pc_ent(p, p->c[i]);

            if (!pc_used(p, i) && ARTPANEL_ENT_SET(e) == set
                && e->key == h->pinned[j])
            {
                pick_take(p, i, false);
                taken++;
                break;
            }
        }
    /* and any other cover of the set (a pak without a pinned list) */
    for (i = 0; i < p->n && taken < quota; i++)
        if (!pc_used(p, i) && ARTPANEL_ENT_SET(pc_ent(p, p->c[i])) == set)
        {
            pick_take(p, i, false);
            taken++;
        }
}

int artpool_pick(struct artpool_idx *x, const struct artpanel_hdr *ph,
                 uint32_t seed, bool rotate)
{
    struct pool_pick p;
    uint64_t prev[ARTPOOL_RAND];
    int nprev = x->pool.n_rand, k, nb, nv, nm;

    memset(&p, 0, sizeof(p));
    p.ph = ph;                      /* NULL: no candidate < SLOT0 */
    p.x = x;
    p.rng = seed ? seed : 0x9e3779b9u;
    if (nprev > ARTPOOL_RAND)
        nprev = ARTPOOL_RAND;
    memcpy(prev, x->pool.rand, sizeof(prev));
    for (k = 0; ph && k < ARTPANEL_SLOTS; k++)
        if (ph->ent[k].state == ARTREC_OK)
            p.c[p.n++] = (uint8_t)k;
    for (k = 0; k < ARTPOOL_ENTS; k++)
    {
        const struct artpanel_ent *e = &x->pool.ent[k];
        int i;

        if (e->state != ARTREC_OK)
            continue;
        for (i = 0; i < p.n && pc_ent(&p, p.c[i])->key != e->key; i++)
            ;
        if (i == p.n)               /* panels.pak's copy wins */
            p.c[p.n++] = (uint8_t)(ARTPOOL_SLOT0 + k);
    }
    nb = pick_count(&p, 1, true) + pick_count(&p, 1, false);
    nb = nb < ARTPOOL_BOOKS ? nb : ARTPOOL_BOOKS;
    nv = pick_count(&p, 2, true) + pick_count(&p, 2, false);
    nv = nv < ARTPOOL_VIDEOS ? nv : ARTPOOL_VIDEOS;
    nm = ARTPANEL_SLOTS - nb - nv;
    memset(x->shown.h.ent, 0, sizeof(x->shown.h.ent));
    memset(x->pool.rand, 0, sizeof(x->pool.rand));
    /* col 0's order: music, videos, books */
    ARTFMT_PACE();
    pick_set(&p, 0, nm, ARTPOOL_MUSIC_RAND, prev, nprev, rotate);
    ARTFMT_PACE();
    pick_set(&p, 2, nv, 0, prev, nprev, rotate);
    ARTFMT_PACE();
    pick_set(&p, 1, nb, ARTPOOL_BOOK_RAND, prev, nprev, rotate);
    x->pool.n_rand = (uint16_t)p.nrand;
    x->shown.h.n_slots = 0;
    ARTFMT_PACE();
    artpool_seal(x);
    return p.out;
}

bool artpanel_read(const char *path, uint32_t slot_bytes,
                   const struct artpanel_ent *e, void *pix, size_t n,
                   const volatile int *seq, int seq0)
{
    struct artpak_tag tag;
    off_t off = artpanel_slot_offset(slot_bytes, e->slot);
    bool ok;
    int fd = open(path, O_RDONLY);

    if (fd < 0)
        return false;
    ok = lseek(fd, off, SEEK_SET) == off
         && read(fd, pix, n) == (ssize_t)n
         && read(fd, &tag, sizeof(tag)) == (ssize_t)sizeof(tag)
         && tag.magic == ARTPAK_TAG_MAGIC && tag.key == e->key;
    close(fd);
    return ok && *seq == seq0;  /* a rewrite began during the read */
}

void artbundle_name(artcache_key_t key, char out[17])
{
    static const char hex[] = "0123456789ABCDEF";
    uint32_t hi = (uint32_t)(key >> 32);
    int i;

    out[0] = hex[(hi >> 28) & 15];
    out[1] = hex[(hi >> 24) & 15];
    out[2] = '/';
    for (i = 0; i < 8; i++)
        out[3 + i] = hex[(hi >> (28 - 4 * i)) & 15];
    memcpy(out + 11, ".ART", 5);
}

/* --- I/O helpers -------------------------------------------------------- */
/* Positive short reads can be normal; only an actual EOF proves truncation.
 * An error after a short read is still an I/O failure, not corruption. */
static enum artidx_open_result read_exact(int fd, void *buf, size_t n)
{
    char *p = buf;
    while (n)
    {
        ssize_t got = read(fd, p, n);
        if (got < 0)
            return ARTIDX_IO_ERROR;
        if (!got)
            return ARTIDX_CORRUPT;
        p += got;
        n -= got;
    }
    return ARTIDX_OPENED;
}

/* A failed publish in ipodui.log: the file, the step (1 remove prev,
 * 2 path -> prev, 3 tmp -> path, 4 the rollback; artfile_replace(): 5 the
 * remove, 6 the rename), errno and, on the
 * device, the file layer's last rename failure (rc/undo, file.c) */
static enum artfile_publish_result pub_fail(const char *path, int step,
                                            enum artfile_publish_result rc)
{
#ifndef ARTFMT_HOST
    int err = errno;
    const char *n = strrchr(path, '/');
#ifdef SIMULATOR
    ui_log("art", "pub %s st=%d e=%d", n ? n + 1 : path, step, err);
#else
    struct rbfs_rename_fault rf;
    rbfs_rename_fault_take(&rf);
    ui_log("art", "pub %s st=%d e=%d fs=%d/%d", n ? n + 1 : path, step, err,
           rf.rc, rf.undo);
#endif
    errno = err;
#else
    (void)path; (void)step;
#endif
    return rc;
}

enum artfile_publish_result artfile_publish(const char *path, const char *tmp,
                                           const char *prev)
{
    bool moved;
    int exists;

    ARTFMT_PACE();
    exists = artfile_exists(path);
    if (exists < 0)
    {
#ifndef ARTFMT_HOST
        static bool logged;         /* once per boot: a pass retries it */
        if (!logged)
        {
            logged = true;
            pub_fail(path, 0, ARTFILE_RETAINED);
        }
#endif
        return ARTFILE_RETAINED;    /* a deferral: tmp kept, nothing moved */
    }
    /* Never rename over an existing name: on the device the firmware's
     * overwrite rename left two "index.prev" entries in normal use
     * (docs/handoff-2026-10-19-crash-fat.md section 6). Drop the old prev
     * first, and only while path exists. A cut after the remove leaves
     * path and tmp; after the first rename, prev and tmp (pass_begin()
     * recovers either): path or prev names a complete copy at every step.
     * A failed first rename now costs the old prev, never path. */
    if (exists && !art_remove(prev, path))  /* only its name when it is a */
        return pub_fail(path, 1, ARTFILE_RETAINED); /* second name on path's
                                    chain; prev only goes while path stands */
    ARTFMT_PACE();                  /* (path stands; none between the two
                                       renames: readers find path again at
                                       once, as before) */
    moved = rename(path, prev) == 0;
    if (!moved && errno != ENOENT)
        return pub_fail(path, 2, ARTFILE_RETAINED);
    if (rename(tmp, path) == 0)
        return ARTFILE_PUBLISHED;
    if (moved && rename(prev, path) != 0)
        return pub_fail(path, 4, ARTFILE_RECOVER_PREV);
    return pub_fail(path, 3, ARTFILE_RETAINED);
}

int artfile_replace(const char *tmp, const char *path)
{
    if (!art_remove(path, tmp))             /* path may name tmp's chain */
        return pub_fail(path, 5, -1);
    if (rename(tmp, path) != 0)
        return pub_fail(path, 6, -1);
    return 0;
}

bool artfile_tmp_clear(const char *tmp, const char *path)
{
    return art_remove(tmp, path);
}

void artfile_recover(const char *path, const char *tmp, const char *prev)
{
    art_drop_alias(tmp, path);
    art_drop_alias(prev, path);
}

#ifdef ARTFMT_HOST
#define ARTFILE_PATH_BYTES 1030
#else
#define ARTFILE_PATH_BYTES (MAX_PATH + 6)
#endif
static bool suffix_path(char *out, const char *path, const char *suffix)
{
    return snprintf(out, ARTFILE_PATH_BYTES, "%s%s", path, suffix)
           < ARTFILE_PATH_BYTES;
}

static bool read_at(int fd, long off, void *buf, size_t n)
{
    if (lseek(fd, off, SEEK_SET) != off)
        return false;
    return read(fd, buf, n) == (ssize_t)n;
}

static bool write_all(int fd, const void *buf, size_t n)
{
    return write(fd, buf, n) == (ssize_t)n;
}

static bool bundle_hdr_ok(const struct artbundle_hdr *hd, artcache_key_t key);
/* Full validation used by the writer, never in a draw callback. With pixels
 * supplied, compare the read-back against exactly what was decoded. */
static enum artidx_open_result bundle_check(const char *path,
                                            artcache_key_t key,
                                            const void *pixels, void *scratch,
                                            size_t scratch_bytes)
{
    struct artbundle_hdr hd;
    enum artidx_open_result rc;
    int fd = open(path, O_RDONLY);
    unsigned i;
    if (fd < 0)
        return errno == ENOENT ? ARTIDX_MISSING : ARTIDX_IO_ERROR;
    rc = read_exact(fd, scratch, ARTBUNDLE_HDR_BYTES);
    if (rc != ARTIDX_OPENED)
        goto out;
    memcpy(&hd, scratch, sizeof(hd));
    if (!bundle_hdr_ok(&hd, key) || !hd.n_img)
    {
        rc = ARTIDX_CORRUPT;
        goto out;
    }
    for (i = 0; i < hd.n_img; i++)
    {
        size_t left = (size_t)hd.img[i].w * hd.img[i].h * 2, pos = 0;
        if (!left || hd.img[i].offset < ARTBUNDLE_HDR_BYTES)
        {
            rc = ARTIDX_CORRUPT;
            goto out;
        }
        if (lseek(fd, hd.img[i].offset, SEEK_SET) != (off_t)hd.img[i].offset)
        {
            rc = ARTIDX_IO_ERROR;
            goto out;
        }
        while (left)
        {
            size_t n = left < scratch_bytes ? left : scratch_bytes;

            ARTFMT_PACE();          /* (the one caller's pixels: pinned) */
            rc = read_exact(fd, scratch, n);
            if (rc != ARTIDX_OPENED)
                goto out;
            if (pixels && memcmp((const char *)pixels + pos, scratch, n))
            {
                rc = ARTIDX_CORRUPT;
                goto out;
            }
            left -= n;
            pos += n;
        }
    }
out:
    if (close(fd) != 0)
        rc = ARTIDX_IO_ERROR;
    return rc;
}

/* --- bundles ----------------------------------------------------------- */
bool artbundle_write_img_buffered(const char *path, artcache_key_t key,
                                  uint32_t version,
                                  const struct artbundle_img *im,
                                  const void *pix, void *hdrbuf,
                                  size_t scratch_bytes)
{
    struct artbundle_hdr hd;    /* built here: hdrbuf is only 4-aligned
                                   (#121), the struct needs 8 */
    size_t n = (size_t)im->w * im->h * 2;
    int fd;
    bool ok;
    char tmp[ARTFILE_PATH_BYTES], prev[ARTFILE_PATH_BYTES];
    enum artidx_open_result rc;

    if (scratch_bytes < ARTBUNDLE_HDR_BYTES || !im->w || !im->h
        || !suffix_path(tmp, path, ".tmp")
        || !suffix_path(prev, path, ".prev"))
        return false;
    /* a zero header first, the pixels, then the real header: a cut
     * write never leaves a valid header over missing pixels */
    memset(hdrbuf, 0, ARTBUNDLE_HDR_BYTES);
    if (!artfile_tmp_clear(tmp, path))
        return false;
    ARTFMT_PACE();
    fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0)
        return false;
    ok = write_all(fd, hdrbuf, ARTBUNDLE_HDR_BYTES);
    ARTFMT_PACE();
    ok = ok && write_all(fd, pix, n);
    ARTFMT_PACE();
    memset(&hd, 0, sizeof(hd));
    hd.magic = ARTBUNDLE_MAGIC;
    hd.version = version;
    hd.key = key;
    hd.n_img = 1;
    hd.img[0] = *im;
    hd.img[0].offset = ARTBUNDLE_HDR_BYTES;
    memcpy(hdrbuf, &hd, sizeof(hd));
    ok = ok && lseek(fd, 0, SEEK_SET) == 0
         && write_all(fd, hdrbuf, ARTBUNDLE_HDR_BYTES);
    ok = ok && fsync(fd) == 0;
    if (close(fd) != 0)
        ok = false;
    ARTFMT_PACE();
    if (!ok || bundle_check(tmp, key, pix, hdrbuf, scratch_bytes) != ARTIDX_OPENED)
        return false;
    ARTFMT_PACE();
    rc = bundle_check(path, key, NULL, hdrbuf, scratch_bytes);
    ARTFMT_PACE();
    if (rc == ARTIDX_IO_ERROR)
        return false;
    if (rc == ARTIDX_CORRUPT)
        /* do not replace a good prev with damage; no rename over the
         * damaged copy either (section 6 of the 2026-10-19 handoff) */
        return artfile_replace(tmp, path) == 0;
    return artfile_publish(path, tmp, prev) == ARTFILE_PUBLISHED;
}

bool artbundle_write_img(const char *path, artcache_key_t key,
                         uint32_t version, const struct artbundle_img *im,
                         const void *pix, void *hdrbuf)
{
    return artbundle_write_img_buffered(path, key, version, im, pix, hdrbuf,
                                       ARTBUNDLE_HDR_BYTES);
}

bool artbundle_write(const char *path, artcache_key_t key,
                     enum artcache_size size, unsigned w, unsigned h,
                     const void *pix, void *hdrbuf)
{
    struct artbundle_img im;

    if (!w || !h || w > 0xffff || h > 0xffff)
        return false;
    memset(&im, 0, sizeof(im));
    im.size_id = (uint8_t)size;
    im.w = (uint16_t)w;
    im.h = (uint16_t)h;
    return artbundle_write_img(path, key, ARTBUNDLE_VERSION_FITTED, &im, pix,
                               hdrbuf);
}

static bool bundle_hdr_ok(const struct artbundle_hdr *hd, artcache_key_t key)
{
    return hd->magic == ARTBUNDLE_MAGIC
           && (hd->version == ARTBUNDLE_VERSION_FITTED
               || hd->version == ARTBUNDLE_VERSION_COMPOSED)
           && hd->key == key && hd->n_img <= ARTBUNDLE_MAX_IMG;
}

static int bundle_read(const char *path, artcache_key_t key,
                   enum artcache_size size, unsigned box_w, unsigned box_h,
                   void *buf, size_t bufsize, uint16_t *w, uint16_t *h)
{
    struct artbundle_hdr hd;
    int fd, i, res = 0;

    fd = open(path, O_RDONLY);
    if (fd < 0)
        return errno == ENOENT ? 0 : -1;
    if (read(fd, &hd, sizeof(hd)) == (ssize_t)sizeof(hd)
        && bundle_hdr_ok(&hd, key))
    {
        for (i = 0; i < hd.n_img; i++)
        {
            const struct artbundle_img *im = &hd.img[i];
            size_t n = (size_t)im->w * im->h * 2;

            if (im->size_id != size || im->format != 0)
                continue;
            res = -1;
            if (im->w && im->h && im->w <= box_w && im->h <= box_h
                && n <= bufsize && read_at(fd, im->offset, buf, n))
            {
                *w = im->w;
                *h = im->h;
                res = 1;
            }
            break;
        }
    }
    close(fd);
    return res;
}

static int bundle_read_one(const char *path, artcache_key_t key,
                       enum artcache_size size, void *buf, size_t bufsize,
                       struct artbundle_img *im)
{
    struct artbundle_hdr hd;    /* a copy: buf is only 4-aligned (#121) */
    ssize_t got;
    int fd, i, res = 0;

    if (bufsize < ARTBUNDLE_HDR_BYTES)
        return -1;
    fd = open(path, O_RDONLY);
    if (fd < 0)
        return errno == ENOENT ? 0 : -1;
    got = read(fd, buf, bufsize);
    close(fd);
    if (got < (ssize_t)sizeof(hd))
        return 0;
    memcpy(&hd, buf, sizeof(hd));
    if (!bundle_hdr_ok(&hd, key))
        return 0;
    for (i = 0; i < hd.n_img; i++)
    {
        size_t n = (size_t)hd.img[i].w * hd.img[i].h * 2;

        if (hd.img[i].size_id != size)
            continue;
        res = -1;
        if (hd.img[i].w && hd.img[i].h
            && hd.img[i].offset == ARTBUNDLE_HDR_BYTES
            && (hd.img[i].format == 0
                || hd.img[i].format == ARTBUNDLE_FMT_NPCOMPOSED)
            && (size_t)got >= ARTBUNDLE_HDR_BYTES + n)
        {
            *im = hd.img[i];
            res = 1;
        }
        break;
    }
    return res;
}

int artbundle_read(const char *path, artcache_key_t key,
                   enum artcache_size size, unsigned box_w, unsigned box_h,
                   void *buf, size_t bufsize, uint16_t *w, uint16_t *h)
{
    char prev[ARTFILE_PATH_BYTES];
    int rc = bundle_read(path, key, size, box_w, box_h, buf, bufsize, w, h);
    if (rc != 1 && suffix_path(prev, path, ".prev"))
    {
        int old = bundle_read(prev, key, size, box_w, box_h, buf, bufsize, w, h);
        if (old == 1)
            return old;
    }
    return rc;
}

int artbundle_read_one(const char *path, artcache_key_t key,
                       enum artcache_size size, void *buf, size_t bufsize,
                       struct artbundle_img *im)
{
    char prev[ARTFILE_PATH_BYTES];
    int rc = bundle_read_one(path, key, size, buf, bufsize, im);
    if (rc != 1 && suffix_path(prev, path, ".prev"))
    {
        int old = bundle_read_one(prev, key, size, buf, bufsize, im);
        if (old == 1)
            return old;
    }
    return rc;
}

/* --- index reader ------------------------------------------------------- */
void artidx_init(struct artidx_reader *r)
{
    r->fd = -1;
    r->page = -1;
    r->n_pages = 0;
}

void artidx_close(struct artidx_reader *r)
{
    if (r->fd >= 0)
        close(r->fd);
    artidx_init(r);
}

static long page_off(int p)
{
    return ARTIDX_HDR_BYTES + (long)p * ARTIDX_PAGE_BYTES;
}

static int page_recs(const struct artidx_reader *r, int p)
{
    uint32_t left = r->hdr.count - (uint32_t)p * ARTIDX_PAGE_RECS;

    return left < (uint32_t)ARTIDX_PAGE_RECS ? (int)left : ARTIDX_PAGE_RECS;
}

static bool load_page(struct artidx_reader *r, int p)
{
    if (r->page == p)
        return true;
    r->page = -1;
    if (!read_at(r->fd, page_off(p), r->recs,
                 page_recs(r, p) * sizeof(struct artcache_rec)))
        return false;
    r->page = p;
    return true;
}

enum artidx_open_result artidx_open(struct artidx_reader *r, const char *path)
{
    return artidx_open_ex(r, path, NULL, NULL);
}

enum artidx_open_result artidx_open_ex(struct artidx_reader *r,
                                       const char *path,
                                       artidx_visit_fn visit, void *ctx)
{
    uint32_t crc = ARTFMT_CRC_INIT;
    uint64_t last = 0;
    int p, i;
    enum artidx_open_result rc;

    artidx_close(r);
    r->fd = open(path, O_RDONLY);
    if (r->fd < 0)
    {
        r->fd = -1;
        return errno == ENOENT ? ARTIDX_MISSING : ARTIDX_IO_ERROR;
    }
    rc = read_exact(r->fd, &r->hdr, sizeof(r->hdr));
    if (rc != ARTIDX_OPENED)
        goto fail;
    if (!artidx_hdr_ok(&r->hdr))
        goto corrupt;
    r->n_pages = (r->hdr.count + ARTIDX_PAGE_RECS - 1) / ARTIDX_PAGE_RECS;
    /* one pass: CRC, order, fences */
    for (p = 0; p < r->n_pages; p++)
    {
        int n = page_recs(r, p);

        ARTFMT_PACE();
        r->page = -1;
        if (lseek(r->fd, page_off(p), SEEK_SET) != page_off(p))
        {
            rc = ARTIDX_IO_ERROR;
            goto fail;
        }
        rc = read_exact(r->fd, r->recs, n * sizeof(struct artcache_rec));
        if (rc != ARTIDX_OPENED)
            goto fail;
        crc = artfmt_crc(crc, r->recs, n * sizeof(struct artcache_rec));
        for (i = 0; i < n; i++)
        {
            if (r->recs[i].key == 0 || (p + i > 0 && r->recs[i].key <= last))
                goto corrupt;
            last = r->recs[i].key;
            if (visit)
                visit(ctx, &r->recs[i]);
        }
        r->fence[p] = r->recs[0].key;
        r->page = p;
    }
    if (artfmt_crc_end(crc) != r->hdr.rec_crc)
        goto corrupt;
    return ARTIDX_OPENED;
corrupt:
    rc = ARTIDX_CORRUPT;
fail:
    artidx_close(r);
    return rc;
}

bool artidx_get(struct artidx_reader *r, uint32_t i, struct artcache_rec *out)
{
    if (r->fd < 0 || i >= r->hdr.count)
        return false;
    if (!load_page(r, i / ARTIDX_PAGE_RECS))
        return false;
    *out = r->recs[i % ARTIDX_PAGE_RECS];
    return true;
}

bool artidx_lookup(struct artidx_reader *r, artcache_key_t key,
                   struct artcache_rec *out, bool *io_err)
{
    int lo = 0, hi, p;

    if (io_err)
        *io_err = false;
    if (r->fd < 0 || r->n_pages == 0 || key < r->fence[0])
        return false;
    /* last page whose first key <= key */
    hi = r->n_pages - 1;
    while (lo < hi)
    {
        int mid = (lo + hi + 1) / 2;

        if (r->fence[mid] <= key)
            lo = mid;
        else
            hi = mid - 1;
    }
    p = lo;
    if (!load_page(r, p))
    {
        if (io_err)
            *io_err = true;
        return false;
    }
    lo = 0;
    hi = page_recs(r, p) - 1;
    while (lo <= hi)
    {
        int mid = (lo + hi) / 2;
        uint64_t k = r->recs[mid].key;

        if (k == key)
        {
            *out = r->recs[mid];
            return true;
        }
        if (k < key)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return false;
}

/* --- index writer ------------------------------------------------------- */
struct wbuf {
    int fd;
    struct artcache_rec *page;
    int n;
    uint32_t count, crc;
    bool err;
};

static void wb_put(struct wbuf *w, const struct artcache_rec *rec)
{
    if (w->err)
        return;
    if (w->count >= ARTIDX_MAX_RECS)
    {
        w->err = true;
        return;
    }
    w->page[w->n++] = *rec;
    w->count++;
    if (w->n == ARTIDX_PAGE_RECS)
    {
        ARTFMT_PACE();
        w->crc = artfmt_crc(w->crc, w->page, ARTIDX_PAGE_BYTES);
        w->err = !write_all(w->fd, w->page, ARTIDX_PAGE_BYTES);
        w->n = 0;
    }
}

int artidx_write_merged(const char *path, struct artidx_reader *old,
                        const struct artcache_rec *add, int n_add,
                        artidx_keep_fn keep, void *ctx,
                        const struct artidx_hdr *hdr_tmpl, void *page)
{
    struct wbuf w = { -1, page, 0, 0, ARTFMT_CRC_INIT, false };
    struct artidx_hdr h = *hdr_tmpl;
    uint32_t i = 0, n_old = old && old->fd >= 0 ? old->hdr.count : 0;
    int a = 0;
    struct artcache_rec o;
    bool have_o = false;

    w.fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (w.fd < 0)
        return -1;
    /* header placeholder: a file cut short has a bad header CRC */
    memset(&h.hdr_crc, 0xff, sizeof(h.hdr_crc));
    memset(page, 0, ARTIDX_HDR_BYTES);
    memcpy(page, &h, sizeof(h));
    if (!write_all(w.fd, page, ARTIDX_HDR_BYTES))
        w.err = true;
    while (!w.err)
    {
        if (!have_o && i < n_old)
        {
            if (!(i % ARTIDX_PAGE_RECS))
                ARTFMT_PACE();
            if (!artidx_get(old, i++, &o))
            {
                w.err = true;
                break;
            }
            have_o = true;
        }
        if (!have_o && a >= n_add)
            break;
        if (a < n_add && (!have_o || add[a].key <= o.key))
        {
            if (have_o && add[a].key == o.key)
                have_o = false;             /* replaced */
            if (add[a].state != 0)
                wb_put(&w, &add[a]);
            a++;
        }
        else
        {
            if (!keep || keep(ctx, &o, i - 1))
                wb_put(&w, &o);
            have_o = false;
        }
    }
    if (!w.err && w.n)
    {
        w.crc = artfmt_crc(w.crc, w.page, w.n * sizeof(struct artcache_rec));
        w.err = !write_all(w.fd, w.page, w.n * sizeof(struct artcache_rec));
    }
    if (!w.err)
    {
        h.count = w.count;
        h.rec_crc = artfmt_crc_end(w.crc);
        artidx_hdr_seal(&h);
        w.err = lseek(w.fd, 0, SEEK_SET) != 0 || !write_all(w.fd, &h, sizeof(h));
    }
    if (!w.err && fsync(w.fd) != 0)
        w.err = true;
    if (close(w.fd) < 0)
        w.err = true;
    return w.err ? -1 : (int)w.count;
}

/* --- GC ----------------------------------------------------------------- */
static int cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;

    return x < y ? -1 : x > y;
}

bool artidx_gc_mark(struct artidx_reader *idx, const char *tpath,
                    const struct artalb_hdr *thdr, struct artalb_rec *tbuf,
                    int tbuf_n, artidx_keys_fn keys, void *ctx, uint32_t cap,
                    uint32_t *live)
{
    uint32_t n_idx = idx->fd >= 0 ? idx->hdr.count : 0, pos = 0;
    bool end = false;

    memset(live, 0, (n_idx + 31) / 32 * sizeof(uint32_t));
    if (n_idx == 0)
        return true;
    if (cap == 0 || tbuf_n <= 0)
        return false;
    while (!end)
    {
        uint32_t m = 0, j = 0, i;
        const uint64_t *kb;

        /* 1: the next 'cap' keys of the table */
        while (m < cap)
        {
            int want = (int)(cap - m < (uint32_t)tbuf_n ? cap - m
                                                        : (uint32_t)tbuf_n);
            int got, k;
            uint64_t *kw;

            ARTFMT_PACE();
            got = artalb_read(tpath, thdr, pos, want, tbuf);

            if (got < 0)
                return false;
            if (got == 0)
            {
                end = true;
                break;
            }
            kw = keys(ctx);         /* after the read */
            for (k = 0; k < got; k++)
                if (tbuf[k].key)
                    kw[m++] = tbuf[k].key;
            pos += got;
        }
        if (m == 0)
            break;
        /* 2: sorted (no I/O, so the buffer stays put) */
        qsort(keys(ctx), m, sizeof(uint64_t), cmp_u64);
        /* 3: merged with one walk of the index (sorted by key) */
        for (i = 0; i < n_idx && j < m; i++)
        {
            struct artcache_rec r;

            if (!(i % ARTIDX_PAGE_RECS))
                ARTFMT_PACE();      /* before the page read: kb after it */
            if (!artidx_get(idx, i, &r))
                return false;
            kb = keys(ctx);         /* after a page read */
            while (j < m && kb[j] < r.key)
                j++;
            if (j < m && kb[j] == r.key)
                live[i / 32] |= 1u << (i % 32);
        }
    }
    return true;
}

/* --- chapter art -------------------------------------------------------- */
void artrec_chapter_init(struct artcache_rec *r, artcache_key_t key,
                         artcache_key_t bundle, artcache_key_t book,
                         uint32_t mtime, uint8_t src_type)
{
    memset(r, 0, sizeof(*r));
    r->key = key;
    r->state = ARTREC_OK;
    r->flags = ARTREC_F_CHAPTER;
    r->src_kind = ARTSRC_EMBEDDED;
    r->src_type = src_type;
    r->slot = (uint32_t)bundle;
    r->src_hash = (uint32_t)(bundle >> 32);
    r->src_size = (uint32_t)book;
    r->src_crc = (uint32_t)(book >> 32);
    r->mtime = mtime;
}

int artchap_owner(artchap_img_fn img, void *ctx, int i, uint32_t pos)
{
    int k, owner = i;

    for (k = i - 1; k >= 0; k--)
    {
        uint32_t p = 0;
        uint8_t type = 0;

        if (img(ctx, k, &p, &type) != 0 && p == pos)
            owner = k;
    }
    return owner;
}

int artchap_plan(artcache_key_t book, uint32_t mtime, int count,
                 artchap_img_fn img, void *ctx, struct artcache_rec *out,
                 int room)
{
    artcache_key_t bundle = 0;
    uint32_t group_pos = 0;
    bool in_group = false;
    int i, n = 0;

    for (i = 0; i < count; i++)
    {
        uint32_t pos = 0;
        uint8_t type = 0;
        int st = img(ctx, i, &pos, &type);

        if (st == 0)
            continue;               /* no image: the group goes on (the
                                       parser maps consecutive chapters
                                       to one sample by position) */
        if (!in_group || pos != group_pos)
        {
            in_group = true;
            group_pos = pos;
            /* the group's owner: the first chapter with an image at this
             * position, adjacent or not (the builder's size + CRC dedupe
             * points copies of one image at the first copy) */
            bundle = artcache_key_chapter(book,
                                          artchap_owner(img, ctx, i, pos));
        }
        if (st < 0 || n >= room)
            continue;
        artrec_chapter_init(&out[n++], artcache_key_chapter(book, i), bundle,
                            book, mtime, type);
    }
    return n;
}

static int cmp_rec(const void *a, const void *b)
{
    const struct artcache_rec *x = a, *y = b;

    if (x->key != y->key)
        return x->key < y->key ? -1 : 1;
    return (int)y->state - (int)x->state;   /* state 0 (delete) last */
}

int artrec_sort_unique(struct artcache_rec *r, int n)
{
    int i, m = 0;

    qsort(r, n, sizeof(*r), cmp_rec);
    for (i = 0; i < n; i++)
    {
        if (m > 0 && r[m - 1].key == r[i].key)
            m--;                    /* the later one wins */
        r[m++] = r[i];
    }
    return m;
}

static bool bsearch_u64(const uint64_t *a, uint32_t n, uint64_t k)
{
    uint32_t lo = 0, hi = n;

    while (lo < hi)
    {
        uint32_t mid = lo + (hi - lo) / 2;

        if (a[mid] < k)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo < n && a[lo] == k;
}

bool artidx_gc_mark_chapters(struct artidx_reader *idx, artidx_keys_fn keys,
                             void *ctx, uint32_t cap, uint32_t *live)
{
    uint32_t n_idx = idx->fd >= 0 ? idx->hdr.count : 0, m = 0, i;
    bool all = false;
    struct artcache_rec r;

    /* 1: the tags of the live books whose chapter pass is done */
    for (i = 0; i < n_idx; i++)
    {
        if (!((live[i / 32] >> (i % 32)) & 1))
            continue;               /* (no read: the bit is in RAM) */
        if (!(i % ARTIDX_PAGE_RECS))
            ARTFMT_PACE();
        if (!artidx_get(idx, i, &r))
            return false;
        if (artrec_is_chapter(&r) || !(r.flags & ARTREC_F_CHAPTERS))
            continue;
        if (m >= cap)
        {
            all = true;             /* too many: keep every chapter */
            break;
        }
        keys(ctx)[m++] = artchap_book_tag(r.key, r.mtime);
    }
    if (m == 0 && !all)
        return true;                /* no book has chapter records */
    if (!all)
        qsort(keys(ctx), m, sizeof(uint64_t), cmp_u64);
    /* 2: the chapter records of those books */
    for (i = 0; i < n_idx; i++)
    {
        if (!(i % ARTIDX_PAGE_RECS))
            ARTFMT_PACE();
        if (!artidx_get(idx, i, &r))
            return false;
        if (!artrec_is_chapter(&r))
            continue;
        if (all || bsearch_u64(keys(ctx), m,
                               artchap_book_tag(artrec_book_key(&r),
                                                r.mtime)))
            live[i / 32] |= 1u << (i % 32);
    }
    return true;
}

/* --- album table -------------------------------------------------------- */
#define ARTFMT_MIN(a, b) ((a) < (b) ? (a) : (b))

static bool libhdr_ok(struct libfile_hdr *h)
{
    struct libfile_sum_state st;
    uint32_t sum = h->sum;

    if (h->magic != LIBFILE_MAGIC || h->version != LIBFILE_VERSION
        || h->nsec > LIBFILE_MAX_SEC)
        return false;
    h->sum = 0;
    libfile_sum_init(&st);
    libfile_sum_add(&st, h, LIBFILE_HDR_SIZE);
    h->sum = sum;
    return libfile_sum_end(&st) == sum;
}

enum artidx_open_result artalb_check(const char *path,
                                     const struct artalb_stamp *want,
                                     struct artalb_hdr *hdr, void *page)
{
    const uint32_t per = ARTIDX_PAGE_BYTES / sizeof(struct libfile_album);
    struct libfile_album *recs = page;
    struct libfile_sum_state st;
    const struct libfile_sec *sec = NULL;
    uint32_t done = 0, i;
    int fd = open(path, O_RDONLY);
    enum artidx_open_result rc;

    if (fd < 0)
        return errno == ENOENT ? ARTIDX_MISSING : ARTIDX_IO_ERROR;
    rc = read_exact(fd, &hdr->lib, LIBFILE_HDR_SIZE);
    if (rc != ARTIDX_OPENED)
        goto fail;
    if (!libhdr_ok(&hdr->lib))
        goto corrupt;
    if (hdr->lib.commitid != want->commitid
        || hdr->lib.entries != want->entries
        || hdr->lib.sortkey_version != want->sortkey_version)
    {
        close(fd);
        return ARTIDX_MISSING;      /* another database's: not current */
    }
    for (i = 0; i < hdr->lib.nsec; i++)
        if (hdr->lib.sec[i].cat == LIBFILE_CAT_ALBUMTAB
            && hdr->lib.sec[i].col == 0)
            sec = &hdr->lib.sec[i];
    if (!sec)
    {
        close(fd);
        return ARTIDX_MISSING;      /* written without one (no room) */
    }
    if (sec->elem != sizeof(struct libfile_album) || sec->offset % 4
        || sec->offset < LIBFILE_HDR_SIZE)
        goto corrupt;
    if (lseek(fd, sec->offset, SEEK_SET) != (off_t)sec->offset)
    {
        rc = ARTIDX_IO_ERROR;
        goto fail;
    }
    hdr->offset = sec->offset;
    hdr->n_albums = sec->count;
    libfile_sum_init(&st);
    while (done < sec->count)
    {
        uint32_t n = ARTFMT_MIN(sec->count - done, per), j;

        ARTFMT_PACE();
        rc = read_exact(fd, recs, n * sizeof(*recs));
        if (rc != ARTIDX_OPENED)
            goto fail;
        libfile_sum_add(&st, recs, n * sizeof(*recs));
        for (j = 0; j < n; j++)
            if (recs[j].rep < 0 || recs[j].rep >= want->entries)
                goto corrupt;
        done += n;
    }
    if (libfile_sum_end(&st) != sec->sum)
        goto corrupt;
    close(fd);
    return ARTIDX_OPENED;
corrupt:
    rc = ARTIDX_CORRUPT;
fail:
    close(fd);
    return rc;
}

int artalb_read(const char *path, const struct artalb_hdr *expect,
                uint32_t first, int n, struct artalb_rec *out)
{
    /* the fixed part of the header: its sum covers every section's sum */
    const size_t fixed = offsetof(struct libfile_hdr, sec);
    const long off = expect->offset
                   + (long)first * sizeof(struct libfile_album);
    ARTFMT_LIBHDR_FIXED(h);
    struct libfile_album in[8];
    int fd, got = 0;

    if (first >= expect->n_albums)
        return 0;
    if ((uint32_t)n > expect->n_albums - first)
        n = expect->n_albums - first;
    ARTFMT_PACE();
    fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    if (read(fd, &h, fixed) != (ssize_t)fixed
        || memcmp(&h, &expect->lib, fixed)
        || lseek(fd, off, SEEK_SET) != off)
        got = -1;
    while (got >= 0 && got < n)
    {
        int k = ARTFMT_MIN(n - got, (int)(sizeof(in) / sizeof(in[0]))), j;

        ARTFMT_PACE();
        if (read(fd, in, k * sizeof(in[0])) != (ssize_t)(k * sizeof(in[0])))
        {
            got = -1;
            break;
        }
        for (j = 0; j < k; j++, got++)
        {
            out[got].key = (uint64_t)in[j].key_hi << 32 | in[j].key_lo;
            out[got].rep_idx = in[j].rep;
            out[got].max_mtime = in[j].mtime;
            out[got].flags = in[j].flags;
            out[got].count = in[j].count;
        }
    }
    close(fd);
    return got;
}

int artalb_panels(const char *path, const struct artalb_hdr *expect,
                  struct artalb_rec *out, int max)
{
    const size_t fixed = offsetof(struct libfile_hdr, sec);
    const struct libfile_sec *sec = NULL;
    struct libfile_sum_state st;
    ARTFMT_LIBHDR_FIXED(h);
    struct libfile_album in[8];
    uint32_t i;
    int fd, got = 0;

    for (i = 0; i < expect->lib.nsec && i < LIBFILE_MAX_SEC; i++)
        if (expect->lib.sec[i].cat == LIBFILE_CAT_PANELSET
            && expect->lib.sec[i].col == 0)
            sec = &expect->lib.sec[i];
    if (!sec || !sec->count)
        return 0;               /* a file from before H1: no panel set */
    if (sec->elem != sizeof(struct libfile_album) || sec->offset % 4
        || sec->offset < LIBFILE_HDR_SIZE || sec->count > (uint32_t)max)
        return -1;
    ARTFMT_PACE();
    fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    if (read(fd, &h, fixed) != (ssize_t)fixed
        || memcmp(&h, &expect->lib, fixed)
        || lseek(fd, sec->offset, SEEK_SET) != (off_t)sec->offset)
        got = -1;
    libfile_sum_init(&st);
    while (got >= 0 && got < (int)sec->count)
    {
        int k = ARTFMT_MIN((int)sec->count - got,
                           (int)(sizeof(in) / sizeof(in[0]))), j;

        ARTFMT_PACE();
        if (read(fd, in, k * sizeof(in[0])) != (ssize_t)(k * sizeof(in[0])))
        {
            got = -1;
            break;
        }
        libfile_sum_add(&st, in, k * sizeof(in[0]));
        for (j = 0; j < k; j++, got++)
        {
            if (in[j].rep < 0 || in[j].rep >= expect->lib.entries)
            {
                got = -1;
                break;
            }
            out[got].key = (uint64_t)in[j].key_hi << 32 | in[j].key_lo;
            out[got].rep_idx = in[j].rep;
            out[got].max_mtime = in[j].mtime;
            out[got].flags = in[j].flags;
            out[got].count = in[j].count;
        }
    }
    close(fd);
    if (got >= 0 && libfile_sum_end(&st) != sec->sum)
        got = -1;
    return got;
}

int artalb_pool(const char *path, const struct artalb_hdr *expect,
                int first, int max, struct artalb_rec *out, uint8_t *pinned,
                int *total)
{
    const size_t fixed = offsetof(struct libfile_hdr, sec);
    const struct libfile_sec *sec = NULL;
    struct libfile_sum_state st;
    ARTFMT_LIBHDR_FIXED(h);
    struct libfile_pool in[4];
    uint32_t i, w[3], n = 0;
    int fd, got = 0, at = 0;

    *total = 0;
    for (i = 0; i < expect->lib.nsec && i < LIBFILE_MAX_SEC; i++)
        if (expect->lib.sec[i].cat == LIBFILE_CAT_PANELSET
            && expect->lib.sec[i].col == 1)
            sec = &expect->lib.sec[i];
    if (!sec || sec->count < 3)
        return 0;               /* a file from before the pool */
    if (sec->elem != sizeof(uint32_t) || sec->offset % 4
        || sec->offset < LIBFILE_HDR_SIZE)
        return -1;
    ARTFMT_PACE();
    fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    libfile_sum_init(&st);
    if (read(fd, &h, fixed) != (ssize_t)fixed
        || memcmp(&h, &expect->lib, fixed)
        || lseek(fd, sec->offset, SEEK_SET) != (off_t)sec->offset
        || read(fd, w, sizeof(w)) != (ssize_t)sizeof(w))
        got = -1;
    else
    {
        libfile_sum_add(&st, w, sizeof(w));
        n = w[2];
        if (w[1] != LIBFILE_POOL_TAG || n > LIBFILE_POOL_MAX
            || sec->count != 3 + n * (sizeof(in[0]) / 4))
            got = -1;
    }
    while (got >= 0 && at < (int)n)
    {
        int k = ARTFMT_MIN((int)n - at, (int)(sizeof(in) / sizeof(in[0]))), j;

        ARTFMT_PACE();
        if (read(fd, in, k * sizeof(in[0])) != (ssize_t)(k * sizeof(in[0])))
        {
            got = -1;
            break;
        }
        libfile_sum_add(&st, in, k * sizeof(in[0]));
        for (j = 0; j < k; j++, at++)
        {
            const struct libfile_album *a = &in[j].a;

            if (a->rep < 0 || a->rep >= expect->lib.entries)
            {
                got = -1;
                break;
            }
            if (at < first || got >= max)
                continue;
            out[got].key = (uint64_t)a->key_hi << 32 | a->key_lo;
            out[got].rep_idx = a->rep;
            out[got].max_mtime = a->mtime;
            out[got].flags = a->flags;
            out[got].count = a->count;
            if (pinned)
                pinned[got] = in[j].pinned != 0;
            got++;
        }
    }
    close(fd);
    if (got >= 0 && libfile_sum_end(&st) != sec->sum)
        got = -1;
    if (got >= 0)
        *total = n;
    return got;
}
