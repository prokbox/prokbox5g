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
 * iPod UI shell: the Apple fallback's pure half (#137; afb_core.h).
 * The database layout is the one our reference writer
 * (tools/itunesdb/mkitdb.py) proved on Apple firmware 1.3 [silicon,
 * 2026-11-05]; tests/applefb holds the two byte for byte.
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
#include <stdio.h>
#include "afb_core.h"

/* -- names and kinds ----------------------------------------------------- */
static int lower(int c)
{
    return (c >= 'A' && c <= 'Z') ? c + 32 : c;
}

/* by extension (lower case, with the dot), the kind */
static const struct { char ext[6]; uint8_t kind; } exts[] = {
    { ".mp3", AFB_K_MP3 }, { ".m4a", AFB_K_M4A }, { ".aac", AFB_K_M4A },
    { ".m4b", AFB_K_M4B }, { ".wav", AFB_K_WAV }, { ".aif", AFB_K_AIF },
    { ".aiff", AFB_K_AIF },
};

enum afb_kind afb_kind_of(const char *path)
{
    const char *base = strrchr(path, '/');
    size_t n;

    base = base ? base + 1 : path;
    if (base[0] == '.')                 /* "._x.mp3": a Mac's sidecar */
        return AFB_K_NONE;
    n = strlen(base);
    for (unsigned i = 0; i < sizeof(exts) / sizeof(exts[0]); i++)
    {
        size_t m = strlen(exts[i].ext), j = 0;

        if (n < m)
            continue;
        while (j < m && lower((unsigned char)base[n - m + j]) == exts[i].ext[j])
            j++;
        if (j == m)
            return exts[i].kind;
    }
    return AFB_K_NONE;
}

/* per kind: the type word as the database stores it (a reversed
 * four-letter code, little-endian: " 3PM"), the link's extension, the
 * kind's text */
static const struct { char ftype[5]; char ext[4]; const char *text; }
    kinds[AFB_K__N] = {
    { " 3PM", "MP3", "MPEG audio file" }, { " 3PM", "MP3", "MPEG audio file" },
    { " A4M", "M4A", "AAC audio file" },  { " B4M", "M4B", "AAC audio file" },
    { " VAW", "WAV", "WAV audio file" },  { "FFIA", "AIF", "AIFF audio file" },
};

uint32_t afb_ftype(enum afb_kind k)
{
    const uint8_t *f = (const uint8_t *)kinds[k < AFB_K__N ? k : 0].ftype;

    return f[0] | f[1] << 8 | f[2] << 16 | (uint32_t)f[3] << 24;
}

const char *afb_kind_text(enum afb_kind k)
{
    return kinds[k < AFB_K__N ? k : 0].text;
}

uint32_t afb_media(enum afb_kind k)
{
    return k == AFB_K_M4B ? 8 : 1;
}

void afb_link_name(char *buf, size_t len, uint32_t no, enum afb_kind k)
{
    snprintf(buf, len, "P%07lu.%.3s", (unsigned long)(no % 10000000u),
             kinds[k < AFB_K__N ? k : 0].ext);
}

bool afb_location(char *buf, size_t len, const char *path)
{
    size_t n = strlen(path);

    if (n + 1 > len || path[0] != '/')
        return false;
    for (size_t i = 0; i <= n; i++)
        buf[i] = path[i] == '/' ? ':' : path[i];
    return true;
}

static bool under(const char *path, const char *dir)
{
    size_t n = strlen(dir);

    for (size_t i = 0; i < n; i++)
        if (lower((unsigned char)path[i]) != lower((unsigned char)dir[i]))
            return false;
    return path[n] == '/' || path[n] == '\0';
}

bool afb_in_link_dir(const char *path)
{
    return under(path, AFB_LINK_DIR);
}

bool afb_in_music_dir(const char *path)
{
    return under(path, AFB_MUSIC_DIR) && path[strlen(AFB_MUSIC_DIR)] == '/';
}

uint32_t afb_hash(uint32_t h, const void *p, size_t n)
{
    const uint8_t *b = p;

    while (n--)
    {
        h ^= *b++;
        h *= 16777619u;
    }
    return h;
}

/* -- the writer ------------------------------------------------------------ */
static void le16(uint8_t *p, uint32_t v)
{
    p[0] = v; p[1] = v >> 8;
}

static void le32(uint8_t *p, uint32_t v)
{
    p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24;
}

static void le64(uint8_t *p, uint64_t v)
{
    le32(p, (uint32_t)v);
    le32(p + 4, (uint32_t)(v >> 32));
}

static uint32_t rd32(const uint8_t *p)
{
    return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16
           | (uint32_t)p[3] << 24;
}

static void out(struct itdb_w *w, const void *p, uint32_t n)
{
    if (w->err)
        return;
    if (w->o->write(w->o->ctx, p, n) < 0)
        w->err = ITDB_E_IO;
    else
        w->pos += n;
}

static void patch32(struct itdb_w *w, uint32_t off, uint32_t v)
{
    uint8_t b[4];

    le32(b, v);
    if (!w->err && w->o->patch(w->o->ctx, off, b, 4) < 0)
        w->err = ITDB_E_IO;
}

static void head(uint8_t *h, const char *tag, uint32_t hl, uint32_t tl)
{
    memcpy(h, tag, 4);
    le32(h + 4, hl);
    le32(h + 8, tl);
}

/* one UTF-8 code point; bad bytes read as U+FFFD */
static uint32_t utf8_next(const char **sp)
{
    const uint8_t *s = (const uint8_t *)*sp;
    uint32_t c = *s++;
    int more = 0;

    if (c >= 0xf0 && c < 0xf8)
        c &= 0x07, more = 3;
    else if (c >= 0xe0)
        c &= 0x0f, more = 2;
    else if (c >= 0xc0)
        c &= 0x1f, more = 1;
    else if (c >= 0x80)
        c = 0xfffd;
    while (more--)
    {
        if ((*s & 0xc0) != 0x80)
        {
            c = 0xfffd;
            break;
        }
        c = (c << 6) | (*s++ & 0x3f);
    }
    *sp = (const char *)s;
    return c;
}

static uint32_t utf16_units(const char *s)
{
    uint32_t n = 0;

    while (*s)
        n += utf8_next(&s) >= 0x10000 ? 2 : 1;
    return n;
}

#define MHOD_STR_HDR (0x18 + 16)

static uint32_t mhod_str_len(const char *s)
{
    return MHOD_STR_HDR + 2 * utf16_units(s);
}

static void mhod_str(struct itdb_w *w, uint32_t type, const char *s)
{
    uint8_t h[MHOD_STR_HDR], u[64];
    uint32_t units = utf16_units(s), k = 0;

    memset(h, 0, sizeof(h));
    head(h, "mhod", 0x18, MHOD_STR_HDR + 2 * units);
    le32(h + 12, type);
    le32(h + 0x18, 1);                  /* UTF-16 */
    le32(h + 0x1c, 2 * units);
    out(w, h, sizeof(h));
    while (*s)
    {
        uint32_t c = utf8_next(&s);

        if (c >= 0x10000)
        {
            c -= 0x10000;
            le16(u + k, 0xd800 | (c >> 10));
            le16(u + k + 2, 0xdc00 | (c & 0x3ff));
            k += 4;
        }
        else
        {
            le16(u + k, c);
            k += 2;
        }
        if (k > sizeof(u) - 4)
        {
            out(w, u, k);
            k = 0;
        }
    }
    if (k)
        out(w, u, k);
}

#define MHBD_HL 0x68
#define MHSD_HL 0x60
#define LIST_HL 0x5c
#define MHIT_HL 0x184
#define MHYP_HL 0x6c
#define MHIP_HL 0x4c
#define MHOD_POS 0x2c
#define MHIP_TL (MHIP_HL + MHOD_POS)

static bool has(const char *s)
{
    return s && *s;
}

int itdb_begin(struct itdb_w *w, const struct afb_out *o, uint32_t ts_mac,
               uint64_t db_id, uint64_t pl_id, uint32_t first_id)
{
    uint8_t h[MHBD_HL];

    memset(w, 0, sizeof(*w));
    w->o = o;
    w->ts = ts_mac;
    w->pl_id = pl_id;
    w->first_id = first_id;
    memset(h, 0, sizeof(h));
    head(h, "mhbd", MHBD_HL, 0);        /* total: patched at the end */
    le32(h + 12, 1);
    le32(h + 16, ITDB_VERSION);
    le32(h + 20, 2);                    /* two sets */
    le64(h + 24, db_id);
    le16(h + 32, 2);
    out(w, h, MHBD_HL);
    memset(h, 0, sizeof(h));
    head(h, "mhsd", MHSD_HL, 0);        /* patched */
    le32(h + 12, 1);                    /* the tracks */
    out(w, h, MHSD_HL);
    memset(h, 0, sizeof(h));
    head(h, "mhlt", LIST_HL, 0);        /* count: patched */
    out(w, h, LIST_HL);
    return w->err;
}

int itdb_add(struct itdb_w *w, const struct itdb_track *t)
{
    uint8_t h[MHIT_HL];
    uint32_t nmhod = 3, body;
    enum afb_kind k = t->kind;

    body = mhod_str_len(t->title) + mhod_str_len(t->location)
           + mhod_str_len(afb_kind_text(k));
    if (has(t->album))
        body += mhod_str_len(t->album), nmhod++;
    if (has(t->artist))
        body += mhod_str_len(t->artist), nmhod++;
    if (has(t->genre))
        body += mhod_str_len(t->genre), nmhod++;
    memset(h, 0, sizeof(h));
    head(h, "mhit", MHIT_HL, MHIT_HL + body);
    le32(h + 12, nmhod);
    le32(h + 16, w->first_id + w->n);
    le32(h + 20, 1);                    /* visible */
    le32(h + 24, afb_ftype(k));
    if (k == AFB_K_MP3 || k == AFB_K_NONE)
    {
        h[28] = t->vbr ? 1 : 0;
        h[29] = 1;
    }
    le32(h + 32, w->ts);                /* modified */
    le32(h + 36, t->size);
    le32(h + 40, t->ms);
    le32(h + 44, t->track);
    le32(h + 52, t->year);
    le32(h + 56, t->kbps);
    le32(h + 60, t->sr << 16);          /* 16.16 */
    le32(h + 104, w->ts);               /* added */
    le64(h + 112, t->dbid);
    le32(h + 0xd0, afb_media(k));
    out(w, h, MHIT_HL);
    mhod_str(w, 1, t->title);
    mhod_str(w, 2, t->location);
    mhod_str(w, 6, afb_kind_text(k));
    if (has(t->album))
        mhod_str(w, 3, t->album);
    if (has(t->artist))
        mhod_str(w, 4, t->artist);
    if (has(t->genre))
        mhod_str(w, 5, t->genre);
    if (!w->err)
        w->n++;
    return w->err;
}

long itdb_end(struct itdb_w *w, const char *plname)
{
    uint8_t h[MHIT_HL > MHYP_HL ? MHYP_HL : MHIT_HL];
    uint32_t sd1 = w->pos - MHBD_HL;
    uint32_t mhyp = MHYP_HL + mhod_str_len(plname) + w->n * MHIP_TL;
    uint32_t sd2 = MHSD_HL + LIST_HL + mhyp;

    memset(h, 0, MHSD_HL);
    head(h, "mhsd", MHSD_HL, sd2);
    le32(h + 12, 2);                    /* the playlists */
    out(w, h, MHSD_HL);
    memset(h, 0, LIST_HL);
    head(h, "mhlp", LIST_HL, 1);
    out(w, h, LIST_HL);
    memset(h, 0, MHYP_HL);
    head(h, "mhyp", MHYP_HL, mhyp);
    le32(h + 12, 1);                    /* one string: the name */
    le32(h + 16, w->n);
    le32(h + 20, 1);                    /* the master playlist */
    le32(h + 24, w->ts);
    le64(h + 28, w->pl_id);
    out(w, h, MHYP_HL);
    mhod_str(w, 1, plname);
    for (uint32_t i = 0; i < w->n && !w->err; i++)
    {
        uint8_t ip[MHIP_TL];

        memset(ip, 0, sizeof(ip));
        head(ip, "mhip", MHIP_HL, MHIP_TL);
        le32(ip + 12, 1);
        le32(ip + 24, w->first_id + i);
        le32(ip + 28, w->ts);
        head(ip + MHIP_HL, "mhod", 0x18, MHOD_POS);
        le32(ip + MHIP_HL + 12, 100);   /* the position */
        le32(ip + MHIP_HL + 24, i + 1);
        out(w, ip, sizeof(ip));
    }
    patch32(w, 8, w->pos);                      /* mhbd total */
    patch32(w, MHBD_HL + 8, sd1);               /* mhsd 1 total */
    patch32(w, MHBD_HL + MHSD_HL + 8, w->n);    /* mhlt count */
    return w->err ? w->err : (long)w->pos;
}

/* -- the verifier -------------------------------------------------------- */
static bool tag_is(const uint8_t *b, const char *t)
{
    return !memcmp(b, t, 4);
}

int itdb_verify(const struct afb_in *in, uint32_t len, bool strict,
                struct itdb_stats *st)
{
    uint8_t b[32];
    uint32_t ends[8], off, lists_t = 0, lists_p = 0, want_items = 0;
    uint32_t imin = 0xffffffffu, imax = 0;
    int d = 0;

    memset(st, 0, sizeof(*st));
    st->min_id = 0xffffffffu;
    if (len < MHBD_HL || in->read(in->ctx, 0, b, 24) < 0)
        return len < MHBD_HL ? ITDB_E_LEN : ITDB_E_IO;
    if (!tag_is(b, "mhbd"))
        return ITDB_E_MAGIC;
    if (rd32(b + 8) != len || rd32(b + 4) < 24 || rd32(b + 4) > len)
        return ITDB_E_LEN;
    st->version = rd32(b + 16);
    if (strict && st->version != ITDB_VERSION)
        return ITDB_E_VERSION;
    off = rd32(b + 4);
    ends[0] = len;
    while (1)
    {
        uint32_t hl, tl;

        if (off == ends[d])
        {
            if (d == 0)
                break;
            d--;
            continue;
        }
        if (off > ends[d] || ends[d] - off < 12)
            return ITDB_E_NEST;
        if (in->read(in->ctx, off, b, ends[d] - off < 32 ? 12 : 32) < 0)
            return ITDB_E_IO;
        hl = rd32(b + 4);
        tl = rd32(b + 8);
        if (tag_is(b, "mhlt") || tag_is(b, "mhlp"))
        {
            /* a list: its header only; the children follow as siblings */
            if (hl < 12 || hl > ends[d] - off)
                return ITDB_E_NEST;
            if (tag_is(b, "mhlt"))
                lists_t += tl;
            else
                lists_p += tl;
            off += hl;
            continue;
        }
        if (!tag_is(b, "mhsd") && !tag_is(b, "mhit") && !tag_is(b, "mhod")
            && !tag_is(b, "mhyp") && !tag_is(b, "mhip"))
            return ITDB_E_TAG;
        if (hl < 12 || tl < hl || tl > ends[d] - off)
            return ITDB_E_NEST;
        if (tag_is(b, "mhit"))
        {
            uint32_t id;

            if (ends[d] - off < 32 || hl < 20)
                return ITDB_E_NEST;
            id = rd32(b + 16);
            st->tracks++;
            if (id < st->min_id)
                st->min_id = id;
            if (id > st->max_id)
                st->max_id = id;
        }
        else if (tag_is(b, "mhyp"))
        {
            if (hl < 20)
                return ITDB_E_NEST;
            st->playlists++;
            want_items += rd32(b + 16);
        }
        else if (tag_is(b, "mhip"))
        {
            uint32_t id;

            if (hl < 28 || ends[d] - off < 32)
                return ITDB_E_NEST;
            id = rd32(b + 24);
            st->items++;
            if (id < imin)
                imin = id;
            if (id > imax)
                imax = id;
        }
        if (tag_is(b, "mhod"))
        {
            off += tl;
            continue;
        }
        if (d + 1 >= (int)(sizeof(ends) / sizeof(ends[0])))
            return ITDB_E_NEST;
        ends[++d] = off + tl;
        off += hl;
    }
    if (lists_t != st->tracks || lists_p != st->playlists
        || want_items != st->items)
        return ITDB_E_COUNT;
    /* our writer's ids run first_id.. without gaps, so a range check is
     * exact for our own files: every item names a track */
    if (st->items && (!st->tracks || imin < st->min_id || imax > st->max_id
                      || st->max_id - st->min_id + 1 != st->tracks))
        return ITDB_E_ID;
    return ITDB_OK;
}

/* -- the verify rule --------------------------------------------------------- */
enum afb_verdict afb_classify(const struct afb_facts *f)
{
    bool same = f->orig_exists && f->orig_fst == f->rec_fst
                && f->orig_size == f->rec_size;

    if (!f->orig_exists)
        return AFB_V_ORPHAN;
    /* a disk checker cut the original (first cluster 0, size 0) and left
     * the data under the link */
    if (f->orig_fst == 0 && f->orig_size == 0 && f->link_exists
        && f->link_fst == f->rec_fst && f->link_size == f->rec_size
        && f->rec_fst != 0)
        return AFB_V_EMPTIED;
    if (!same)
        return f->orig_healthy ? AFB_V_STALE : AFB_V_DAMAGED;
    if (!f->orig_healthy)
        return (f->runs_known && f->runs_free) ? AFB_V_FREED : AFB_V_DAMAGED;
    if (f->runs_known && !f->orig_eq_runs)
        return AFB_V_STALE;
    if (!f->link_exists)
        return AFB_V_LINK_GONE;
    return AFB_V_OK;
}

const char *afb_verdict_name(enum afb_verdict v)
{
    static const char *const n[AFB_V__N] = { "ok", "orphan", "emptied",
        "stale", "freed", "damaged", "linkgone" };

    return v < AFB_V__N ? n[v] : "?";
}

/* -- the park cycle ------------------------------------------------------- */
unsigned afb_plan(const struct afb_situation *s)
{
    if (!s->on)
        return (s->links_present ? AFB_DO_PARK : 0)
               | ((s->links_present || s->state != AFB_ST_NONE)
                  ? AFB_DO_TEARDOWN : 0);
    if (s->state != AFB_ST_LIVE)
        /* first run, or an unpark a power cut stopped: from scratch */
        return (s->links_present ? AFB_DO_PARK : 0) | AFB_DO_UNPARK;
    if (!s->links_present)
        return AFB_DO_UNPARK;           /* parked (a USB session, a cut) */
    /* links live: a session that kept them (the park failed, or none was
     * exposed) is checked and made again; at boot only when the volume
     * changed since they were made */
    if (s->when == AFB_AT_USB || !s->fp_same)
        return AFB_DO_VERIFY | AFB_DO_PARK | AFB_DO_UNPARK;
    return 0;
}

/* -- records ---------------------------------------------------------------- */
#define REC_HDR 16

uint32_t afb_rec_bytes(const struct afb_rec *r)
{
    uint32_t nr = r->nruns == 0xff ? 0 : r->nruns;

    return (REC_HDR + 8 * nr + r->pathlen + 3) & ~3u;
}

void afb_rec_put(uint8_t *buf, const struct afb_rec *r, const char *path)
{
    uint32_t nr = r->nruns == 0xff ? 0 : r->nruns, n = afb_rec_bytes(r);

    memset(buf, 0, n);
    le32(buf, r->fst);
    le32(buf + 4, r->size);
    le32(buf + 8, r->linkno);
    le16(buf + 12, r->pathlen);
    buf[14] = r->nruns;
    buf[15] = r->kind;
    for (uint32_t i = 0; i < 2 * nr; i++)
        le32(buf + REC_HDR + 4 * i, r->runs[i]);
    memcpy(buf + REC_HDR + 8 * nr, path, r->pathlen);
}

uint32_t afb_rec_get(const uint8_t *buf, uint32_t avail, struct afb_rec *r,
                     const char **path, uint32_t *need)
{
    uint32_t nr, n;

    *need = REC_HDR;
    if (avail < REC_HDR)
        return 0;
    r->fst = rd32(buf);
    r->size = rd32(buf + 4);
    r->linkno = rd32(buf + 8);
    r->pathlen = buf[12] | buf[13] << 8;
    r->nruns = buf[14];
    r->kind = buf[15];
    if ((r->nruns > AFB_MAXRUNS && r->nruns != 0xff) || !r->pathlen
        || r->pathlen >= AFB_LOC_MAX || r->kind >= AFB_K__N)
    {
        *need = 0;                      /* bad, not short */
        return 0;
    }
    nr = r->nruns == 0xff ? 0 : r->nruns;
    n = afb_rec_bytes(r);
    *need = n;
    if (avail < n)
        return 0;
    for (uint32_t i = 0; i < 2 * nr; i++)
        r->runs[i] = rd32(buf + REC_HDR + 4 * i);
    *path = (const char *)buf + REC_HDR + 8 * nr;
    return n;
}
