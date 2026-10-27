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

/* iPod UI shell: the sync-time index format (P12, docs/phase12-brief.md
 * 3.5): header checks, the seal and its CRC, the folder signature, the
 * paged lists' reader side (directories, pages, the page cache, the load
 * rate) and the Notes names. Resident; the builder's half is
 * xindex_build.c. Pure (stdint/string and the firmware's crc_32), host
 * tested in tests/xindex. */
#include <stdint.h>
#include <string.h>
#include "crc32.h"
#include "ipodui/xindex_core.h"

uint32_t xidx_rec_size(int kind)
{
    switch (kind)
    {
        case XIDX_NOTES:    return sizeof(struct xnote_rec);
        case XIDX_CONTACTS: return sizeof(struct xcontact_rec);
        case XIDX_CAL:      return sizeof(struct xcal_rec);
        default:            return 0;
    }
}

uint32_t xidx_file_max(int kind)
{
    switch (kind)
    {
        case XIDX_NOTES:    return XIDX_NOTES_FILE_MAX;
        case XIDX_CONTACTS: return XIDX_CONTACTS_FILE_MAX;
        default:            return XIDX_FILE_MAX;
    }
}

int xidx_check(const struct xidx_hdr *h, uint32_t file_bytes)
{
    uint32_t rs;
    uint64_t rec_end, str_end;

    if (file_bytes < sizeof(*h))
        return XIDX_ESHORT;
    if (h->magic != XIDX_MAGIC || h->version != XIDX_VERSION)
        return XIDX_EMAGIC;
    rs = xidx_rec_size(h->kind);
    if (!rs)
        return XIDX_EKIND;
    if (file_bytes > xidx_file_max(h->kind)
        || (h->kind == XIDX_CAL && h->count > XCAL_MAX_INSTANCES))
        return XIDX_ECAP;
    rec_end = (uint64_t)h->rec_off + (uint64_t)h->count * rs;
    str_end = (uint64_t)h->str_off + h->str_bytes;
    if (h->rec_off < sizeof(*h) || (h->rec_off & 3) || rec_end > h->str_off
        || str_end > file_bytes)
        return XIDX_EBOUNDS;
    return XIDX_OK;
}

/* the body the CRC covers: records, tables and strings */
static uint32_t body_crc(const struct xidx_hdr *h)
{
    return crc_32((const uint8_t *)h + h->rec_off,
                  h->str_off + h->str_bytes - h->rec_off, 0xffffffffu);
}

int xidx_check_body(const void *file, uint32_t file_bytes)
{
    const struct xidx_hdr *h = file;
    int rc = xidx_check(h, file_bytes);

    if (rc != XIDX_OK)
        return rc;
    return body_crc(h) == h->crc ? XIDX_OK : XIDX_ECRC;
}

void xidx_seal(void *file, uint32_t file_bytes)
{
    struct xidx_hdr *h = file;

    (void)file_bytes;
    h->magic = XIDX_MAGIC;
    h->version = XIDX_VERSION;
    h->crc = body_crc(h);
}

#define FNV_PRIME 0x01000193u

static uint32_t fnv_byte(uint32_t h, uint8_t b)
{
    return (h ^ b) * FNV_PRIME;
}

static uint32_t fnv_le32(uint32_t h, uint32_t v)
{
    int i;

    for (i = 0; i < 4; i++, v >>= 8)
        h = fnv_byte(h, v & 0xff);
    return h;
}

uint32_t xidx_sig_add(uint32_t sig, const char *name, uint32_t size,
                      uint32_t mtime)
{
    while (*name)
        sig = fnv_byte(sig, (uint8_t)*name++);
    sig = fnv_byte(sig, 0);
    sig = fnv_le32(sig, size);
    return fnv_le32(sig, mtime);
}

/* -- Paged lists ------------------------------------------------------------ */

uint32_t xidx_page_rows(const struct xidx_pdir *dir, int npages, int p,
                        uint32_t total)
{
    if (p < 0 || p >= npages)
        return 0;
    return (p + 1 < npages ? dir[p + 1].row0 : total) - dir[p].row0;
}

int xidx_dir_check(const struct xidx_pdir *dir, int npages, uint32_t total,
                   uint32_t lo, uint32_t hi)
{
    int p;

    if (npages < 0 || (total && !npages) || (!total && npages))
        return XIDX_EBOUNDS;
    for (p = 0; p < npages; p++)
    {
        if ((p ? dir[p].row0 <= dir[p - 1].row0 : dir[p].row0 != 0)
            || dir[p].row0 >= total
            || dir[p].bytes < sizeof(struct xidx_page) + 1
            || dir[p].bytes > XIDX_PAGE_MAX
            || (dir[p].off & 3) || dir[p].off < lo || dir[p].off > hi
            || dir[p].bytes > hi - dir[p].off)
            return XIDX_EBOUNDS;
    }
    return XIDX_OK;
}

int xidx_dir_find(const struct xidx_pdir *dir, int npages, uint32_t row)
{
    int lo = 0, hi = npages - 1;

    if (npages <= 0 || row < dir[0].row0)
        return -1;
    while (lo < hi)
    {
        int mid = (lo + hi + 1) / 2;

        if (dir[mid].row0 <= row)
            lo = mid;
        else
            hi = mid - 1;
    }
    return lo;
}

int xidx_page_check(const void *pg, const struct xidx_pdir *d,
                    uint32_t rows, uint32_t rec_size)
{
    const struct xidx_page *h = pg;

    if (h->rows != rows || h->bytes != d->bytes
        || sizeof(*h) + (uint32_t)rows * rec_size >= d->bytes
        || ((const uint8_t *)pg)[d->bytes - 1] != 0)
        return XIDX_EBOUNDS;
    return XIDX_OK;
}

const void *xidx_page_rec(const void *pg, uint32_t i, uint32_t rec_size)
{
    const struct xidx_page *h = pg;

    if (i >= h->rows)
        return NULL;
    return (const uint8_t *)pg + sizeof(*h) + i * rec_size;
}

const char *xidx_page_str(const void *pg, uint32_t off)
{
    const struct xidx_page *h = pg;

    /* the page's last byte is 0: a string inside ends inside */
    if (off < sizeof(*h) || off >= h->bytes)
        return NULL;
    return (const char *)pg + off;
}

/* -- the reader's page cache ------------------------------------------------- */

void xidx_pc_init(struct xidx_pcache *c, void *buf, uint32_t cap)
{
    memset(c, 0, sizeof(*c));
    c->buf = buf;
    c->cap = cap;
}

static void pc_touch(struct xidx_pcache *c, int k)
{
    int i;

    if (c->clock == 0xff)
    {
        /* renumber the ages, oldest first, before the clock wraps */
        for (i = 0; i < c->n; i++)
            c->s[i].age = c->s[i].age / 2;
        c->clock = 0x80;
    }
    c->s[k].age = ++c->clock;
}

const void *xidx_pc_get(struct xidx_pcache *c, int list, int page)
{
    int i;

    for (i = 0; i < c->n; i++)
        if (c->s[i].list == list && c->s[i].page == page)
        {
            pc_touch(c, i);
            return c->buf + c->s[i].at;
        }
    return NULL;
}

/* drops slot k: the later slots' bytes move down over it */
static void pc_drop(struct xidx_pcache *c, int k)
{
    uint32_t at = c->s[k].at, len = c->s[k].bytes, end = at + len;
    int i;

    memmove(c->buf + at, c->buf + end, c->used - end);
    c->used -= len;
    for (i = k; i + 1 < c->n; i++)
        c->s[i] = c->s[i + 1];
    c->n--;
    for (i = 0; i < c->n; i++)
        if (c->s[i].at > at)
            c->s[i].at -= len;
}

void *xidx_pc_room(struct xidx_pcache *c, uint32_t bytes)
{
    bytes = (bytes + 3) & ~3u;          /* slots stay word-aligned */
    if (bytes > c->cap)
        return NULL;
    while (c->n && (c->n >= XIDX_PC_SLOTS || c->cap - c->used < bytes))
    {
        int i, old = 0;

        for (i = 1; i < c->n; i++)
            if (c->s[i].age < c->s[old].age)
                old = i;
        pc_drop(c, old);
    }
    if (c->cap - c->used < bytes)
        return NULL;
    return c->buf + c->used;
}

void xidx_pc_add(struct xidx_pcache *c, int list, int page, uint32_t bytes)
{
    struct xidx_pslot *s;
    uint32_t pad = (bytes + 3) & ~3u;   /* the next page stays aligned, as
                                           the file aligns its pages */

    if (c->n >= XIDX_PC_SLOTS || bytes > c->cap - c->used)
        return;
    if (pad > c->cap - c->used)
        pad = c->cap - c->used;
    s = &c->s[c->n];
    s->at = c->used;
    s->bytes = (uint16_t)pad;
    s->list = (uint8_t)list;
    s->page = (uint16_t)page;
    c->used += pad;
    pc_touch(c, c->n++);
}

const void *xidx_pc_peek(const struct xidx_pcache *c, int list, int page)
{
    int i;

    for (i = 0; i < c->n; i++)
        if (c->s[i].list == list && c->s[i].page == page)
            return c->buf + c->s[i].at;
    return NULL;
}

/* -- the page-load rate ------------------------------------------------------ */

unsigned xidx_rate_recent(const struct xidx_rate *r, uint32_t now,
                          uint32_t per)
{
    unsigned i, k = 0;

    for (i = 0; i < r->n; i++)
        if (now - r->t[i] < per)
            k++;
    return k;
}

void xidx_rate_add(struct xidx_rate *r, uint32_t now)
{
    r->t[r->at] = now;
    r->at = (uint8_t)((r->at + 1) % XIDX_RATE_N);
    if (r->n < XIDX_RATE_N)
        r->n++;
}

/* -- Notes ----------------------------------------------------------------- */

static int ext_is_txt(const char *name, int len)
{
    static const char ext[] = XNOTE_EXT;
    int i;

    if (len <= 4)
        return 0;
    for (i = 0; i < 4; i++)
    {
        char c = name[len - 4 + i];

        if (c >= 'A' && c <= 'Z')
            c += 'a' - 'A';
        if (c != ext[i])
            return 0;
    }
    return 1;
}

int xnote_display_len(const char *name, int is_dir)
{
    int len = (int)strlen(name);

    return !is_dir && ext_is_txt(name, len) ? len - 4 : len;
}

int xnote_is_note(const char *name)
{
    return name[0] != '.' && ext_is_txt(name, (int)strlen(name));
}
