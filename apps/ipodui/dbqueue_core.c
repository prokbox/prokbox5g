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

/* Queue by database index (#109): the pure part. See dbqueue_core.h and
 * docs/design/queue-by-index.md. */
#include <stdio.h>
#include <string.h>
#include "crc32.h"
#include "dbqueue_core.h"

#define DBQ_SENTINEL_DIR "/<dbq>/"

uint32_t dbq_crc(const void *p, uint32_t len)
{
    return crc_32(p, len, 0xffffffff);
}

uint32_t dbq_seq_next(uint32_t seq)
{
    seq = (seq + 1) & DBQ_SEQ_MASK;
    return seq ? seq : 1;
}

void dbq_block_seal(struct dbq_block *b, uint32_t seq, uint32_t count,
                    uint32_t mode, const uint32_t *vals)
{
    b->magic = DBQ_BLOCK_MAGIC;
    b->seq = seq;
    b->count = count;
    b->mode = mode;
    b->crc = dbq_crc(vals, count * sizeof(uint32_t));
}

bool dbq_block_check(const struct dbq_block *b, const uint32_t *vals,
                     uint32_t seq, uint32_t count)
{
    return b->magic == DBQ_BLOCK_MAGIC && b->seq == seq
           && b->count == count && b->mode <= DBQ_MODE_PATH
           && b->crc == dbq_crc(vals, count * sizeof(uint32_t));
}

void dbq_hdr_seal(struct dbq_hdr *h)
{
    h->magic = DBQ_HDR_MAGIC;
    h->crc = dbq_crc(h, offsetof(struct dbq_hdr, crc));
}

bool dbq_hdr_check(const struct dbq_hdr *h)
{
    return h->magic == DBQ_HDR_MAGIC && h->nblocks <= DBQ_MAX_BLOCKS
           && h->crc == dbq_crc(h, offsetof(struct dbq_hdr, crc));
}

const struct dbq_entry *dbq_hdr_find(const struct dbq_hdr *h, uint32_t tag,
                                     uint32_t first_k, uint32_t count)
{
    uint32_t i;

    if (!dbq_hdr_check(h) || first_k != 0)
        return NULL;
    for (i = 0; i < h->nblocks; i++)
    {
        const struct dbq_entry *e = &h->e[i];
        if (e->seq == dbq_tag_seq(tag) && e->count == count
            && e->mode <= DBQ_MODE_PATH && e->file <= 1)
            return e;
    }
    return NULL;
}

int dbq_sentinel(uint32_t k, char *buf, size_t len)
{
    int n;

    if (!buf || len == 0)
        return 0;
    n = snprintf(buf, len, DBQ_SENTINEL_DIR "%lu", (unsigned long)k);
    return n < 0 ? 0 : n;
}

bool dbq_is_sentinel(const char *path)
{
    return path && !strncmp(path, DBQ_SENTINEL_DIR,
                            sizeof(DBQ_SENTINEL_DIR) - 1);
}

const char *dbq_why_name(int why)
{
    static const char *const names[] = {
        "ok", "deleted", "lost", "io", "busy"
    };
    return why >= 0 && why <= DBQ_WHY_BUSY ? names[why] : "?";
}

int dbq_resolve_finish(int why, uint32_t k, char *buf, size_t len)
{
    if (!buf || len == 0)
        return 0;
    if (why != DBQ_OK || buf[0] != '/')
        dbq_sentinel(k, buf, len);
    return 0;
}

int dbq_check_name(const int32_t hdr[2], uint32_t expect_idx,
                   const char *name, int got, size_t len)
{
    int32_t tl = hdr[0];

    if (tl < 2 || (size_t)tl > len || got < tl)
        return DBQ_WHY_IO;
    if (expect_idx != DBQ_LOST && (uint32_t)hdr[1] != expect_idx)
        return DBQ_WHY_IO;      /* a seek that isn't this entry's name */
    if (name[tl - 1] != '\0' || name[0] != '/')
        return DBQ_WHY_IO;
    return DBQ_OK;
}

bool dbq_stamp_eq(const struct dbq_stamp *a, const struct dbq_stamp *b)
{
    return a->commitid == b->commitid && a->entries == b->entries
           && !strncmp(a->db_path, b->db_path, DBQ_PATH_LEN);
}

enum dbq_res dbq_res_mode(int mode, uint32_t v, const struct dbq_stamp *blk,
                          const struct dbq_stamp *act, bool dir_holds)
{
    if (v == DBQ_LOST)
        return DBQ_RES_LOST;
    switch (mode)
    {
    case DBQ_MODE_IDX:
        if (act && act->entries >= 0 && dbq_stamp_eq(blk, act)
            && (int32_t)v < act->entries)
            return DBQ_RES_LIVE;
        return dir_holds ? DBQ_RES_DISK : DBQ_RES_LOST;
    case DBQ_MODE_SEEK:
        return dir_holds ? DBQ_RES_SEEK : DBQ_RES_LOST;
    case DBQ_MODE_PATH:
        return DBQ_RES_PATH;
    default:
        return DBQ_RES_LOST;
    }
}

bool dbq_rebase_check(uint32_t *vals, uint32_t n,
                      const struct dbq_fp_ops *ops)
{
    uint32_t k, fo, fn;
    int32_t so, sn;
    int r;

    for (k = 0; k < n; k++)
    {
        if (ops->yield && (k & 255) == 255)
            ops->yield(ops->c);
        if (vals[k] == DBQ_LOST)
            continue;
        if (ops->fp(ops->c, 0, vals[k], &fo, &so) != DBQ_FP_OK)
        {
            vals[k] = DBQ_LOST;         /* gone in the old snapshot */
            continue;
        }
        r = ops->fp(ops->c, 1, vals[k], &fn, &sn);
        if (r == DBQ_FP_DELETED)
            continue;                   /* deleted by this commit */
        if (r == DBQ_FP_RANGE || fo != fn)
            return true;                /* renumbered (or shrunk) */
    }
    return false;
}

void dbq_rebase_seek(uint32_t *vals, uint32_t n,
                     const struct dbq_fp_ops *ops)
{
    uint32_t k, fo;
    int32_t so;

    for (k = 0; k < n; k++)
    {
        if (ops->yield && (k & 255) == 255)
            ops->yield(ops->c);
        if (vals[k] == DBQ_LOST
            || ops->fp(ops->c, 0, vals[k], &fo, &so) != DBQ_FP_OK || so < 0)
            vals[k] = DBQ_LOST;
        else
            vals[k] = (uint32_t)so;
    }
}

enum dbq_rebase dbq_rebase(uint32_t *vals, uint32_t n,
                           const struct dbq_fp_ops *ops)
{
    /* every slot, history included (H2): one mode per block */
    if (!dbq_rebase_check(vals, n, ops))
        return DBQ_REBASE_STAMP;
    dbq_rebase_seek(vals, n, ops);
    return DBQ_REBASE_SEEK;
}

int dbq_reserve_target(int cur, int setting, int want, int amount, bool keep)
{
    if (want < setting)
        want = setting;
    if (want == cur || (keep && want < cur))
        return 0;               /* a keeping reserve only ever grows */
    if (keep && amount > want)
        return -1;
    return want;
}

int dbq_reserve_copy(unsigned long *dst, const unsigned long *src,
                     int amount, int newcap, bool keep)
{
    if (!keep || amount <= 0)
        return 0;
    if (amount > newcap)
        amount = newcap;
    memcpy(dst, src, amount * sizeof(*dst));
    return amount;
}

static bool perm_less(const uint32_t *key, uint32_t a, uint32_t b)
{
    return key[a] < key[b] || (key[a] == key[b] && a < b);
}

static void sift(uint32_t *p, const uint32_t *key, uint32_t i, uint32_t n)
{
    for (;;)
    {
        uint32_t c = 2 * i + 1, t;
        if (c >= n)
            return;
        if (c + 1 < n && perm_less(key, p[c], p[c + 1]))
            c++;
        if (!perm_less(key, p[i], p[c]))
            return;
        t = p[i]; p[i] = p[c]; p[c] = t;
        i = c;
    }
}

void dbq_sort_perm(uint32_t *perm, const uint32_t *key, uint32_t n)
{
    uint32_t i, t;

    for (i = 0; i < n; i++)
        perm[i] = i;
    if (n < 2)
        return;
    for (i = n / 2; i-- > 0; )      /* heapsort: no recursion, no heap */
        sift(perm, key, i, n);
    for (i = n - 1; i > 0; i--)
    {
        t = perm[0]; perm[0] = perm[i]; perm[i] = t;
        sift(perm, key, 0, i);
    }
}
