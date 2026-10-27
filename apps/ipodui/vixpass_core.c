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
 * iPod UI shell: video index pass, the pure part (Phase 14a)
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
#include "vixpass_core.h"

typedef char vpc_size_check[sizeof(struct vixpass_ent) == 16
                            && sizeof(struct vpc_hdr) == 16 ? 1 : -1];

uint32_t vpc_fnv(uint32_t h, const void *p, size_t n)
{
    const uint8_t *b = p;

    while (n--)
        h = (h ^ *b++) * 16777619u;
    return h;
}

static uint32_t fnv_str(uint32_t h, const char *s)
{
    return vpc_fnv(h, s ? s : "", s ? strlen(s) : 0);
}

uint32_t vpc_id(const char *title, const char *album, uint32_t length,
                int32_t name_seek)
{
    uint32_t h = VPC_FNV_INIT;

    h = fnv_str(h, title);
    h = vpc_fnv(h, "\x1f", 1);
    h = fnv_str(h, album);
    h = vpc_fnv(h, "\x1f", 1);
    h = vpc_fnv(h, &length, sizeof(length));
    return vpc_fnv(h, &name_seek, sizeof(name_seek));
}

uint32_t vpc_sum(uint32_t h, const struct vixpass_ent *e, uint32_t n)
{
    return vpc_fnv(h, e, (size_t)n * sizeof(*e));
}

bool vpc_hdr_ok(const struct vpc_hdr *h, uint32_t cap)
{
    return h->magic == VPC_MAGIC && h->version == VPC_VERSION
        && h->count <= cap;
}

int vpc_cmp(const struct vixpass_ent *a, uint32_t id, uint32_t mtime)
{
    if (a->id != id)
        return a->id < id ? -1 : 1;
    return a->mtime == mtime ? 0 : (a->mtime < mtime ? -1 : 1);
}

int vpc_match(const struct vixpass_ent *old, int nold, uint8_t *used,
              uint32_t id, uint32_t mtime)
{
    int lo = 0, hi = nold, k = -1;

    while (lo < hi)
    {
        int mid = (lo + hi) / 2, c = vpc_cmp(&old[mid], id, mtime);

        if (c == 0)
        {
            k = mid;
            break;
        }
        if (c < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (k < 0)
        return -1;
    /* copies: the first unused record of the run */
    while (k > 0 && !vpc_cmp(&old[k - 1], id, mtime))
        k--;
    while (k < nold && !vpc_cmp(&old[k], id, mtime) && VPC_USED(used, k))
        k++;
    if (k >= nold || vpc_cmp(&old[k], id, mtime))
        return -1;
    VPC_MARK(used, k);
    return k;
}

int vpc_claim_name(const struct vixpass_ent *old, int nold, uint8_t *used,
                   uint64_t name)
{
    int k;

    if (!name)
        return -1;
    for (k = 0; k < nold; k++)
        if (old[k].name == name && !VPC_USED(used, k))
        {
            VPC_MARK(used, k);
            return k;
        }
    return -1;
}

bool vpc_has_name(const struct vixpass_ent *e, int n, uint64_t name)
{
    int i;

    for (i = 0; i < n; i++)
        if (e[i].name == name)
            return true;
    return false;
}

bool vpc_has_name64(const uint64_t *names, int n, uint64_t name)
{
    int i;

    for (i = 0; i < n; i++)
        if (names[i] == name)
            return true;
    return false;
}

/* Shell sort: a few hundred records at most */
void vpc_sort(struct vixpass_ent *e, int32_t *pend, int n)
{
    int gap, i, j;

    for (gap = n / 2; gap > 0; gap /= 2)
        for (i = gap; i < n; i++)
        {
            struct vixpass_ent t = e[i];
            int32_t p = pend ? pend[i] : 0;

            for (j = i; j >= gap && vpc_cmp(&e[j - gap], t.id, t.mtime) > 0;
                 j -= gap)
            {
                e[j] = e[j - gap];
                if (pend)
                    pend[j] = pend[j - gap];
            }
            e[j] = t;
            if (pend)
                pend[j] = p;
        }
}

int vpc_keep_resolved(struct vixpass_ent *e, const int32_t *pend, int n)
{
    int i, m = 0;

    for (i = 0; i < n; i++)
        if (pend[i] < 0)
            e[m++] = e[i];
    return m;
}

int vpc_keep_unused(struct vixpass_ent *e, const uint8_t *used, int n)
{
    int i, m = 0;

    for (i = 0; i < n; i++)
        if (!VPC_USED(used, i) && e[i].name)
            e[m++] = e[i];
    return m;
}

int vpc_merge(const struct vixpass_ent *a, int na, int *ia,
              const struct vixpass_ent *b, int nb, int *ib,
              struct vixpass_ent *out, int cap)
{
    int n = 0;

    while (n < cap && (*ia < na || *ib < nb))
    {
        if (*ib >= nb
            || (*ia < na && vpc_cmp(&a[*ia], b[*ib].id, b[*ib].mtime) <= 0))
            out[n++] = a[(*ia)++];
        else
            out[n++] = b[(*ib)++];
    }
    return n;
}

bool vpc_hex_name(const char *s, uint64_t *name)
{
    uint64_t h = 0;
    int i;

    for (i = 0; i < VPC_NAME_LEN; i++)
    {
        char c = s[i];
        int d = c >= '0' && c <= '9' ? c - '0'
              : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;

        if (d < 0)
            return false;
        h = h << 4 | (uint64_t)d;
    }
    *name = h;
    return true;
}

bool vpc_dir_name(const char *d_name, uint64_t *name)
{
    return strlen(d_name) == VPC_NAME_LEN + 4
        && !strcmp(d_name + VPC_NAME_LEN, ".vix")
        && vpc_hex_name(d_name, name);
}
