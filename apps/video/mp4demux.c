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

/* MP4/M4V demuxer (see mp4demux.h).  It follows tests/video/mp4demux.py,
   the reference, step for step: same track choice, same table expansion,
   same PTS arithmetic, same refusals. */

#include <string.h>
#include "mp4demux.h"

#define FOURCC(a, b, c, d) \
    ((uint32_t)(a) << 24 | (uint32_t)(b) << 16 | (uint32_t)(c) << 8 | (d))

#define T_MOOV FOURCC('m','o','o','v')
#define T_MOOF FOURCC('m','o','o','f')
#define T_MVEX FOURCC('m','v','e','x')
#define T_TRAK FOURCC('t','r','a','k')
#define T_MDIA FOURCC('m','d','i','a')
#define T_MDHD FOURCC('m','d','h','d')
#define T_HDLR FOURCC('h','d','l','r')
#define T_MINF FOURCC('m','i','n','f')
#define T_STBL FOURCC('s','t','b','l')
#define T_STSD FOURCC('s','t','s','d')
#define T_EDTS FOURCC('e','d','t','s')
#define T_ELST FOURCC('e','l','s','t')
#define T_AVC1 FOURCC('a','v','c','1')
#define T_AVC3 FOURCC('a','v','c','3')
#define T_AVCC FOURCC('a','v','c','C')
#define T_STSZ FOURCC('s','t','s','z')
#define T_STCO FOURCC('s','t','c','o')
#define T_CO64 FOURCC('c','o','6','4')
#define T_STSC FOURCC('s','t','s','c')
#define T_STTS FOURCC('s','t','t','s')
#define T_CTTS FOURCC('c','t','t','s')
#define T_STSS FOURCC('s','t','s','s')
#define T_VIDE FOURCC('v','i','d','e')
#define T_SOUN FOURCC('s','o','u','n')

static uint16_t be16(const uint8_t *p)
{
    return (uint16_t)(p[0] << 8 | p[1]);
}

static uint32_t be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 |
           (uint32_t)p[2] << 8 | p[3];
}

static uint64_t be64(const uint8_t *p)
{
    return (uint64_t)be32(p) << 32 | be32(p + 4);
}

static int read_exact(const struct mp4_io *io, uint64_t off, void *buf,
                      unsigned long len)
{
    if (off > io->size || len > io->size - off)
        return MP4_E_FORMAT;
    return io->read_at(io->ctx, off, buf, len) == (long)len ? MP4_OK
                                                             : MP4_E_IO;
}

static int read_u32(const struct mp4_io *io, uint64_t off, uint32_t *v)
{
    uint8_t b[4];
    int rc = read_exact(io, off, b, 4);
    *v = be32(b);
    return rc;
}

/* The box at p inside [p, end): 1 and its type and payload, 0 if fewer
   than 8 bytes are left (trailing bytes are ignored, as the reference
   does), or < 0. */
static int box_at(const struct mp4_io *io, uint64_t p, uint64_t end,
                  uint32_t *type, struct mp4_range *pl)
{
    uint8_t h[16];
    uint64_t size;
    unsigned hdr = 8;
    int rc;

    if (end < p + 8)
        return 0;
    if ((rc = read_exact(io, p, h, 8)) < 0)
        return rc;
    size = be32(h);
    *type = be32(h + 4);
    if (size == 1)
    {
        if (end < p + 16)
            return MP4_E_FORMAT;
        if ((rc = read_exact(io, p + 8, h + 8, 8)) < 0)
            return rc;
        size = be64(h + 8);
        hdr = 16;
    }
    else if (size == 0)
        size = end - p;
    if (size < hdr || size > end - p)
        return MP4_E_FORMAT;
    pl->pos = p + hdr;
    pl->end = p + size;
    return 1;
}

/* The payload of the first box along path inside r; 1 found, 0 not, < 0. */
static int find(const struct mp4_io *io, struct mp4_range r,
                const uint32_t *path, int depth, struct mp4_range *out)
{
    struct mp4_range pl;
    uint32_t type;
    uint64_t p = r.pos;
    int rc;

    while ((rc = box_at(io, p, r.end, &type, &pl)) > 0)
    {
        if (type == path[0])
            return depth == 1 ? (*out = pl, 1)
                              : find(io, pl, path + 1, depth - 1, out);
        p = pl.end;
    }
    return rc;
}

#define FIND(io, r, out, ...) \
    find((io), (r), (const uint32_t[]){ __VA_ARGS__ }, \
         (int)(sizeof((const uint32_t[]){ __VA_ARGS__ }) / sizeof(uint32_t)), \
         (out))

struct trak
{
    uint32_t handler;
    uint32_t timescale;
    int64_t shift;
    bool empty_edit;
    struct mp4_range stbl;
};

static int parse_trak(const struct mp4_io *io, struct mp4_range t,
                      struct trak *tk)
{
    struct mp4_range r;
    uint8_t b[24];
    int rc;

    memset(tk, 0, sizeof(*tk));
    if ((rc = FIND(io, t, &r, T_MDIA, T_HDLR)) < 0)
        return rc;
    if (rc > 0 && (rc = read_u32(io, r.pos + 8, &tk->handler)) < 0)
        return rc;

    if ((rc = FIND(io, t, &r, T_MDIA, T_MDHD)) <= 0)
        return rc < 0 ? rc : MP4_E_FORMAT;
    if ((rc = read_exact(io, r.pos, b, 1)) < 0)
        return rc;
    rc = read_u32(io, r.pos + (b[0] == 1 ? 20 : 12), &tk->timescale);
    if (rc < 0)
        return rc;

    if ((rc = FIND(io, t, &tk->stbl, T_MDIA, T_MINF, T_STBL)) <= 0)
        return rc < 0 ? rc : MP4_E_FORMAT;

    /* only the first edit counts (mp4demux.py edit_shift) */
    if ((rc = FIND(io, t, &r, T_EDTS, T_ELST)) < 0)
        return rc;
    if (rc > 0)
    {
        uint32_t n;
        if ((rc = read_exact(io, r.pos, b, 8)) < 0)
            return rc;
        n = be32(b + 4);
        if (n > 0)
        {
            if (b[0] == 1)
            {
                if ((rc = read_exact(io, r.pos + 8, b, 16)) < 0)
                    return rc;
                tk->shift = (int64_t)be64(b + 8);
            }
            else
            {
                if ((rc = read_exact(io, r.pos + 8, b, 8)) < 0)
                    return rc;
                tk->shift = (int32_t)be32(b + 4);
            }
            tk->empty_edit = tk->shift == -1;
        }
    }
    return MP4_OK;
}

/* Walks avcC's parameter sets.  With out, writes them as Annex B to
   out[*w...]; returns MP4_OK or MP4_E_BAD_AVCC. */
static int avcc_sets(const uint8_t *c, uint32_t len, uint8_t *out,
                     size_t *w)
{
    uint32_t p = 5;
    int pass;

    if (len < 7 || c[0] != 1)
        return MP4_E_BAD_AVCC;
    for (pass = 0; pass < 2; pass++)
    {
        unsigned n;
        if (p >= len)
            return MP4_E_BAD_AVCC;
        n = c[p++] & (pass ? 0xff : 0x1f);
        while (n--)
        {
            uint32_t sl;
            if (p + 2 > len)
                return MP4_E_BAD_AVCC;
            sl = be16(c + p);
            if (sl > len - p - 2)
                return MP4_E_BAD_AVCC;
            if (out)
            {
                static const uint8_t sc[4] = { 0, 0, 0, 1 };
                memcpy(out + *w, sc, 4);
                memmove(out + *w + 4, c + p + 2, sl);
                *w += 4 + sl;
            }
            p += 2 + sl;
        }
    }
    return MP4_OK;
}

static int parse_avc_entry(const struct mp4_io *io, struct mp4_range stsd,
                           struct mp4_movie *m)
{
    struct mp4_range e, c;
    uint32_t type;
    uint64_t p = stsd.pos + 8;
    int rc;

    while ((rc = box_at(io, p, stsd.end, &type, &e)) > 0)
    {
        p = e.end;
        if (type != T_AVC1 && type != T_AVC3)
            continue;
        /* VisualSampleEntry: width and height at +24, 78 bytes, then
           child boxes */
        {
            uint8_t wh[4];
            uint32_t ct;
            uint64_t q = e.pos + 78;
            if ((rc = read_exact(io, e.pos + 24, wh, 4)) < 0)
                return rc;
            while ((rc = box_at(io, q, e.end, &ct, &c)) > 0)
            {
                if (ct == T_AVCC)
                {
                    m->width = be16(wh);
                    m->height = be16(wh + 2);
                    m->avcc = c;
                    return 1;
                }
                q = c.end;
            }
            if (rc < 0)
                return rc;
        }
    }
    return rc;
}

int mp4_open(struct mp4_movie *m, const struct mp4_io *io)
{
    struct mp4_range file = { 0, io->size }, moov = { 0, 0 }, pl, r;
    struct trak tk, vt;
    uint32_t type;
    uint64_t p;
    bool moof = false, have_video = false;
    int rc;

    memset(m, 0, sizeof(*m));
    m->io = io;

    for (p = 0; (rc = box_at(io, p, file.end, &type, &pl)) > 0; p = pl.end)
    {
        if (type == T_MOOV)
            moov = pl;          /* the last one, like the reference's dict */
        else if (type == T_MOOF)
            moof = true;
    }
    if (rc < 0)
        return rc;
    if (moof)
        return MP4_E_FRAGMENTED;
    if (!moov.end)
        return MP4_E_NO_MOOV;
    if ((rc = FIND(io, moov, &r, T_MVEX)) != 0)
        return rc < 0 ? rc : MP4_E_FRAGMENTED;

    for (p = moov.pos; (rc = box_at(io, p, moov.end, &type, &pl)) > 0;
         p = pl.end)
    {
        if (type != T_TRAK)
            continue;
        if ((rc = parse_trak(io, pl, &tk)) < 0)
            return rc;
        if (tk.handler == T_VIDE && !have_video)
        {
            if ((rc = FIND(io, tk.stbl, &r, T_STSD)) < 0)
                return rc;
            if (rc > 0 && (rc = parse_avc_entry(io, r, m)) < 0)
                return rc;
            if (rc > 0)
            {
                have_video = true;
                vt = tk;
            }
        }
        else if (tk.handler == T_SOUN)
            m->has_audio = true;
    }
    if (rc < 0)
        return rc;
    if (!have_video)
        return MP4_E_NO_AVC;

    /* avcC: the NAL length size, and every set must parse */
    {
        uint8_t c[512];
        uint32_t len = (uint32_t)(m->avcc.end - m->avcc.pos);
        if (len > sizeof(c) || len < 7)
            return MP4_E_BAD_AVCC;
        if ((rc = read_exact(io, m->avcc.pos, c, len)) < 0)
            return rc;
        if ((rc = avcc_sets(c, len, NULL, NULL)) < 0)
            return rc;
        m->nal_length = (c[4] & 3) + 1;
    }

    if (vt.empty_edit)
        return MP4_E_EMPTY_EDIT;
    m->shift = vt.shift;
    m->timescale = vt.timescale;
    if (!m->timescale)
        return MP4_E_FORMAT;

    /* the sample-table boxes; for repeats the last wins (the reference
       keeps a dict) */
    for (p = vt.stbl.pos; (rc = box_at(io, p, vt.stbl.end, &type, &pl)) > 0;
         p = pl.end)
    {
        switch (type)
        {
            case T_STSZ: m->stsz = pl; break;
            case T_STCO: m->stco = pl; m->co64 = false; break;
            case T_CO64: if (!m->stco.end || m->co64)
                             m->stco = pl, m->co64 = true;
                         break;
            case T_STSC: m->stsc = pl; break;
            case T_STTS: m->stts = pl; break;
            case T_CTTS: m->ctts = pl; break;
            case T_STSS: m->stss = pl; break;
        }
    }
    if (rc < 0)
        return rc;
    if (!m->stsz.end || !m->stco.end || !m->stsc.end || !m->stts.end)
        return MP4_E_TABLE;
    return read_u32(io, m->stsz.pos + 8, &m->nsamples);
}

/* --- the sample iterator ------------------------------------------------ */

static void cur_init(struct mp4_cursor *c, const struct mp4_io *io,
                     uint64_t pos, uint64_t end, uint8_t *buf, uint32_t cap)
{
    c->io = io;
    c->pos = pos;
    c->end = end;
    c->buf = buf;
    c->cap = cap;
    c->fill = c->at = 0;
}

static int cur_u32(struct mp4_cursor *c, uint32_t *v)
{
    if (c->at + 4 > c->fill)
    {
        uint64_t left;
        long n;
        c->pos += c->at;
        c->at = 0;
        left = c->end > c->pos ? c->end - c->pos : 0;
        n = (long)(left < c->cap ? left : c->cap);
        if (n < 4)
            return MP4_E_TABLE;
        if (c->io->read_at(c->io->ctx, c->pos, c->buf, (unsigned long)n) != n)
            return MP4_E_IO;
        c->fill = (uint32_t)n;
    }
    *v = be32(c->buf + c->at);
    c->at += 4;
    return MP4_OK;
}

static int cur_u64(struct mp4_cursor *c, uint64_t *v)
{
    uint32_t hi, lo;
    int rc;
    if ((rc = cur_u32(c, &hi)) < 0 || (rc = cur_u32(c, &lo)) < 0)
        return rc;
    *v = (uint64_t)hi << 32 | lo;
    return MP4_OK;
}

/* Reads the next stsc entry into first/per (desc is skipped). */
static int sc_read(struct mp4_iter *it, uint32_t *first, uint32_t *per)
{
    uint32_t desc;
    int rc;
    if ((rc = cur_u32(&it->sc, first)) < 0 ||
        (rc = cur_u32(&it->sc, per)) < 0 ||
        (rc = cur_u32(&it->sc, &desc)) < 0)
        return rc;
    it->sc_left--;
    return MP4_OK;
}

int mp4_iter_init(struct mp4_iter *it, const struct mp4_movie *m,
                  void *scratch, size_t scratch_len)
{
    const struct mp4_io *io = m->io;
    uint8_t *s = scratch;
    uint32_t w = (uint32_t)(scratch_len / 6) & ~7u;
    int rc;

    if (w < 64)
        return MP4_E_SPACE;
    memset(it, 0, sizeof(*it));
    it->m = m;

    if ((rc = read_u32(io, m->stsz.pos + 4, &it->fixed_size)) < 0)
        return rc;
    cur_init(&it->sz, io, m->stsz.pos + 12, m->stsz.end, s, w);
    if ((rc = read_u32(io, m->stco.pos + 4, &it->chunks)) < 0)
        return rc;
    cur_init(&it->co, io, m->stco.pos + 8, m->stco.end, s + w, w);
    if ((rc = read_u32(io, m->stsc.pos + 4, &it->sc_left)) < 0)
        return rc;
    cur_init(&it->sc, io, m->stsc.pos + 8, m->stsc.end, s + 2 * w, w);
    if ((rc = read_u32(io, m->stts.pos + 4, &it->ts_left)) < 0)
        return rc;
    cur_init(&it->ts, io, m->stts.pos + 8, m->stts.end, s + 3 * w, w);

    if (m->ctts.end)
    {
        uint8_t ver;
        if ((rc = read_exact(io, m->ctts.pos, &ver, 1)) < 0 ||
            (rc = read_u32(io, m->ctts.pos + 4, &it->ct_left)) < 0)
            return rc;
        it->ct_signed = ver != 0;
        cur_init(&it->ct, io, m->ctts.pos + 8, m->ctts.end, s + 4 * w, w);
    }

    it->all_key = !m->stss.end;
    if (!it->all_key)
    {
        if ((rc = read_u32(io, m->stss.pos + 4, &it->ss_left)) < 0)
            return rc;
        cur_init(&it->ss, io, m->stss.pos + 8, m->stss.end, s + 5 * w, w);
        if (it->ss_left)
        {
            if ((rc = cur_u32(&it->ss, &it->ss_next)) < 0)
                return rc;
            it->ss_left--;
        }
    }

    /* stsc: the current entry, and the one after it (pending) */
    if (it->sc_left == 0)
        return m->nsamples ? MP4_E_TABLE : MP4_OK;
    if ((rc = sc_read(it, &it->cur_first, &it->cur_per)) < 0)
        return rc;
    if (it->sc_left && (rc = sc_read(it, &it->nxt_first, &it->nxt_per)) < 0)
        return rc;
    return MP4_OK;
}

/* Moves to the next chunk; MP4_E_TABLE when the chunks run out.  An stsc
   entry covers its first chunk up to the next entry's first chunk; chunks
   before the first entry's first chunk hold no samples. */
static int next_chunk(struct mp4_iter *it)
{
    int rc;

    it->chunk++;
    if (it->chunk > it->chunks)
        return MP4_E_TABLE;
    if (it->m->co64)
        rc = cur_u64(&it->co, &it->chunk_pos);
    else
    {
        uint32_t v;
        rc = cur_u32(&it->co, &v);
        it->chunk_pos = v;
    }
    if (rc < 0)
        return rc;

    while (it->nxt_first && it->chunk >= it->nxt_first)
    {
        it->cur_first = it->nxt_first;
        it->cur_per = it->nxt_per;
        if (it->sc_left)
        {
            if ((rc = sc_read(it, &it->nxt_first, &it->nxt_per)) < 0)
                return rc;
        }
        else
            it->nxt_first = 0;
    }
    it->chunk_left = it->chunk >= it->cur_first ? it->cur_per : 0;
    return MP4_OK;
}

int mp4_iter_next(struct mp4_iter *it, struct mp4_sample *s)
{
    const struct mp4_movie *m = it->m;
    uint32_t size;
    uint64_t dts;
    int64_t cts = 0, pts, num;
    int rc;

    if (it->index >= m->nsamples)
        return 0;

    if (it->fixed_size)
        size = it->fixed_size;
    else if ((rc = cur_u32(&it->sz, &size)) < 0)
        return rc;

    while (it->chunk_left == 0)
        if ((rc = next_chunk(it)) < 0)
            return rc;
    s->offset = it->chunk_pos;
    it->chunk_pos += size;
    it->chunk_left--;

    while (it->ts_run == 0)
    {
        if (it->ts_left == 0)
            return MP4_E_TABLE;
        if ((rc = cur_u32(&it->ts, &it->ts_run)) < 0 ||
            (rc = cur_u32(&it->ts, &it->ts_delta)) < 0)
            return rc;
        it->ts_left--;
    }
    dts = it->dts;
    it->dts += it->ts_delta;
    it->ts_run--;

    if (m->ctts.end)
    {
        while (it->ct_run == 0 && it->ct_left)
        {
            uint32_t v;
            if ((rc = cur_u32(&it->ct, &it->ct_run)) < 0 ||
                (rc = cur_u32(&it->ct, &v)) < 0)
                return rc;
            it->ct_off = it->ct_signed ? (int64_t)(int32_t)v : (int64_t)v;
            it->ct_left--;
        }
        if (it->ct_run)
        {
            cts = it->ct_off;
            it->ct_run--;
        }
    }

    if (it->all_key)
        s->key = true;
    else
    {
        while (it->ss_next && it->ss_next < it->index + 1)
        {
            if (it->ss_left)
            {
                if ((rc = cur_u32(&it->ss, &it->ss_next)) < 0)
                    return rc;
                it->ss_left--;
            }
            else
                it->ss_next = 0;
        }
        s->key = it->ss_next == it->index + 1;
    }

    /* PTS in us, floored like the reference's // */
    pts = (int64_t)dts + cts - m->shift;
    num = pts * 1000000;
    s->pts_us = num / (int64_t)m->timescale;
    if (num % (int64_t)m->timescale && num < 0)
        s->pts_us--;
    s->size = size;
    it->index++;
    return 1;
}

/* --- Annex B ------------------------------------------------------------ */

long mp4_params(const struct mp4_movie *m, uint8_t *out, size_t cap)
{
    uint32_t len = (uint32_t)(m->avcc.end - m->avcc.pos);
    uint8_t *src;
    size_t w = 0;
    int rc;

    if (cap < 2 * (size_t)len)
        return MP4_E_SPACE;
    src = out + cap - len;
    if ((rc = read_exact(m->io, m->avcc.pos, src, len)) < 0)
        return rc;
    if ((rc = avcc_sets(src, len, out, &w)) < 0)
        return rc;
    return (long)w;
}

long mp4_annexb(const uint8_t *in, size_t len, int nal_length,
                uint8_t *out, size_t cap)
{
    size_t p = 0, w = 0;
    int ls = nal_length;

    if (ls < 1 || ls > 4)
        return MP4_E_FORMAT;
    while (p + ls <= len)
    {
        uint32_t n = 0;
        int i;
        for (i = 0; i < ls; i++)
            n = n << 8 | in[p + i];
        p += ls;
        if (n > len - p)
            return MP4_E_FORMAT;            /* NAL runs past the sample */
        if (w + 4 + n > cap)
            return MP4_E_SPACE;
        out[w] = 0, out[w + 1] = 0, out[w + 2] = 0, out[w + 3] = 1;
        memmove(out + w + 4, in + p, n);
        w += 4 + n;
        p += n;
    }
    if (p != len)
        return MP4_E_FORMAT;                /* trailing bytes */
    return (long)w;
}
