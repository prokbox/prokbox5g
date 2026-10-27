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

/* The video sample index (see vix.h). */

#include <string.h>
#include "vix.h"

static void le32(uint8_t *p, uint32_t v)
{
    p[0] = v;
    p[1] = v >> 8;
    p[2] = v >> 16;
    p[3] = v >> 24;
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
           (uint32_t)p[3] << 24;
}

long vix_write(const struct mp4_movie *m, void *scratch, size_t scratch_len,
               vix_write_fn write, void *ctx)
{
    struct mp4_iter it;
    struct mp4_sample s;
    uint8_t hdr[VIX_HEADER_SIZE];
    uint8_t *batch;
    size_t iter_len = MP4_SCRATCH_MIN, cap, n = 0;
    long total;
    int rc;

    /* the iterator's windows first, then whole records: half each from
       4 KB up, else the iterator's minimum and the rest for records */
    if (scratch_len < iter_len + VIX_RECORD_SIZE)
        return MP4_E_SPACE;
    if (scratch_len >= 4096)
        iter_len = scratch_len / 2;
    batch = (uint8_t *)scratch + iter_len;
    cap = (scratch_len - iter_len) / VIX_RECORD_SIZE * VIX_RECORD_SIZE;

    if ((rc = mp4_iter_init(&it, m, scratch, iter_len)) < 0)
        return rc;

    memcpy(hdr, "VIX1", 4);
    le32(hdr + 4, VIX_VERSION);
    le32(hdr + 8, m->nsamples);
    le32(hdr + 12, VIX_UNITS);
    le32(hdr + 16, m->width | m->height << 16);
    le32(hdr + 20, (uint32_t)m->avcc.pos);
    le32(hdr + 24, (uint32_t)(m->avcc.end - m->avcc.pos));
    le32(hdr + 28, 0);
    if ((rc = write(ctx, hdr, sizeof(hdr))) < 0)
        return rc;
    total = sizeof(hdr);

    while ((rc = mp4_iter_next(&it, &s)) > 0)
    {
        uint8_t *r = batch + n;
        uint32_t flags = (s.key ? 1 : 0) | (uint32_t)(s.offset >> 32) << 4;
        if (s.size >= 1u << 24 || s.offset >= (uint64_t)1 << 36 ||
            s.pts_us < 0)
            return MP4_E_TABLE;
        le32(r, (uint32_t)s.offset);
        le32(r + 4, s.size | flags << 24);
        le32(r + 8, (uint32_t)(s.pts_us / 1000));
        n += VIX_RECORD_SIZE;
        if (n == cap)
        {
            if ((rc = write(ctx, batch, n)) < 0)
                return rc;
            total += n;
            n = 0;
        }
    }
    if (rc < 0)
        return rc;
    if (n)
    {
        if ((rc = write(ctx, batch, n)) < 0)
            return rc;
        total += n;
    }
    return total;
}

int vix_read_header(const uint8_t *buf, struct vix_header *h)
{
    if (memcmp(buf, "VIX1", 4) || rd32(buf + 4) != VIX_VERSION ||
        rd32(buf + 12) != VIX_UNITS)
        return MP4_E_FORMAT;
    h->count = rd32(buf + 8);
    h->width = rd32(buf + 16) & 0xffff;
    h->height = rd32(buf + 16) >> 16;
    h->avcc_off = rd32(buf + 20);
    h->avcc_len = rd32(buf + 24);
    return MP4_OK;
}

void vix_read_record(const uint8_t *rec, struct mp4_sample *s)
{
    uint32_t sf = rd32(rec + 4);
    s->offset = rd32(rec) | (uint64_t)(sf >> 28) << 32;
    s->size = sf & 0xffffff;
    s->key = (sf >> 24) & 1;
    s->pts_us = (int64_t)rd32(rec + 8) * 1000;
}

void vix_name(const char *path, uint64_t size, uint32_t mtime, char *out)
{
    static const char hex[] = "0123456789abcdef";
    uint64_t h = 0xcbf29ce484222325ull;         /* FNV-1a 64 */
    uint8_t tail[12];
    int i;

    for (; *path; path++)
        h = (h ^ (uint8_t)*path) * 0x100000001b3ull;
    for (i = 0; i < 8; i++)
        tail[i] = size >> (8 * i);
    for (i = 0; i < 4; i++)
        tail[8 + i] = mtime >> (8 * i);
    for (i = 0; i < 12; i++)
        h = (h ^ tail[i]) * 0x100000001b3ull;
    for (i = VIX_NAME_LEN - 1; i >= 0; i--, h >>= 4)
        out[i] = hex[h & 15];
    out[VIX_NAME_LEN] = '\0';
}
