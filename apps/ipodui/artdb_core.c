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

#include <string.h>
#include "artdb_core.h"

/* Layout notes (public documentation only; docs/design/
 * ipod-control-support.md §5.7 names the sources). Every chunk starts
 * with a four-letter tag, its header length and (except the lists, whose
 * third word is a count) its total length. Lists run to the end of their
 * parent. The reader checks that every chunk nests inside its parent and
 * inside the file; nothing is trusted past a failed check. */

/* -- little-endian helpers ---------------------------------------------- */
static uint32_t rd16(const uint8_t *p)
{
    return p[0] | (uint32_t)p[1] << 8;
}

static uint32_t rd32(const uint8_t *p)
{
    return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16
           | (uint32_t)p[3] << 24;
}

static uint64_t rd64(const uint8_t *p)
{
    return rd32(p) | (uint64_t)rd32(p + 4) << 32;
}

static bool tag_is(const uint8_t *p, const char *t)
{
    return !memcmp(p, t, 4);
}

/* -- names ---------------------------------------------------------------- */
/* "F<digits>_<digits>.ithmb" and nothing else */
static bool name_ok(const char *s)
{
    int d;

    if (*s++ != 'F')
        return false;
    for (d = 0; *s >= '0' && *s <= '9'; s++)
        d++;
    if (!d || d > 6 || *s++ != '_')
        return false;
    for (d = 0; *s >= '0' && *s <= '9'; s++)
        d++;
    return d && d <= 6 && !strcmp(s, ".ithmb");
}

bool artdb_img_geom(const struct artdb_img *im, uint16_t *slot_w,
                    uint16_t *slot_h, uint16_t *stride)
{
    uint32_t sw, sh, row;

    if (!im->w || !im->h || im->pad_v < 0 || im->pad_h < 0)
        return false;
    sw = (uint32_t)im->w + (uint32_t)im->pad_h;     /* <= 2 * 65535 */
    sh = (uint32_t)im->h + (uint32_t)im->pad_v;
    if (sw > ARTDB_MAX_SIDE || sh > ARTDB_MAX_SIDE)
        return false;
    row = 2 * sh;               /* size / row: the stride in pixels */
    if (im->size % row || im->size / row < sw
        || im->size / row > ARTDB_MAX_SIDE + 16)
        return false;
    if (slot_w)
        *slot_w = (uint16_t)sw;
    if (slot_h)
        *slot_h = (uint16_t)sh;
    if (stride)
        *stride = (uint16_t)(im->size / row);
    return true;
}

bool artdb_img_ok(const struct artdb_img *im)
{
    return artdb_img_geom(im, NULL, NULL, NULL) && name_ok(im->file);
}

/* -- reading ---------------------------------------------------------------- */
struct rd {
    const struct afb_in *in;
    uint32_t len;
};

static int get(const struct rd *r, uint32_t off, void *p, uint32_t n)
{
    if (off > r->len || n > r->len - off)
        return ARTDB_E_LEN;
    return r->in->read(r->in->ctx, off, p, n) < 0 ? ARTDB_E_IO : 0;
}

/* A chunk at off inside [off, end): its tag, header length and total
 * length (or, for a list, 'third' is its count and the chunk runs to
 * end). want: the tag, or NULL for any. */
struct chunk {
    uint32_t off, hl, tl, third;
    uint8_t tag[4];
};

static int chunk_at(const struct rd *r, uint32_t off, uint32_t end,
                    bool is_list, struct chunk *c)
{
    uint8_t h[12];
    int rc;

    if (off > end || end - off < 12)
        return ARTDB_E_NEST;
    if ((rc = get(r, off, h, 12)) < 0)
        return rc;
    memcpy(c->tag, h, 4);
    c->off = off;
    c->hl = rd32(h + 4);
    c->third = rd32(h + 8);
    c->tl = is_list ? end - off : c->third;
    if (c->hl < 12 || c->tl < c->hl || c->tl > end - off)
        return ARTDB_E_NEST;
    return 0;
}

/* a string mhod's text (type 3 in the image database, any string type in
 * the song database): UTF-16LE or UTF-8 into UTF-8 out; colons stay */
static int read_text(const struct rd *r, uint32_t at, uint32_t end,
                     uint32_t bytes, bool utf16, char *out, size_t outlen)
{
    uint8_t b[512];
    size_t k = 0;

    if (at > end || bytes > end - at)
        return ARTDB_E_NEST;
    if (bytes > sizeof(b))
        return ARTDB_E_TAG;             /* nests, but longer than any use */
    if (get(r, at, b, bytes) < 0)
        return ARTDB_E_IO;
    if (!utf16)
    {
        if (bytes + 1 > outlen)
            return ARTDB_E_TAG;
        memcpy(out, b, bytes);
        out[bytes] = '\0';
        return memchr(out, '\0', bytes) ? ARTDB_E_TAG : 0;
    }
    if (bytes & 1)
        return ARTDB_E_TAG;
    for (uint32_t i = 0; i < bytes; i += 2)
    {
        uint32_t c = rd16(b + i);
        uint8_t u[4];
        int n;

        if (c >= 0xd800 && c < 0xdc00 && i + 3 < bytes
            && rd16(b + i + 2) >= 0xdc00 && rd16(b + i + 2) < 0xe000)
        {
            c = 0x10000 + ((c - 0xd800) << 10) + (rd16(b + i + 2) - 0xdc00);
            i += 2;
        }
        else if (c >= 0xd800 && c < 0xe000)
            c = 0xfffd;                 /* a lone surrogate */
        if (!c)
            return ARTDB_E_TAG;
        if (c < 0x80)
            u[0] = c, n = 1;
        else if (c < 0x800)
            u[0] = 0xc0 | c >> 6, u[1] = 0x80 | (c & 0x3f), n = 2;
        else if (c < 0x10000)
            u[0] = 0xe0 | c >> 12, u[1] = 0x80 | ((c >> 6) & 0x3f),
            u[2] = 0x80 | (c & 0x3f), n = 3;
        else
            u[0] = 0xf0 | c >> 18, u[1] = 0x80 | ((c >> 12) & 0x3f),
            u[2] = 0x80 | ((c >> 6) & 0x3f), u[3] = 0x80 | (c & 0x3f),
            n = 4;
        if (k + n + 1 > outlen)
            return ARTDB_E_TAG;
        memcpy(out + k, u, n);
        k += n;
    }
    out[k] = '\0';
    return 0;
}

/* One mhni (inside a container mhod): its fields and its name. < 0 a
 * structural error; 1 an image; 0 an image with no usable name */
static int read_mhni(const struct rd *r, uint32_t off, uint32_t end,
                     struct artdb_img *im)
{
    uint8_t h[0x2c];
    struct chunk c, s;
    uint32_t at;
    int rc;

    if ((rc = chunk_at(r, off, end, false, &c)) < 0)
        return rc;
    if (!tag_is(c.tag, "mhni") || c.hl < 0x24)
        return ARTDB_E_TAG;
    if ((rc = get(r, off, h, c.hl < sizeof(h) ? c.hl : sizeof(h))) < 0)
        return rc;
    im->fmt = rd32(h + 16);
    im->offset = rd32(h + 20);
    im->size = rd32(h + 24);
    im->pad_v = (int16_t)rd16(h + 28);
    im->pad_h = (int16_t)rd16(h + 30);
    im->h = rd16(h + 32);
    im->w = rd16(h + 34);
    im->file[0] = '\0';
    for (at = off + c.hl; at < off + c.tl; at += s.tl)
    {
        uint8_t m[0x24];
        char name[64];

        if ((rc = chunk_at(r, at, off + c.tl, false, &s)) < 0)
            return rc;
        if (!tag_is(s.tag, "mhod"))
            return ARTDB_E_TAG;
        if (s.hl < 16 || s.tl < s.hl + 12 || get(r, at, m, 16) < 0)
            return ARTDB_E_NEST;
        if ((rd32(m + 12) & 0xffff) != 3)
            continue;
        if (get(r, at + s.hl, m, 12) < 0)
            return ARTDB_E_IO;
        /* string length, encoding (2: UTF-16LE, else UTF-8), 0 */
        rc = read_text(r, at + s.hl + 12, at + s.tl, rd32(m), rd32(m + 4) == 2,
                       name, sizeof(name));
        if (rc == ARTDB_E_NEST)
            return rc;
        /* ":F1029_1.ithmb": one leading colon, a plain name after it */
        if (rc == 0 && name[0] == ':' && !strchr(name + 1, ':')
            && strlen(name + 1) < sizeof(im->file))
            strcpy(im->file, name + 1);
    }
    return im->file[0] ? 1 : 0;
}

int artdb_read(const struct afb_in *in, uint32_t len,
               int (*cb)(void *ctx, const struct artdb_img *im), void *ctx,
               uint32_t *bad)
{
    struct rd r = { in, len };
    struct chunk fd, sd;
    uint32_t at, nbad = 0;
    int n = 0, rc;

    if ((rc = chunk_at(&r, 0, len, false, &fd)) < 0)
        return rc;
    if (!tag_is(fd.tag, "mhfd"))
        return ARTDB_E_MAGIC;
    if (fd.tl != len)
        return ARTDB_E_LEN;
    for (at = fd.hl; at < fd.tl; at += sd.tl)
    {
        uint8_t h[16];
        struct chunk li, ii;

        if ((rc = chunk_at(&r, at, fd.tl, false, &sd)) < 0)
            return rc;
        if (!tag_is(sd.tag, "mhsd") || sd.hl < 16)
            return ARTDB_E_TAG;
        if ((rc = get(&r, at, h, 16)) < 0)
            return rc;
        if ((rd32(h + 12) & 0xffff) != 1)
            continue;                   /* the album and file lists */
        if ((rc = chunk_at(&r, at + sd.hl, at + sd.tl, true, &li)) < 0)
            return rc;
        if (!tag_is(li.tag, "mhli"))
            return ARTDB_E_TAG;
        for (uint32_t i = 0, p = li.off + li.hl; i < li.third; i++, p += ii.tl)
        {
            uint8_t ih[0x34];
            struct chunk od;
            struct artdb_img im;

            if ((rc = chunk_at(&r, p, at + sd.tl, false, &ii)) < 0)
                return rc;
            if (!tag_is(ii.tag, "mhii") || ii.hl < 0x1c)
                return ARTDB_E_TAG;
            memset(ih, 0, sizeof(ih));
            if ((rc = get(&r, p, ih, ii.hl < sizeof(ih) ? ii.hl
                                                         : sizeof(ih))) < 0)
                return rc;
            memset(&im, 0, sizeof(im));
            im.img_id = rd32(ih + 16);
            im.dbid = rd64(ih + 20);
            im.src_size = ii.hl >= 0x34 ? rd32(ih + 48) : 0;
            for (uint32_t q = p + ii.hl; q < p + ii.tl; q += od.tl)
            {
                uint8_t oh[16];

                if ((rc = chunk_at(&r, q, p + ii.tl, false, &od)) < 0)
                    return rc;
                if (!tag_is(od.tag, "mhod") || od.hl < 16)
                    return ARTDB_E_TAG;
                if ((rc = get(&r, q, oh, 16)) < 0)
                    return rc;
                if ((rd32(oh + 12) & 0xffff) != 2)
                    continue;           /* only the thumbnail containers */
                rc = read_mhni(&r, q + od.hl, q + od.tl, &im);
                if (rc < 0)
                    return rc;
                if (rc == 0 || !artdb_img_ok(&im))
                {
                    nbad++;
                    continue;
                }
                artdb_img_geom(&im, &im.slot_w, &im.slot_h, &im.stride);
                if ((rc = cb(ctx, &im)) < 0)
                    return rc;
                n++;
            }
        }
    }
    if (bad)
        *bad = nbad;
    return n;
}

int artdb_songs(const struct afb_in *in, uint32_t len,
                int (*cb)(void *ctx, const struct artdb_song *s), void *ctx)
{
    struct rd r = { in, len };
    struct chunk bd, sd;
    uint32_t at;
    int n = 0, rc;

    if ((rc = chunk_at(&r, 0, len, false, &bd)) < 0)
        return rc;
    if (!tag_is(bd.tag, "mhbd"))
        return ARTDB_E_MAGIC;
    if (bd.tl != len)
        return ARTDB_E_LEN;
    for (at = bd.hl; at < bd.tl; at += sd.tl)
    {
        uint8_t h[16];
        struct chunk lt, it;

        if ((rc = chunk_at(&r, at, bd.tl, false, &sd)) < 0)
            return rc;
        if (!tag_is(sd.tag, "mhsd") || sd.hl < 16)
            return ARTDB_E_TAG;
        if ((rc = get(&r, at, h, 16)) < 0)
            return rc;
        if (rd32(h + 12) != 1)
            continue;                   /* playlists, albums, ... */
        if ((rc = chunk_at(&r, at + sd.hl, at + sd.tl, true, &lt)) < 0)
            return rc;
        if (!tag_is(lt.tag, "mhlt"))
            return ARTDB_E_TAG;
        for (uint32_t i = 0, p = lt.off + lt.hl; i < lt.third; i++, p += it.tl)
        {
            uint8_t th[356];            /* through the +352 link */
            struct chunk od;
            struct artdb_song s;
            char path[AFB_LOC_MAX * 2];
            bool have = false;

            if ((rc = chunk_at(&r, p, at + sd.tl, false, &it)) < 0)
                return rc;
            if (!tag_is(it.tag, "mhit") || it.hl < 0x78)
                return ARTDB_E_TAG;
            memset(th, 0, sizeof(th));
            if ((rc = get(&r, p, th, it.hl < sizeof(th) ? it.hl
                                                         : sizeof(th))) < 0)
                return rc;
            memset(&s, 0, sizeof(s));
            s.dbid = rd64(th + 112);
            if (it.hl >= 126)
                s.art_count = rd16(th + 124);
            if (it.hl >= 165)
                s.has_art = th[164];
            if (it.hl >= 0x184)
                s.art_id = rd32(th + 352);
            for (uint32_t q = p + it.hl; q < p + it.tl; q += od.tl)
            {
                uint8_t m[0x28];

                if ((rc = chunk_at(&r, q, p + it.tl, false, &od)) < 0)
                    return rc;
                if (!tag_is(od.tag, "mhod"))
                    return ARTDB_E_TAG;
                if (od.tl < 0x28 || (rc = get(&r, q, m, 0x28)) < 0)
                {
                    if (rc < 0)
                        return rc;
                    continue;           /* too short to be a string */
                }
                if (rd32(m + 12) != 2 || have)
                    continue;
                /* +24 position (2: UTF-8 on some writers), +28 bytes */
                rc = read_text(&r, q + 0x28, q + od.tl, rd32(m + 28),
                               rd32(m + 24) != 2, path, sizeof(path));
                if (rc == ARTDB_E_NEST)
                    return rc;
                if (rc == 0 && path[0] == ':')
                {
                    for (char *c = path; *c; c++)
                        if (*c == ':')
                            *c = '/';
                    have = true;
                }
            }
            if (!have)
                continue;
            s.path = path;
            if ((rc = cb(ctx, &s)) < 0)
                return rc;
            n++;
        }
    }
    return n;
}
