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
 * iPod UI shell: the vCard parser and the Contacts index (P12 W3-K,
 * docs/phase12-brief.md 3.5; the XIDX_CONTACTS kind of xindex_core.h).
 * Pure (stdint, string), host-tested in tests/xindex/test_xcontact.c.
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
#include <stdint.h>
#include <string.h>
#include "ipodui/xcontact_core.h"

/* -- small text helpers ----------------------------------------------------- */

static int up(int c)
{
    return c >= 'a' && c <= 'z' ? c - 'a' + 'A' : c;
}

/* case-insensitive: needle (upper case) inside hay[0..hl) */
static int ci_find(const char *hay, int hl, const char *needle)
{
    int nl = (int)strlen(needle), i, k;

    for (i = 0; i + nl <= hl; i++)
    {
        for (k = 0; k < nl && up((uint8_t)hay[i + k]) == needle[k]; k++)
            ;
        if (k == nl)
            return 1;
    }
    return 0;
}

static int ci_eq(const char *a, const char *b)
{
    while (*a && up((uint8_t)*a) == up((uint8_t)*b))
        a++, b++;
    return !*a && !*b;
}

static int hexv(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* valid UTF-8 (a sequence cut at the end is not) */
static int utf8_valid(const uint8_t *s, int len)
{
    int i = 0, n, k;

    while (i < len)
    {
        uint8_t c = s[i++];

        if (c < 0x80)
            continue;
        n = c >= 0xf8 ? -1 : c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : c >= 0xc2 ? 1 : -1;
        if (n < 0 || i + n > len)
            return 0;
        for (k = 0; k < n; k++)
            if ((s[i++] & 0xc0) != 0x80)
                return 0;
    }
    return 1;
}

/* Decodes the raw text of one value (a component) into dst: quoted-
 * printable bytes, the \ escapes, controls dropped, a line break kept as
 * '\n'. UTF-8 as it is; with latin1 (a CHARSET, or bytes that are not
 * UTF-8) each byte >= 0x80 becomes its character. Cut at a character. */
static int decode_pass(const char *raw, int rl, char *dst, int max, int qp,
                       int latin1)
{
    int i = 0, o = 0;

    while (i < rl)
    {
        int c = (uint8_t)raw[i++], put;

        if (c == '\\' && i < rl)
        {
            int e = (uint8_t)raw[i++];

            c = (e == 'n' || e == 'N') ? '\n' : e;
        }
        else if (qp && c == '=' && i + 1 < rl && hexv((uint8_t)raw[i]) >= 0
                 && hexv((uint8_t)raw[i + 1]) >= 0)
        {
            c = hexv((uint8_t)raw[i]) << 4 | hexv((uint8_t)raw[i + 1]);
            i += 2;
        }
        if (c == '\r')
            continue;
        if (c == '\t')
            c = ' ';
        if (c < 0x20 && c != '\n')
            continue;
        if (c == 0x7f)
            continue;
        put = latin1 && c >= 0x80 ? 2 : 1;
        if (o + put >= max)
            break;
        if (put == 2)
        {
            dst[o++] = 0xc0 | (c >> 6);
            dst[o++] = 0x80 | (c & 0x3f);
        }
        else
            dst[o++] = c;
    }
    dst[o] = '\0';
    return o;
}

static int decode(const char *raw, int rl, char *dst, int max, int qp,
                  int latin1)
{
    int n = decode_pass(raw, rl, dst, max, qp, latin1);

    if (!latin1)
    {
        /* a cut inside a sequence is a cut, not bad text */
        int cut = n;

        while (cut > 0 && ((uint8_t)dst[cut - 1] & 0xc0) == 0x80)
            cut--;
        if (cut > 0 && (uint8_t)dst[cut - 1] >= 0xc0)
            cut--;
        if (!utf8_valid((const uint8_t *)dst, n)
            && !utf8_valid((const uint8_t *)dst, cut))
            n = decode_pass(raw, rl, dst, max, qp, 1);
        else if (!utf8_valid((const uint8_t *)dst, n))
        {
            n = cut;
            dst[n] = '\0';
        }
    }
    /* trim */
    while (n > 0 && (dst[n - 1] == ' ' || dst[n - 1] == '\n'))
        dst[--n] = '\0';
    {
        int s = 0;

        while (dst[s] == ' ' || dst[s] == '\n')
            s++;
        if (s)
        {
            memmove(dst, dst + s, n - s + 1);
            n -= s;
        }
    }
    return n;
}

/* Splits a value at its unescaped ';' (in place): the raw components */
static int split(char *s, char **comp, int maxc)
{
    int n = 0;

    comp[n++] = s;
    for (; *s; s++)
    {
        if (*s == '\\' && s[1])
            s++;
        else if (*s == ';' && n < maxc)
        {
            *s = '\0';
            comp[n++] = s + 1;
        }
    }
    return n;
}

static int clen(const char *s)
{
    return (int)strlen(s);
}

static void append(char *dst, int max, const char *s)
{
    int n = clen(dst), l = clen(s);

    if (n + l >= max)
        l = max - 1 - n;
    if (l > 0)
    {
        memcpy(dst + n, s, l);
        dst[n + l] = '\0';
    }
}

/* -- the property line --------------------------------------------------- */

struct params {
    uint8_t qp, b64, latin1;
    uint8_t cell, home, work, other;
};

static void param_word(struct params *p, const char *w, int wl)
{
    if (ci_find(w, wl, "CELL") || ci_find(w, wl, "MOBILE")
        || ci_find(w, wl, "IPHONE"))
        p->cell = 1;
    if (ci_find(w, wl, "HOME"))
        p->home = 1;
    if (ci_find(w, wl, "WORK"))
        p->work = 1;
    if (ci_find(w, wl, "FAX") || ci_find(w, wl, "PAGER")
        || ci_find(w, wl, "OTHER"))
        p->other = 1;
}

/* The ";" parameters of head[from..hl) */
static void parse_params(struct params *p, char *head, int from, int hl)
{
    int i = from;

    memset(p, 0, sizeof(*p));
    while (i < hl)
    {
        int j = i, q = 0, eq = -1;
        char *key, *val;
        int kl, vl;

        while (j < hl && (head[j] != ';' || q))
        {
            if (head[j] == '"')
                q = !q;
            else if (head[j] == '=' && eq < 0 && !q)
                eq = j;
            j++;
        }
        key = head + i;
        if (eq >= 0)
        {
            kl = eq - i;
            val = head + eq + 1;
            vl = j - eq - 1;
        }
        else
        {
            kl = 0;
            val = key;                  /* 2.1: a bare word */
            vl = j - i;
        }
        if (kl == 0 || ci_find(key, kl, "TYPE"))
        {
            if (kl == 0 && (ci_find(val, vl, "QUOTED-PRINTABLE")))
                p->qp = 1;
            else if (kl == 0 && (ci_find(val, vl, "BASE64")))
                p->b64 = 1;
            else
                param_word(p, val, vl);
        }
        else if (ci_find(key, kl, "ENCODING"))
        {
            if (ci_find(val, vl, "QUOTED-PRINTABLE"))
                p->qp = 1;
            else if (ci_find(val, vl, "BASE64") || (vl == 1 && up(val[0]) == 'B'))
                p->b64 = 1;
        }
        else if (ci_find(key, kl, "CHARSET"))
        {
            if (!ci_find(val, vl, "UTF-8") && !ci_find(val, vl, "UTF8")
                && !ci_find(val, vl, "ASCII"))
                p->latin1 = 1;
        }
        i = j + 1;
    }
}

static int sub_of(const struct params *p)
{
    return p->cell ? XCS_MOBILE : p->home ? XCS_HOME : p->work ? XCS_WORK
           : p->other ? XCS_OTHER : XCS_NONE;
}

static void item_add(struct xcbuild *b, int kind, int sub, const char *val)
{
    int l = clen(val);
    struct xc_item_buf *it = &b->items;

    if (!l || it->n >= XC_ITEMS_MAX || it->len + 2 + l > XC_DETAIL_MAX)
        return;
    it->b[it->len++] = kind | (sub << 4);
    memcpy(it->b + it->len, val, l + 1);
    it->len += l + 1;
    it->n++;
}

static void card_reset(struct xcbuild *b)
{
    b->given[0] = b->family[0] = b->fn[0] = b->org[0] = b->note[0] = '\0';
    b->items.len = 0;
    b->items.n = 0;
}

/* a birthday: 19850412, 1985-04-12, --0412, 1985-04-12T00:00:00Z ->
 * "4/12/1985" or "4/12"; the placeholder year 1604 (and 0) is no year */
static void bday_fmt(const char *v, char *out, int max)
{
    int d[8], nd = 0, i = 0, y = 0, m, dd;
    char t[16];

    if (v[0] == '-' && v[1] == '-')
        i = 2;
    for (; v[i] && nd < 8; i++)
    {
        if (v[i] >= '0' && v[i] <= '9')
            d[nd++] = v[i] - '0';
        else if (v[i] == '-')
            continue;
        else
            break;
    }
    out[0] = '\0';
    if (v[0] == '-' && v[1] == '-' ? nd < 4 : nd < 8)
        return;
    if (v[0] == '-' && v[1] == '-')
    {
        m = d[0] * 10 + d[1];
        dd = d[2] * 10 + d[3];
    }
    else
    {
        y = d[0] * 1000 + d[1] * 100 + d[2] * 10 + d[3];
        m = d[4] * 10 + d[5];
        dd = d[6] * 10 + d[7];
    }
    if (m < 1 || m > 12 || dd < 1 || dd > 31)
        return;
    i = 0;
    if (m >= 10)
        t[i++] = '0' + m / 10;
    t[i++] = '0' + m % 10;
    t[i++] = '/';
    if (dd >= 10)
        t[i++] = '0' + dd / 10;
    t[i++] = '0' + dd % 10;
    if (y > 0 && y != 1604)
    {
        t[i++] = '/';
        t[i++] = '0' + y / 1000;
        t[i++] = '0' + y / 100 % 10;
        t[i++] = '0' + y / 10 % 10;
        t[i++] = '0' + y % 10;
    }
    t[i] = '\0';
    if (clen(t) < max)
        memcpy(out, t, clen(t) + 1);
}

static void card_end(struct xcbuild *b);

/* one property: name, parameters, value */
static void property(struct xcbuild *b, char *line, int len)
{
    int colon = -1, q = 0, i, nl, hl;
    char *name, *value, *comp[8];
    struct params p;
    char *t0 = b->tmp[0], *t1 = b->tmp[1];

    for (i = 0; i < len; i++)
    {
        if (line[i] == '"')
            q = !q;
        else if (line[i] == ':' && !q)
        {
            colon = i;
            break;
        }
    }
    if (colon <= 0)
        return;
    line[colon] = '\0';
    value = line + colon + 1;
    hl = colon;
    for (nl = 0; nl < hl && line[nl] != ';'; nl++)
        ;
    name = line;
    for (i = 0; i < nl; i++)            /* a group: "item1.TEL" */
        if (line[i] == '.')
            name = line + i + 1;
    nl -= (int)(name - line);

    if (nl == 5 && ci_find(name, 5, "BEGIN"))
    {
        if (ci_eq(value, "VCARD"))
        {
            card_reset(b);
            b->in_card = 1;
            b->cards++;
        }
        return;
    }
    if (nl == 3 && ci_find(name, 3, "END"))
    {
        if (b->in_card && ci_eq(value, "VCARD"))
        {
            card_end(b);
            b->in_card = 0;
        }
        return;
    }
    if (!b->in_card)
        return;
    {
        int ps = (int)(name - line) + nl + 1;

        parse_params(&p, line, ps < hl ? ps : hl, hl);
    }
    if (p.b64)
        return;                         /* PHOTO, LOGO, KEY: skipped */
    {
        int vl = clen(value);

        if (nl == 2 && ci_find(name, 2, "FN"))
            decode(value, vl, b->fn, sizeof(b->fn), p.qp, p.latin1);
        else if (nl == 1 && ci_find(name, 1, "N"))
        {
            int nc = split(value, comp, 8);

            decode(comp[0], clen(comp[0]), b->family, sizeof(b->family),
                   p.qp, p.latin1);
            if (nc > 1)
                decode(comp[1], clen(comp[1]), b->given, sizeof(b->given),
                       p.qp, p.latin1);
        }
        else if (nl == 3 && ci_find(name, 3, "ORG"))
        {
            split(value, comp, 8);
            decode(comp[0], clen(comp[0]), b->org, sizeof(b->org), p.qp,
                   p.latin1);
        }
        else if (nl == 3 && ci_find(name, 3, "TEL"))
        {
            const char *v = value;

            if (vl > 4 && ci_find(v, 4, "TEL:"))
                v += 4, vl -= 4;
            decode(v, vl, t0, XC_VAL_MAX, p.qp, p.latin1);
            item_add(b, XCD_PHONE, sub_of(&p), t0);
        }
        else if (nl == 5 && ci_find(name, 5, "EMAIL"))
        {
            decode(value, vl, t0, XC_VAL_MAX, p.qp, p.latin1);
            p.cell = 0;
            item_add(b, XCD_EMAIL, sub_of(&p), t0);
        }
        else if (nl == 3 && ci_find(name, 3, "ADR"))
        {
            int nc = split(value, comp, 8), k;
            char *out = b->tmp[1];

            out[0] = '\0';
            /* street / city, region postal / country */
            for (k = 2; k < nc && k < 7; k++)
            {
                decode(comp[k], clen(comp[k]), t0, XC_VAL_MAX, p.qp,
                       p.latin1);
                if (!t0[0])
                    continue;
                if (k == 2 || k == 6)
                {
                    if (out[0])
                        append(out, XC_VAL_MAX * 2, "\n");
                }
                else if (k == 3)
                {
                    if (out[0])
                        append(out, XC_VAL_MAX * 2, "\n");
                }
                else if (out[0] && out[clen(out) - 1] != '\n')
                    append(out, XC_VAL_MAX * 2, k == 4 ? ", " : " ");
                append(out, XC_VAL_MAX * 2, t0);
            }
            if (out[0])
            {
                out[XC_VAL_MAX * 2 - 1] = '\0';
                p.cell = 0;
                item_add(b, XCD_ADDR, sub_of(&p), out);
            }
        }
        else if (nl == 4 && ci_find(name, 4, "BDAY"))
        {
            decode(value, vl, t0, XC_VAL_MAX, p.qp, p.latin1);
            bday_fmt(t0, t1, 16);
            item_add(b, XCD_BDAY, XCS_NONE, t1);
        }
        else if (nl == 4 && ci_find(name, 4, "NOTE"))
            decode(value, vl, b->note, sizeof(b->note), p.qp, p.latin1);
    }
}

/* -- the logical line ---------------------------------------------------- */

static int line_is_qp(const char *l, int n)
{
    int c;

    for (c = 0; c < n && l[c] != ':'; c++)
        ;
    return ci_find(l, c, "QUOTED-PRINTABLE");
}

static void flush(struct xcbuild *b)
{
    if (b->pl)
    {
        char *l = b->line;
        int n = b->pl;

        l[n] = '\0';
        if (n >= 3 && (uint8_t)l[0] == 0xef && (uint8_t)l[1] == 0xbb
            && (uint8_t)l[2] == 0xbf)
            l += 3, n -= 3;
        b->pl = 0;
        property(b, l, n);
    }
}

void xcb_feed(struct xcbuild *b, const void *buf, uint32_t n)
{
    const uint8_t *p = buf;

    for (; n; n--, p++)
    {
        uint8_t c = *p;

        if (c == '\r')
            continue;
        if (c == '\n')
        {
            if (b->bol)
            {
                flush(b);               /* an empty line ends a line */
                b->softcont = 0;
            }
            else
            {
                b->bol = 1;
                if (b->pl && b->line[b->pl - 1] == '='
                    && line_is_qp(b->line, b->pl))
                {
                    b->pl--;            /* a soft break: the next line joins */
                    b->softcont = 1;
                }
            }
            continue;
        }
        if (b->bol)
        {
            b->bol = 0;
            if (b->softcont)
                b->softcont = 0;
            else if (c == ' ' || c == '\t')
                continue;               /* a fold: the space is dropped */
            else
                flush(b);
        }
        if (b->pl < XC_LINE_MAX - 1)
            b->line[b->pl++] = c;       /* past the cap: dropped */
    }
}

void xcb_end_file(struct xcbuild *b)
{
    flush(b);
    b->bol = b->softcont = 0;
    b->in_card = 0;
}

/* -- the cards ------------------------------------------------------------- */

static uint32_t pool_put(struct xcbuild *b, const char *s)
{
    uint32_t l = strlen(s);
    uint32_t off;

    if (!l)
        return 0;
    off = b->pool_used;
    memcpy(b->pool + b->pool_used, s, l + 1);
    b->pool_used += l + 1;
    return off;
}

/* Splits "A B C" at its last space: given = "A B", family = "C" */
static void split_fn(struct xcbuild *b)
{
    char *sp = strrchr(b->fn, ' ');

    if (!sp)
    {
        strncpy(b->given, b->fn, sizeof(b->given) - 1);
        b->given[sizeof(b->given) - 1] = '\0';
        return;
    }
    *sp = '\0';
    strncpy(b->given, b->fn, sizeof(b->given) - 1);
    b->given[sizeof(b->given) - 1] = '\0';
    strncpy(b->family, sp + 1, sizeof(b->family) - 1);
    b->family[sizeof(b->family) - 1] = '\0';
}

static void card_end(struct xcbuild *b)
{
    struct xc_src *s;
    uint32_t need, label;
    uint16_t flags = 0;
    const char *first_val = NULL;
    char tmp[XC_NAME_MAX];

    if (b->tick && (b->cards & 63) == 0)
        b->tick();
    if (!b->given[0] && !b->family[0] && b->fn[0])
        split_fn(b);
    if (!b->given[0] && !b->family[0])
    {
        if (b->org[0])
        {
            strncpy(b->given, b->org, sizeof(b->given) - 1);
            b->given[sizeof(b->given) - 1] = '\0';
            b->org[0] = '\0';
            flags = XCONTACT_ORG_ONLY;
        }
        else if (b->items.n)
        {
            /* the first item's text (a phone or an e-mail) names the card */
            first_val = b->items.b + 1;
            strncpy(tmp, first_val, sizeof(tmp) - 1);
            tmp[sizeof(tmp) - 1] = '\0';
            strncpy(b->given, tmp, sizeof(b->given) - 1);
            b->given[sizeof(b->given) - 1] = '\0';
            flags = XCONTACT_NO_NAME;
        }
        else
            return;                     /* nothing to show */
    }
    if (b->note[0])
        item_add(b, XCD_NOTE, XCS_NONE, b->note);
    label = strlen(b->given) + (b->given[0] && b->family[0] ? 1 : 0)
            + strlen(b->family) + 1;
    if (label > XIDX_LABEL_MAX)
        label = XIDX_LABEL_MAX;         /* the row's label is cut there */
    /* what this card adds to the file: a row in each order (record,
     * label, and its share of a page's header, padding and directory
     * entry: 15 B over at least 7 rows), the organisation's NUL and its
     * text, the items. A page holds more than XC_PAGE_BYTES - (the
     * largest row) - its header, so the row bytes bound the pages. */
    need = 2 * (sizeof(struct xcontact_rec) + label + 3)
           + strlen(b->org) + 1 + b->items.len + 1;
    if (b->n >= b->max || b->est + need > b->out_cap
        || b->row_bytes + sizeof(struct xcontact_rec) + label
           > (uint32_t)(XCONTACT_PDIR_MAX - 1)
             * (XC_PAGE_BYTES - sizeof(struct xidx_page)
                - sizeof(struct xcontact_rec) - XIDX_LABEL_MAX)
        || b->pool_used + strlen(b->given) + strlen(b->family)
           + strlen(b->org) + 3 + b->items.len > b->pool_cap)
    {
        b->cut = 1;
        return;
    }
    b->est += need;
    b->row_bytes += sizeof(struct xcontact_rec) + label;
    s = &b->src[b->n++];
    s->given = pool_put(b, b->given);
    s->family = pool_put(b, b->family);
    s->org = pool_put(b, b->org);
    s->detail = b->items.len ? b->pool_used : 0;
    if (b->items.len)
    {
        memcpy(b->pool + b->pool_used, b->items.b, b->items.len);
        b->pool_used += b->items.len;
    }
    s->dlen = b->items.len;
    s->flags = flags;
}

void xcb_init(struct xcbuild *b, struct xc_src *src, int max, char *pool,
              uint32_t pool_cap, uint32_t out_cap, void (*tick)(void))
{
    memset(b, 0, sizeof(*b));
    b->src = src;
    b->max = max;
    b->pool = pool;
    pool[0] = '\0';                     /* offset 0 is the empty string */
    b->pool_used = 1;
    b->pool_cap = pool_cap;
    /* header, tab, two pages' slack, the card block's "" and padding */
    b->est = XCONTACT_DIR_OFF + 2 * (sizeof(struct xidx_page) + 3) + 4;
    b->out_cap = out_cap;
    b->tick = tick;
}

/* -- the order ------------------------------------------------------------- */

struct sort_ctx {
    const struct xc_src *src;
    const char *pool;
    xcontact_cmp_fn cmp;
    int last;                           /* 1: Family Given */
};

static void key_of(const struct sort_ctx *c, uint16_t i, char *k)
{
    const struct xc_src *s = &c->src[i];
    const char *a = c->pool + (c->last ? s->family : s->given);
    const char *b = c->pool + (c->last ? s->given : s->family);
    int n = 0;

    while (*a && n < XC_LABEL_MAX - 2)
        k[n++] = *a++;
    if (n && *b)
        k[n++] = ' ';
    while (*b && n < XC_LABEL_MAX - 1)
        k[n++] = *b++;
    k[n] = '\0';
}

static int less(const struct sort_ctx *c, uint16_t a, uint16_t b)
{
    char ka[XC_LABEL_MAX], kb[XC_LABEL_MAX];
    int r;

    key_of(c, a, ka);
    key_of(c, b, kb);
    r = c->cmp(ka, kb);
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

/* "Given Family" into out (XC_LABEL_MAX); returns its length, no NUL */
static uint32_t name_put(const struct xcbuild *b, const struct xc_src *s,
                         char *out)
{
    const char *g = b->pool + s->given, *f = b->pool + s->family;
    uint32_t n = 0;

    while (*g && n < XC_LABEL_MAX - 2)
        out[n++] = *g++;
    if (n && *f)
        out[n++] = ' ';
    while (*f && n < XC_LABEL_MAX - 1)
        out[n++] = *f++;
    out[n] = '\0';
    return n;
}

/* The row's label (out: NULL to count): "Given Family", cut to
 * XIDX_LABEL_MAX (xidx_label_put()); its bytes with the NUL */
static uint32_t label_put(const struct xcbuild *b, const struct xc_src *s,
                          char *out)
{
    char full[XC_LABEL_MAX];

    return xidx_label_put(out, full, name_put(b, s, full));
}

static uint32_t label_len(const struct xcbuild *b, const struct xc_src *s)
{
    return label_put(b, s, NULL);
}

/* one order's rows, for the pager */
struct order {
    const struct xcbuild *b;
    const uint16_t *perm;               /* row -> card */
    const uint32_t *card;               /* card -> its offset in the block */
};

static uint32_t row_len(void *ctx, uint32_t row)
{
    const struct order *o = ctx;

    return label_len(o->b, &o->b->src[o->perm[row]]);
}

static uint32_t row_put(void *ctx, uint32_t row, void *rec, char *str,
                        uint32_t str_rel)
{
    const struct order *o = ctx;
    const struct xc_src *s = &o->b->src[o->perm[row]];
    struct xcontact_rec *r = rec;

    r->card = o->card[o->perm[row]];
    r->label = (uint16_t)str_rel;
    r->flags = s->flags;
    return label_put(o->b, s, str);
}

/* page p of an order at *at (4-aligned after it); false: no room */
static int page_out(uint8_t *out, uint32_t out_max, uint32_t *at,
                    struct xidx_pdir *dir, int np, int p, uint32_t n,
                    struct order *o)
{
    if (*at + dir[p].bytes + 3 > out_max)
        return 0;
    dir[p].off = *at;
    xidx_page_write(out + *at, dir, np, p, n, sizeof(struct xcontact_rec),
                    row_put, o);
    *at += dir[p].bytes;
    while (*at & 3)
        out[(*at)++] = 0;
    return 1;
}

int32_t xcb_emit(struct xcbuild *b, void *out, uint32_t out_max,
                 uint32_t sig, xcontact_cmp_fn cmp, uint16_t *scratch)
{
    struct xidx_hdr *h = out;
    struct xcontact_tab *tab = (struct xcontact_tab *)(h + 1);
    struct xidx_pdir *df = (struct xidx_pdir *)((uint8_t *)out
                                                + XCONTACT_DIR_OFF), *dl;
    struct sort_ctx c = { b->src, b->pool, cmp, 0 };
    uint16_t *pf = scratch, *pl = scratch + b->n;
    uint32_t *card = (uint32_t *)(void *)(scratch + 2 * b->n);
    struct order of = { b, pf, card }, ol = { b, pl, card };
    uint32_t n = (uint32_t)b->n, at, str_off, i, k;
    int nf, nl, p;
    uint8_t *str;

    if (b->n < 0 || b->n > XCONTACT_MAX || out_max < 256)
        return XIDX_ECAP;
    for (i = 0; i < n; i++)
        pf[i] = pl[i] = (uint16_t)i;
    sort_perm(&c, pf, b->n);
    c.last = 1;
    sort_perm(&c, pl, b->n);
    /* each card's place in the block, in First order ("" at 0) */
    for (i = 0, at = 1; i < n; i++)
    {
        const struct xc_src *s = &b->src[pf[i]];

        card[pf[i]] = at;
        at += strlen(b->pool + s->org) + 1 + s->dlen + 1;
    }
    memset(h, 0, sizeof(*h) + sizeof(*tab));
    k = (out_max - XCONTACT_DIR_OFF) / (2 * sizeof(*df));
    if (k > XCONTACT_PDIR_MAX)
        k = XCONTACT_PDIR_MAX;
    nf = xidx_page_split(n, sizeof(struct xcontact_rec), XC_PAGE_BYTES,
                         row_len, &of, df, k);
    if (nf < 0)
        return XIDX_ECAP;
    dl = df + nf;
    nl = xidx_page_split(n, sizeof(struct xcontact_rec), XC_PAGE_BYTES,
                         row_len, &ol, dl, k);
    if (nl < 0)
        return XIDX_ECAP;
    tab->npage[0] = nf;
    tab->npage[1] = nl;
    tab->hdr_end = at = XCONTACT_DIR_OFF + (nf + nl) * sizeof(*df);
    /* First page 0, Last page 0, then the rest of each */
    if ((nf && !page_out(out, out_max, &at, df, nf, 0, n, &of))
        || (nl && !page_out(out, out_max, &at, dl, nl, 0, n, &ol)))
        return XIDX_ECAP;
    for (p = 1; p < nf; p++)
        if (!page_out(out, out_max, &at, df, nf, p, n, &of))
            return XIDX_ECAP;
    for (p = 1; p < nl; p++)
        if (!page_out(out, out_max, &at, dl, nl, p, n, &ol))
            return XIDX_ECAP;
    /* the card block */
    str_off = at;
    str = (uint8_t *)out + str_off;
    if (out_max - str_off < 1)
        return XIDX_ECAP;
    str[0] = '\0';
    at = 1;
    for (i = 0; i < n; i++)
    {
        const struct xc_src *s = &b->src[pf[i]];
        uint32_t ol_ = strlen(b->pool + s->org);

        if (str_off + at + ol_ + 1 + s->dlen + 1 > out_max)
            return XIDX_ECAP;
        memcpy(str + at, b->pool + s->org, ol_ + 1);
        at += ol_ + 1;
        if (s->dlen)
            memcpy(str + at, b->pool + s->detail, s->dlen);
        at += s->dlen;
        str[at++] = 0;                  /* the end of the items */
    }
    h->kind = XIDX_CONTACTS;
    h->sig = sig;
    h->count = n;
    h->rec_off = XCONTACT_TAB_OFF;
    h->str_off = str_off;
    h->str_bytes = at;
    xidx_seal(out, str_off + at);
    return (int32_t)(str_off + at);
}

int xcontact_is_file(const char *name)
{
    int len = (int)strlen(name), i;

    if (name[0] == '.' || len <= 4)
        return 0;
    for (i = 0; i < 4; i++)
        if (up((uint8_t)name[len - 4 + i]) != up((uint8_t)XCONTACT_EXT[i]))
            return 0;
    return 1;
}
