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

/* Test-only writers (fixwrite.h). Plain stdio; lengths and counts are
 * patched with fseek at the end, as the documented layout needs them. */
#include <string.h>
#include "fixwrite.h"

#define MHFD_HL 0x84
#define MHSD_HL 0x60
#define LIST_HL 0x5c
#define MHII_HL 0x98
#define MHOD_HL 0x18
#define MHNI_HL 0x4c
#define MHIF_HL 0x7c
#define MHBD_HL 0x68

static void le16(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; }
static void le32(uint8_t *p, uint32_t v)
{
    p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24;
}
static void le64(uint8_t *p, uint64_t v)
{
    le32(p, (uint32_t)v);
    le32(p + 4, (uint32_t)(v >> 32));
}

static void head(uint8_t *h, const char *tag, uint32_t hl, uint32_t third)
{
    memcpy(h, tag, 4);
    le32(h + 4, hl);
    le32(h + 8, third);
}

static void put(FILE *f, const void *p, size_t n)
{
    fwrite(p, 1, n, f);
}

static void patch32(FILE *f, long off, uint32_t v)
{
    uint8_t b[4];
    long end = ftell(f);

    le32(b, v);
    fseek(f, off, SEEK_SET);
    fwrite(b, 1, 4, f);
    fseek(f, end, SEEK_SET);
}

/* UTF-8 -> UTF-16LE units (surrogate pairs above U+FFFF) */
static size_t to_utf16(const char *s, uint8_t *out, size_t cap)
{
    const uint8_t *p = (const uint8_t *)s;
    size_t k = 0;

    while (*p && k + 4 <= cap)
    {
        uint32_t c = *p++;
        int more = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : c >= 0xc0 ? 1 : 0;

        c &= more == 3 ? 0x07 : more == 2 ? 0x0f : more == 1 ? 0x1f : 0x7f;
        while (more-- && *p)
            c = c << 6 | (*p++ & 0x3f);
        if (c >= 0x10000)
        {
            c -= 0x10000;
            le16(out + k, 0xd800 | c >> 10);
            le16(out + k + 2, 0xdc00 | (c & 0x3ff));
            k += 4;
        }
        else
        {
            le16(out + k, c);
            k += 2;
        }
    }
    return k;
}

/* -- the cover database -------------------------------------------------- */
static size_t name_of(char *b, size_t n, uint32_t id)
{
    return (size_t)snprintf(b, n, ":F%lu_1.ithmb", (unsigned long)id);
}

void fx_art_begin(struct fx_art *w, FILE *f, const struct fx_fmt *fmts,
                  int nfmt, uint32_t first_id)
{
    uint8_t h[MHFD_HL];

    memset(w, 0, sizeof(*w));
    w->f = f;
    w->next_id = first_id;
    w->nfmt = nfmt;
    memcpy(w->fmt, fmts, nfmt * sizeof(*fmts));
    memset(h, 0, sizeof(h));
    head(h, "mhfd", MHFD_HL, 0);
    le32(h + 16, 2);
    le32(h + 20, 3);
    le32(h + 28, first_id);
    le32(h + 48, 2);
    put(f, h, MHFD_HL);
    memset(h, 0, sizeof(h));
    w->sd1_at = ftell(f);
    head(h, "mhsd", MHSD_HL, 0);
    le16(h + 12, 1);
    put(f, h, MHSD_HL);
    memset(h, 0, sizeof(h));
    w->list_at = ftell(f);
    head(h, "mhli", LIST_HL, 0);
    put(f, h, LIST_HL);
}

uint32_t fx_art_add(struct fx_art *w, uint64_t dbid, uint32_t src_size,
                    const uint32_t *offsets, const struct fx_img_opt *opt)
{
    uint8_t h[MHII_HL], m[256], u[200];
    uint32_t body = 0, id = w->next_id + w->n, nl[FX_MAX_FMTS];
    size_t ul[FX_MAX_FMTS];
    char s[96];

    for (int i = 0; i < w->nfmt; i++)
    {
        if (opt && opt[i].name)
            snprintf(s, sizeof(s), "%s", opt[i].name);
        else
            name_of(s, sizeof(s), w->fmt[i].id);
        ul[i] = to_utf16(s, u, sizeof(u));
        nl[i] = (MHOD_HL + 12 + ul[i] + 3) & ~3u;
        body += MHOD_HL + MHNI_HL + nl[i];
    }
    if (w->label)
        body += MHOD_HL + 12 + 4;
    memset(h, 0, sizeof(h));
    head(h, "mhii", MHII_HL, MHII_HL + body);
    le32(h + 12, w->nfmt + (w->label ? 1 : 0));
    le32(h + 16, id);
    le64(h + 20, dbid);
    le32(h + 48, src_size);
    put(w->f, h, MHII_HL);
    if (w->label)
    {
        memset(m, 0, sizeof(m));
        head(m, "mhod", MHOD_HL, MHOD_HL + 12 + 4);
        le16(m + 12, 1);
        le32(m + MHOD_HL, 4);
        le32(m + MHOD_HL + 4, 1);       /* UTF-8 */
        memcpy(m + MHOD_HL + 12, "Note", 4);
        put(w->f, m, MHOD_HL + 12 + 4);
    }
    for (int i = 0; i < w->nfmt; i++)
    {
        const struct fx_fmt *f = &w->fmt[i];
        uint16_t iw = opt && opt[i].w ? opt[i].w : f->w;
        uint16_t ih = opt && opt[i].h ? opt[i].h : f->h;
        uint32_t size = opt && opt[i].size ? opt[i].size
                                           : (uint32_t)f->w * f->h * 2;

        memset(h, 0, sizeof(h));
        head(h, "mhod", MHOD_HL, MHOD_HL + MHNI_HL + nl[i]);
        le16(h + 12, 2);
        put(w->f, h, MHOD_HL);
        memset(h, 0, sizeof(h));
        head(h, "mhni", MHNI_HL, MHNI_HL + nl[i]);
        le32(h + 12, 1);
        le32(h + 16, f->id);
        le32(h + 20, offsets[i]);
        le32(h + 24, size);
        if (opt)
        {
            le16(h + 28, (uint16_t)opt[i].pad_v);
            le16(h + 30, (uint16_t)opt[i].pad_h);
        }
        le16(h + 32, ih);
        le16(h + 34, iw);
        le32(h + 40, size);
        put(w->f, h, MHNI_HL);
        if (opt && opt[i].name)
            snprintf(s, sizeof(s), "%s", opt[i].name);
        else
            name_of(s, sizeof(s), f->id);
        memset(m, 0, sizeof(m));
        head(m, "mhod", MHOD_HL, nl[i]);
        le16(m + 12, 3);
        m[15] = nl[i] - (MHOD_HL + 12 + ul[i]);
        le32(m + MHOD_HL, ul[i]);
        le32(m + MHOD_HL + 4, 2);       /* UTF-16LE */
        to_utf16(s, m + MHOD_HL + 12, sizeof(m) - MHOD_HL - 12);
        put(w->f, m, nl[i]);
    }
    w->n++;
    return id;
}

long fx_art_end(struct fx_art *w)
{
    uint8_t h[MHIF_HL];
    long end;

    patch32(w->f, w->list_at + 8, w->n);
    patch32(w->f, w->sd1_at + 8, ftell(w->f) - w->sd1_at);
    memset(h, 0, sizeof(h));
    head(h, "mhsd", MHSD_HL, MHSD_HL + LIST_HL);
    le16(h + 12, 2);
    put(w->f, h, MHSD_HL);
    memset(h, 0, sizeof(h));
    head(h, "mhla", LIST_HL, 0);
    put(w->f, h, LIST_HL);
    memset(h, 0, sizeof(h));
    head(h, "mhsd", MHSD_HL, MHSD_HL + LIST_HL + w->nfmt * MHIF_HL);
    le16(h + 12, 3);
    put(w->f, h, MHSD_HL);
    memset(h, 0, sizeof(h));
    head(h, "mhlf", LIST_HL, w->nfmt);
    put(w->f, h, LIST_HL);
    for (int i = 0; i < w->nfmt; i++)
    {
        memset(h, 0, sizeof(h));
        head(h, "mhif", MHIF_HL, MHIF_HL);
        le32(h + 16, w->fmt[i].id);
        le32(h + 20, (uint32_t)w->fmt[i].w * w->fmt[i].h * 2);
        put(w->f, h, MHIF_HL);
    }
    end = ftell(w->f);
    patch32(w->f, 8, end);
    patch32(w->f, 28, w->next_id + w->n);
    return end;
}

/* -- the song database --------------------------------------------------- */
void fx_db_begin(struct fx_db *d, FILE *f, uint32_t mhit_hl, int utf8)
{
    uint8_t h[MHBD_HL];

    memset(d, 0, sizeof(*d));
    d->f = f;
    d->mhit_hl = mhit_hl ? mhit_hl : 0x184;
    d->utf8 = utf8;
    memset(h, 0, sizeof(h));
    head(h, "mhbd", MHBD_HL, 0);
    le32(h + 12, 1);
    le32(h + 16, 0x13);
    le32(h + 20, 1);
    put(f, h, MHBD_HL);
    d->sd_at = ftell(f);
    memset(h, 0, sizeof(h));
    head(h, "mhsd", MHSD_HL, 0);
    le32(h + 12, 1);
    put(f, h, MHSD_HL);
    d->list_at = ftell(f);
    memset(h, 0, sizeof(h));
    head(h, "mhlt", LIST_HL, 0);
    put(f, h, LIST_HL);
}

void fx_db_add(struct fx_db *d, const struct fx_song *s)
{
    uint8_t h[0x200], m[0x28], u[1200];
    uint32_t ul = 0, ml = 0, hl = d->mhit_hl;

    if (s->path)
    {
        char loc[600];
        size_t i;

        for (i = 0; s->path[i] && i + 1 < sizeof(loc); i++)
            loc[i] = s->path[i] == '/' ? ':' : s->path[i];
        loc[i] = '\0';
        if (d->utf8)
        {
            ul = strlen(loc);
            memcpy(u, loc, ul);
        }
        else
            ul = to_utf16(loc, u, sizeof(u));
        ml = 0x28 + ul;
    }
    memset(h, 0, sizeof(h));
    head(h, "mhit", hl, hl + ml);
    le32(h + 12, s->path ? 1 : 0);
    le32(h + 16, 100 + d->n);
    le32(h + 20, 1);
    le64(h + 112, s->dbid);
    if (s->art_id && hl >= 356)         /* the field fits */
        le32(h + 352, s->art_id);
    if (s->art_id)
    {
        le16(h + 124, 1);
        le32(h + 128, s->art_size);
        h[164] = 1;
    }
    else if (hl > 164)
        h[164] = 2;
    put(d->f, h, hl);
    if (s->path)
    {
        memset(m, 0, sizeof(m));
        head(m, "mhod", 0x18, ml);
        le32(m + 12, 2);
        le32(m + 24, d->utf8 ? 2 : 1);
        le32(m + 28, ul);
        put(d->f, m, 0x28);
        put(d->f, u, ul);
    }
    d->n++;
}

long fx_db_end(struct fx_db *d)
{
    long end = ftell(d->f);

    patch32(d->f, d->list_at + 8, d->n);
    patch32(d->f, d->sd_at + 8, end - d->sd_at);
    patch32(d->f, 8, end);
    return end;
}

int fx_mirror;

void fx_pattern(uint8_t *p, unsigned g, unsigned w, unsigned h)
{
    for (unsigned y = 0; y < h; y++)
        for (unsigned x = 0; x < w; x++)
        {
            unsigned xx = fx_mirror ? w - 1 - x : x;
            unsigned r5 = w > 1 ? xx * 31 / (w - 1) : 0;
            unsigned g6 = h > 1 ? y * 63 / (h - 1) : 0;
            unsigned b5 = (g * 7 + 3) & 31;
            unsigned v = r5 << 11 | g6 << 5 | b5;

            *p++ = v;
            *p++ = v >> 8;
        }
}
