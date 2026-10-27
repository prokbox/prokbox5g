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
 * iPod UI shell: module pack format, relocation, integrity (pure, host-tested)
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
#include "ipodui/modload_core.h"

#define ALIGN_UP(x, a) (((x) + (a) - 1) & ~((uint32_t)(a) - 1))

static uint32_t reloc_bytes(const struct modpak_ent *e)
{
    return ALIGN_UP(e->nreloc * 2u, 4u);
}

uint32_t modpak_read_bytes(const struct modpak_ent *e)
{
    return e->image + reloc_bytes(e);
}

uint32_t modpak_reloc_at(const struct modpak_ent *e)
{
    uint32_t end = e->bss_off + e->bss;

    if (end < e->image)
        end = e->image;
    return ALIGN_UP(end, 4u);
}

uint32_t modpak_footprint(const struct modpak_ent *e)
{
    return ALIGN_UP(modpak_reloc_at(e) + reloc_bytes(e), MOD_ALIGN);
}

int modpak_check(const struct modpak_hdr *h, uint32_t file_bytes,
                 uint32_t build_id)
{
    unsigned i;

    if (h->magic != MODPAK_MAGIC)
        return MODPAK_EMAGIC;
    if (h->version != MODPAK_VERSION)
        return MODPAK_EVERSION;
    if (h->build_id != build_id)
        return MODPAK_EBUILD;
    if (h->count > MODPAK_MAX || h->total != file_bytes)
        return MODPAK_EBOUNDS;
    for (i = 0; i < h->count; i++)
    {
        const struct modpak_ent *e = &h->ent[i];

        if (e->offset < MODPAK_INDEX || (e->offset % MODPAK_ALIGN)
            || (e->image & 3) || e->image == 0
            || e->image > 128u * 1024u      /* uint16 word indices */
            || e->bss_off < e->image
            || e->nexport * 4u > e->image
            || e->offset + modpak_read_bytes(e) > file_bytes
            || e->offset + modpak_read_bytes(e) < e->offset
            /* v2 labels: in range; Class A is always 32 MB */
            || e->mclass >= MOD_CLASS__N || e->min_ram >= MOD_RAM__N
            || e->kind >= MOD_KIND__N || e->exit_policy >= MOD_EXIT__N
            || (e->mclass == MOD_CLASS_A && e->min_ram != MOD_RAM_32))
            return MODPAK_EBOUNDS;
    }
    return MODPAK_OK;
}

const struct modpak_ent *modpak_find(const struct modpak_hdr *h, uint32_t id)
{
    unsigned i;

    for (i = 0; i < h->count && i < MODPAK_MAX; i++)
        if (h->ent[i].id == id)
            return &h->ent[i];
    return NULL;
}

int mod_rt_fill(struct mod_rt *r, const struct modpak_ent *e)
{
    if (e->bss > 0xffffu || e->nreloc > 0xffffu || e->nexport > 0xffffu
        || e->bss_off != e->image)
        return MODPAK_EBOUNDS;
    r->id = e->id;
    r->offset = e->offset;
    r->image = e->image;
    r->fletcher = e->fletcher;
    r->bss = (uint16_t)e->bss;
    r->nreloc = (uint16_t)e->nreloc;
    r->nexport = (uint16_t)e->nexport;
    r->mclass = e->mclass;
    r->min_ram = e->min_ram;
    r->kind = e->kind;
    r->exit_policy = e->exit_policy;
    r->abi_major = e->abi_major;
    r->abi_minor = e->abi_minor;
    r->work_kb = e->work_kb;
    return MODPAK_OK;
}

void mod_rt_ent(const struct mod_rt *r, struct modpak_ent *e)
{
    memset(e, 0, sizeof(*e));
    e->id = r->id;
    e->offset = r->offset;
    e->image = r->image;
    e->bss_off = r->image;
    e->bss = r->bss;
    e->nreloc = r->nreloc;
    e->nexport = r->nexport;
    e->fletcher = r->fletcher;
    e->mclass = r->mclass;
    e->min_ram = r->min_ram;
    e->abi_major = r->abi_major;
    e->abi_minor = r->abi_minor;
    e->work_kb = r->work_kb;
    e->kind = r->kind;
    e->exit_policy = r->exit_policy;
}

uint32_t mod_rt_footprint(const struct mod_rt *r)
{
    struct modpak_ent e;

    mod_rt_ent(r, &e);
    return modpak_footprint(&e);
}

int mod_why_hidden(const struct modpak_ent *e, int unit_ram,
                   unsigned classes)
{
    if (e->kind == MOD_KIND_SDK
        && (e->abi_major != SDK_ABI_MAJOR || e->abi_minor > SDK_ABI_MINOR))
        return MOD_WHY_ABI;
    if (e->min_ram > unit_ram)
        return MOD_WHY_RAM;
    if (e->mclass >= MOD_CLASS__N || !(classes & (1u << e->mclass)))
        return MOD_WHY_CLASS;
    return MOD_WHY_OK;
}

int mod_desc_check(const struct mod_desc *d, const struct modpak_ent *e)
{
    if (d->magic != MOD_HDR_MAGIC || d->mclass != e->mclass
        || d->min_ram != e->min_ram || d->kind != e->kind
        || d->exit_policy != e->exit_policy
        || d->abi_major != e->abi_major || d->abi_minor != e->abi_minor
        || d->work_kb != e->work_kb || d->borrow_kb != e->borrow_kb
        || (d->exit_policy == MOD_EXIT_GRACEFUL
            && d->exit_ms > SDK_EXIT_MS_MAX)
        /* a page-load rate: Class A only, and under the ceiling */
        || d->pages > MOD_PAGES_MAX
        || (d->pages && d->mclass != MOD_CLASS_A))
        return MODPAK_EHDR;
    return MODPAK_OK;
}

uint32_t mod_fletcher32(const void *p, size_t n)
{
    const uint8_t *b = p;
    uint32_t s1 = 0xffff, s2 = 0xffff;

    size_t words = (n + 1) / 2;

    while (words)
    {
        /* 359 words keep both sums under 2^32 before the fold */
        size_t k = words > 359 ? 359 : words;

        words -= k;
        while (k--)
        {
            uint32_t w = b[0];

            if (n > 1)
                w |= (uint32_t)b[1] << 8;
            s1 += w;
            s2 += s1;
            b += 2;
            n = n > 1 ? n - 2 : 0;
        }
        s1 = (s1 & 0xffff) + (s1 >> 16);
        s2 = (s2 & 0xffff) + (s2 >> 16);
    }
    s1 = (s1 & 0xffff) + (s1 >> 16);
    s2 = (s2 & 0xffff) + (s2 >> 16);
    return (s2 << 16) | s1;
}

int mod_relocate(void *image, uint32_t image_bytes, const uint16_t *rel,
                 uint32_t n, int32_t delta)
{
    uint32_t *w = image, i;

    for (i = 0; i < n; i++)
        if ((uint32_t)rel[i] * 4u + 4u > image_bytes)
            return MODPAK_ERELOC;
    for (i = 0; i < n; i++)
        w[rel[i]] += (uint32_t)delta;
    return MODPAK_OK;
}

int mod_settle(void *base, const struct modpak_ent *e, uint32_t link_base,
               uint32_t run_addr)
{
    uint8_t *b = base;
    uint32_t at = modpak_reloc_at(e), rb = reloc_bytes(e);

    memmove(b + at, b + e->image, rb);          /* at >= image */
    memset(b + e->image, 0, at - e->image);     /* .bss and padding */
    return mod_relocate(b, e->image, (const uint16_t *)(b + at),
                        e->nreloc, (int32_t)(run_addr - link_base));
}

long mod_fit(const struct mod_span *used, int n, uint32_t arena,
             uint32_t need)
{
    uint32_t at = 0;

    for (;;)
    {
        int i;
        bool clash = false;

        if (at + need > arena || at + need < at)
            return -1;
        for (i = 0; i < n; i++)
        {
            if (used[i].len && at < used[i].off + used[i].len
                && used[i].off < at + need)
            {
                at = ALIGN_UP(used[i].off + used[i].len, MOD_ALIGN);
                clash = true;
                break;
            }
        }
        if (!clash)
            return (long)at;
    }
}

/* The used spans: every placed module (all, or only those that can't go:
 * held, yielding unless free_yield, and keep), then the extra ones */
static int room_spans(const struct mod_room *r, struct mod_span *sp,
                      bool fixed_only, bool free_yield, int keep)
{
    int i, n = 0;

    for (i = 0; i < r->n && n < MOD_ROOM_SPANS; i++)
    {
        int a;

        if (!r->span(r->ctx, i, &sp[n]))
            continue;
        a = r->age(r->ctx, i);
        if (fixed_only && i != keep
            && (a >= 0 || (free_yield && a == MOD_AGE_YIELD)))
            continue;
        n++;
    }
    if (r->extra && n < MOD_ROOM_SPANS)
        n += r->extra(r->ctx, sp + n, MOD_ROOM_SPANS - n);
    return n;
}

int mod_room_possible(const struct mod_room *r, uint32_t arena,
                      uint32_t need, int keep)
{
    struct mod_span sp[MOD_ROOM_SPANS];

    if (mod_fit(sp, room_spans(r, sp, true, false, keep), arena, need) >= 0)
        return 1;
    if (mod_fit(sp, room_spans(r, sp, true, true, keep), arena, need) >= 0)
        return (int)MOD_ROOM_YIELD;
    return 0;
}

long mod_room_find(const struct mod_room *r, uint32_t arena, uint32_t need,
                   int keep)
{
    struct mod_span sp[MOD_ROOM_SPANS];
    long at = mod_fit(sp, room_spans(r, sp, false, false, keep), arena, need);
    int ok;

    if (at >= 0)
        return at;
    /* evict nothing that would not make the room in the end */
    ok = mod_room_possible(r, arena, need, keep);
    if (ok != 1)
        return ok == (int)MOD_ROOM_YIELD ? MOD_ROOM_YIELD : MOD_ROOM_NONE;
    if (r->compact)
    {
        r->compact(r->ctx);
        at = mod_fit(sp, room_spans(r, sp, false, false, keep), arena, need);
    }
    while (at < 0)
    {
        int i, v = -1, va = -1;

        for (i = 0; i < r->n; i++)
        {
            int a = i == keep ? MOD_AGE_HELD : r->age(r->ctx, i);
            struct mod_span s;

            if (a >= 0 && a > va && r->span(r->ctx, i, &s))
            {
                v = i;
                va = a;
            }
        }
        if (v < 0 || !r->evict)
            return MOD_ROOM_NONE;
        r->evict(r->ctx, v);
        if (r->compact)
            r->compact(r->ctx);
        at = mod_fit(sp, room_spans(r, sp, false, false, keep), arena, need);
    }
    return at;
}

int mod_file_put(const struct mod_fileio *io, void *h, uint32_t *size,
                 uint32_t off, const void *p, uint32_t n)
{
    static const uint32_t zero[16];

    if (*size < off)
    {
        if (io->seek(h, *size) != (long)*size)
            return -1;
        while (*size < off)
        {
            uint32_t k = off - *size;

            if (k > sizeof(zero))
                k = sizeof(zero);
            if (io->write(h, zero, k) != (long)k)
                return -1;
            *size += k;
        }
    }
    if (io->seek(h, off) != (long)off || io->write(h, p, n) != (long)n)
        return -1;
    if (off + n > *size)
        *size = off + n;
    return 0;
}
