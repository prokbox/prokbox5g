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

/* iPod UI shell: the sync-time index builder's half (P12 W3-K): pages
 * (split at a target and XIDX_PAGE_ROWS_MIN rows, written), row labels
 * and the Notes layout. Only the indexer uses it (MOD_XINDEX in a module
 * build, apps/ipodui/modules/modules.make; xcontact_core.c and the Notes
 * builder), so the resident shell keeps only the reader's half
 * (xindex_core.c). Pure, host tested in tests/xindex. */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "ipodui/xindex_core.h"

/* -- Paged lists ------------------------------------------------------------ */

int xidx_page_split(uint32_t rows, uint32_t rec_size, uint32_t target,
                    xidx_row_len_fn len, void *ctx, struct xidx_pdir *dir,
                    int dir_max)
{
    uint32_t r, bytes = 0;
    int n = 0;

    for (r = 0; r < rows; r++)
    {
        uint32_t add = rec_size + len(ctx, r);

        /* this row opens the next page: past the target, once the page
         * holds its minimum */
        if (n && r - dir[n - 1].row0 >= XIDX_PAGE_ROWS_MIN
            && bytes + add > target)
            bytes = 0;
        if (!bytes)
        {
            if (n >= dir_max || r > 0xffff)
                return XIDX_ECAP;
            dir[n].off = 0;
            dir[n].row0 = (uint16_t)r;
            dir[n].bytes = 0;
            bytes = sizeof(struct xidx_page);
            n++;
        }
        bytes += add;
        if (bytes > XIDX_PAGE_MAX)
            return XIDX_ECAP;
        dir[n - 1].bytes = (uint16_t)bytes;
    }
    return n;
}

void xidx_page_write(void *dst, const struct xidx_pdir *dir, int npages,
                     int p, uint32_t total, uint32_t rec_size,
                     xidx_row_put_fn put, void *ctx)
{
    struct xidx_page *pg = dst;
    uint32_t rows = xidx_page_rows(dir, npages, p, total), i;
    uint8_t *recs = (uint8_t *)dst + sizeof(*pg);
    uint32_t at = sizeof(*pg) + rows * rec_size;

    pg->rows = (uint16_t)rows;
    pg->bytes = dir[p].bytes;
    /* each row's strings follow the previous row's */
    for (i = 0; i < rows && at <= dir[p].bytes; i++)
    {
        memset(recs + i * rec_size, 0, rec_size);
        at += put(ctx, dir[p].row0 + i, recs + i * rec_size,
                  (char *)dst + at, at);
    }
}

uint32_t xidx_label_put(char *out, const char *s, uint32_t n)
{
    uint32_t cut;

    if (n < XIDX_LABEL_MAX)
    {
        if (out)
        {
            memcpy(out, s, n);
            out[n] = '\0';
        }
        return n + 1;
    }
    /* room for the ellipsis and the NUL; at a character, no space left
     * before the ellipsis */
    cut = XIDX_LABEL_MAX - 1 - (sizeof(XIDX_ELLIPSIS) - 1);
    while (cut > 0 && ((uint8_t)s[cut] & 0xc0) == 0x80)
        cut--;
    while (cut > 0 && s[cut - 1] == ' ')
        cut--;
    if (out)
    {
        memcpy(out, s, cut);
        memcpy(out + cut, XIDX_ELLIPSIS, sizeof(XIDX_ELLIPSIS));
    }
    return cut + sizeof(XIDX_ELLIPSIS);
}

/* -- Notes ----------------------------------------------------------------- */

/* Sort order: the root's entries first, then each folder's, by name */
struct sort_ctx {
    const struct xnote_src *src;
    const char *pool;
    xnote_cmp_fn cmp;
};

static int group(const struct xnote_src *e)
{
    return e->parent == XNOTE_PARENT_ROOT ? 0 : e->parent + 1;
}

static int less(const struct sort_ctx *c, uint16_t a, uint16_t b)
{
    const struct xnote_src *x = &c->src[a], *y = &c->src[b];
    const char *xn = c->pool + x->name, *yn = c->pool + y->name;
    int gx = group(x), gy = group(y), r;

    if (gx != gy)
        return gx < gy;
    r = c->cmp(xn, xnote_display_len(xn, x->dir),
               yn, xnote_display_len(yn, y->dir));
    return r ? r < 0 : a < b;           /* stable: the walk's order */
}

static void sift(const struct sort_ctx *c, uint16_t *p, int i, int n)
{
    for (;;)
    {
        int k = 2 * i + 1;
        uint16_t t;

        if (k >= n)
            return;
        if (k + 1 < n && less(c, p[k], p[k + 1]))
            k++;
        if (!less(c, p[i], p[k]))
            return;
        t = p[i]; p[i] = p[k]; p[k] = t;
        i = k;
    }
}

/* Heap sort: O(n log n) compares, no recursion, no extra memory */
static void sort_perm(const struct sort_ctx *c, uint16_t *p, int n)
{
    int i;

    for (i = n / 2 - 1; i >= 0; i--)
        sift(c, p, i, n);
    for (i = n - 1; i > 0; i--)
    {
        uint16_t t = p[0];

        p[0] = p[i]; p[i] = t;
        sift(c, p, 0, i);
    }
}

struct emit {
    const struct xnote_src *src;
    const char *pool;
    const uint16_t *row_src, *first, *cnt;  /* row -> entry; a folder's */
    uint32_t far;                       /* the next far name's offset (the
                                           rows are written in order) */
};

#define ROW_ROOT 0xffffu

static const char *row_name(const struct emit *e, uint32_t row)
{
    uint16_t i = e->row_src[row];

    return i == ROW_ROOT ? "" : e->pool + e->src[i].name;
}

/* A name of a label's size or more is far: its label is the display
 * name (cut) and the name goes to the string block */
static bool row_far(const struct emit *e, uint32_t row)
{
    return strlen(row_name(e, row)) + 1 > XIDX_LABEL_MAX;
}

static uint32_t row_label(const struct emit *e, uint32_t row, char *out)
{
    const char *name = row_name(e, row);
    uint16_t i = e->row_src[row];

    if (!row_far(e, row))
        return xidx_label_put(out, name, strlen(name));
    return xidx_label_put(out, name, xnote_display_len(name, e->src[i].dir));
}

static uint32_t note_len(void *ctx, uint32_t row)
{
    return row_label(ctx, row, NULL);
}

static uint32_t note_put(void *ctx, uint32_t row, void *rec, char *str,
                         uint32_t str_rel)
{
    struct emit *e = ctx;
    struct xnote_rec *r = rec;
    uint32_t len = row_label(e, row, str);
    uint16_t i = e->row_src[row];

    r->name_off = r->path_off = str_rel;
    if (row_far(e, row))
    {
        r->path_off = XNOTE_PATH_FAR | e->far;
        e->far += strlen(row_name(e, row)) + 1;
    }
    if (i == ROW_ROOT)
        r->size = XNOTE_DIR_SIZE(1, e->cnt[0]);
    else if (e->src[i].dir)
        r->size = XNOTE_DIR_SIZE(e->first[i], e->cnt[i + 1]);
    else
        r->size = e->src[i].size < XNOTE_DIR ? e->src[i].size
                                             : XNOTE_DIR - 1;
    return len;
}

int32_t xnote_emit(void *out, uint32_t out_max, const struct xnote_src *src,
                   int n, const char *pool, uint32_t sig, xnote_cmp_fn cmp,
                   uint16_t *scratch)
{
    struct xidx_hdr *h = out;
    struct xnote_tab *tab = (struct xnote_tab *)(h + 1);
    struct xidx_pdir *dir = (struct xidx_pdir *)(tab + 1);
    struct sort_ctx c = { src, pool, cmp };
    /* perm (n) | a folder's run in perm, then its first row (n) | the
     * counts: [0] the root's, [f + 1] folder f's (n + 1) | row -> entry
     * (n + 1) */
    uint16_t *perm = scratch, *start = scratch + n, *cnt = scratch + 2 * n,
             *row_src = scratch + 3 * n + 1;
    uint32_t at, rows, dir_max, r, far;
    struct emit e = { src, pool, row_src, start, cnt, 0 };
    int i, np, nroot = 0;

    if (n < 0 || n > XNOTE_MAX || out_max < 64)
        return XIDX_ECAP;
    for (i = 0; i < n; i++)
    {
        perm[i] = i;
        start[i] = cnt[i + 1] = 0;
        if (src[i].parent == XNOTE_PARENT_ROOT)
            nroot++;
        else if (src[i].parent >= n || !src[src[i].parent].dir
                 || src[src[i].parent].parent != XNOTE_PARENT_ROOT)
            return XIDX_EBOUNDS;
    }
    cnt[0] = nroot;
    sort_perm(&c, perm, n);
    /* each folder's run in the sorted order */
    for (i = nroot; i < n; i++)
    {
        uint16_t f = src[perm[i]].parent;

        if (!cnt[f + 1])
            start[f] = i;
        cnt[f + 1]++;
    }
    /* the rows: record 0 the folder itself, the root's entries, then each
     * folder's notes in the root's order of the folders */
    row_src[0] = ROW_ROOT;
    rows = 1;
    for (i = 0; i < nroot; i++)
        row_src[rows++] = perm[i];
    for (i = 0; i < nroot; i++)
    {
        uint16_t f = perm[i];
        int k, k0 = start[f];

        if (!src[f].dir)
            continue;
        start[f] = rows;                /* now: its first row */
        for (k = k0; k < k0 + cnt[f + 1]; k++)
            row_src[rows++] = perm[k];
    }
    memset(h, 0, sizeof(*h) + sizeof(*tab));
    dir_max = (out_max - 64) / sizeof(*dir);
    if (dir_max > XNOTE_PDIR_MAX)
        dir_max = XNOTE_PDIR_MAX;
    np = xidx_page_split(rows, sizeof(struct xnote_rec), XNOTE_PAGE_BYTES,
                         note_len, &e, dir, dir_max);
    if (np < 0)
        return XIDX_ECAP;
    tab->npage = np;
    tab->hdr_end = at = sizeof(*h) + sizeof(*tab) + np * sizeof(*dir);
    for (i = 0; i < np; i++)
    {
        if (at + dir[i].bytes > out_max)
            return XIDX_ECAP;
        dir[i].off = at;
        xidx_page_write((uint8_t *)out + at, dir, np, i, rows,
                        sizeof(struct xnote_rec), note_put, &e);
        at += dir[i].bytes;
        while (at & 3)
        {
            if (at >= out_max)
                return XIDX_ECAP;
            ((uint8_t *)out)[at++] = 0;
        }
    }
    /* the string block: the far names, in row order (note_put's) */
    for (r = 0, far = 0; r < rows; r++)
    {
        const char *name = row_name(&e, r);
        uint32_t len = strlen(name) + 1;

        if (!row_far(&e, r))
            continue;
        if (at + far + len > out_max)
            return XIDX_ECAP;
        memcpy((uint8_t *)out + at + far, name, len);
        far += len;
    }
    h->kind = XIDX_NOTES;
    h->sig = sig;
    h->count = rows;
    h->rec_off = sizeof(*h);
    h->str_off = at;
    h->str_bytes = far;
    xidx_seal(out, at + far);
    return (int32_t)(at + far);
}
