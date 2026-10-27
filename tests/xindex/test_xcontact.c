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
 * Host test of the vCard parser and the Contacts index,
 * apps/ipodui/xcontact_core.c
 * (P12 W3-K, brief 3.5): the fixtures are the cases the parser
 * must take
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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ipodui/xcontact_core.h"

static int fails;
#define CHECK(c) do { if (!(c)) { fails++; \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
#define EQ(a, b) do { long _a = (long)(a), _b = (long)(b); if (_a != _b) { \
    fails++; fprintf(stderr, "FAIL %s:%d: %s = %ld, want %ld\n", \
    __FILE__, __LINE__, #a, _a, _b); } } while (0)
#define STREQ(a, b) do { const char *_a = (a), *_b = (b); \
    if (strcmp(_a, _b)) { fails++; \
    fprintf(stderr, "FAIL %s:%d: %s = \"%s\", want \"%s\"\n", __FILE__, \
    __LINE__, #a, _a, _b); } } while (0)

/* The firmware's crc_32 (firmware/common/crc32.c), bitwise */
uint32_t crc_32(const void *src, uint32_t len, uint32_t crc);
uint32_t crc_32(const void *src, uint32_t len, uint32_t crc)
{
    const unsigned char *p = src;
    while (len--)
    {
        int b;
        crc ^= (uint32_t)*p++ << 24;
        for (b = 0; b < 8; b++)
            crc = crc & 0x80000000u ? (crc << 1) ^ 0x04c11db7u : crc << 1;
    }
    return crc;
}

/* ui_sortkey stand-in: ASCII case folded */
static int cmp(const char *a, const char *b)
{
    for (;; a++, b++)
    {
        int x = *a, y = *b;

        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y || !x)
            return x - y;
    }
}

#define OUT_MAX XIDX_CONTACTS_FILE_MAX
static struct xcbuild b;
static struct xc_src src[XCONTACT_MAX];
static char pool[XC_POOL];
static uint16_t scratch[XCONTACT_SCRATCH(XCONTACT_MAX)];
static uint32_t outw[OUT_MAX / 4];
static uint8_t *out = (uint8_t *)outw;
static int32_t bytes;

/* Parses text in pieces of 'chunk' bytes (0: whole) and lays out */
static int parse(const char *text, size_t len, size_t chunk)
{
    size_t at = 0;

    xcb_init(&b, src, XCONTACT_MAX, pool, XC_POOL, OUT_MAX - 64, NULL);
    if (!chunk)
        chunk = len ? len : 1;
    while (at < len)
    {
        size_t k = len - at < chunk ? len - at : chunk;

        xcb_feed(&b, text + at, k);
        at += k;
    }
    xcb_end_file(&b);
    bytes = xcb_emit(&b, out, OUT_MAX, 0x1234, cmp, scratch);
    return (int)bytes;
}

static const struct xidx_hdr *hdr(void) { return (const struct xidx_hdr *)out; }
static const struct xcontact_tab *tab(void) { return (const struct xcontact_tab *)(out + 32); }
static const struct xidx_pdir *dir(int last)
{
    return (const struct xidx_pdir *)(out + XCONTACT_DIR_OFF)
           + (last ? tab()->npage[0] : 0);
}
/* row i of an order through its directory and page (the reader's path) */
static const void *page_at(int last, int i, const struct xcontact_rec **r)
{
    const struct xidx_pdir *d = dir(last);
    int p = xidx_dir_find(d, tab()->npage[last], i);
    const void *pg = out + d[p].off;

    *r = xidx_page_rec(pg, i - d[p].row0, sizeof(struct xcontact_rec));
    return pg;
}
static const struct xcontact_rec *rec(int i)
{
    const struct xcontact_rec *r;

    page_at(0, i, &r);
    return r;
}
static const struct xcontact_rec *ord(int j)
{
    const struct xcontact_rec *r;

    page_at(1, j, &r);
    return r;
}
static const char *label(int last, int i)
{
    const struct xcontact_rec *r;
    const void *pg = page_at(last, i, &r);

    return xidx_page_str(pg, r->label);
}
static const char *str(uint32_t off) { return (const char *)out + hdr()->str_off + off; }
static const char *first_label(int i) { return label(0, i); }
static const char *last_label(int j) { return label(1, j); }
static const char *org(int i) { return str(rec(i)->card); }
/* a card's item bytes: from after its organisation to its 0 end byte */
static uint32_t detail_len(int i)
{
    const char *p = org(i) + strlen(org(i)) + 1, *q = p;

    while (*q)
        q += 1 + strlen(q + 1) + 1;
    return q - p;
}

/* a card's items as "k.s:text|k.s:text" */
static const char *items(int i)
{
    static char buf[1024];
    const char *p = org(i) + strlen(org(i)) + 1;
    uint32_t left = detail_len(i);
    int o = 0;

    buf[0] = '\0';
    while (left)
    {
        uint8_t k = (uint8_t)*p++;
        uint32_t l = strlen(p);

        o += snprintf(buf + o, sizeof(buf) - o, "%s%d.%d:%s", o ? "|" : "",
                      XCD_KIND(k), XCD_SUB(k), p);
        p += l + 1;
        left -= 1 + l + 1;
    }
    return buf;
}

/* the header, both directories and every page are sound; both first
 * pages follow the directories */
static void check_file(void)
{
    int l, p;

    CHECK(xidx_check_body(out, bytes) == XIDX_OK);
    EQ(hdr()->kind, XIDX_CONTACTS);
    EQ(hdr()->sig, 0x1234);
    CHECK(bytes <= XIDX_CONTACTS_FILE_MAX);
    EQ(tab()->hdr_end, XCONTACT_DIR_OFF
                       + 8 * (tab()->npage[0] + tab()->npage[1]));
    for (l = 0; l < 2; l++)
    {
        const struct xidx_pdir *d = dir(l);
        int np = tab()->npage[l];

        CHECK(np <= XCONTACT_PDIR_MAX);
        EQ(xidx_dir_check(d, np, hdr()->count, tab()->hdr_end,
                          hdr()->str_off), XIDX_OK);
        for (p = 0; p < np; p++)
            EQ(xidx_page_check(out + d[p].off, &d[p],
                               xidx_page_rows(d, np, p, hdr()->count),
                               sizeof(struct xcontact_rec)), XIDX_OK);
    }
    if (tab()->npage[0])
    {
        EQ(dir(0)[0].off, tab()->hdr_end);
        EQ(dir(1)[0].off, (dir(0)[0].off + dir(0)[0].bytes + 3) & ~3u);
    }
}

#define P(s) s, strlen(s)

static void test_v30(void)
{
    const char *v =
        "BEGIN:VCARD\r\nVERSION:3.0\r\nFN:Ada Lovelace\r\nN:Lovelace;Ada;;;\r\n"
        "ORG:Analytical Engines\\, Ltd;Dept\r\n"
        "TEL;TYPE=CELL,VOICE:+44 20 7946 0001\r\n"
        "TEL;TYPE=WORK:555-0100\r\n"
        "EMAIL;TYPE=INTERNET,HOME:ada@example.org\r\n"
        "ADR;TYPE=HOME:;;12 Crown St\\; Flat 2;London;;N1 9AA;UK\r\n"
        "BDAY:1815-12-10\r\n"
        "NOTE:First programmer.\\nSecond line\r\n"
        "PHOTO;ENCODING=b;TYPE=JPEG:/9j/4AAQSkZJRgABAQEASABIAAD\r\n"
        " AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA\r\n"
        "END:VCARD\r\n";
    size_t chunks[] = { 0, 1, 7, 64 }, i;

    for (i = 0; i < 4; i++)
    {
        EQ(parse(P(v), chunks[i]) > 0, 1);
        check_file();
        EQ(hdr()->count, 1);
        EQ(org(0)[strlen(org(0)) + 1 + detail_len(0)], 0);   /* the end byte */
        STREQ(first_label(0), "Ada Lovelace");
        STREQ(last_label(0), "Ada Lovelace");
        STREQ(org(0), "Analytical Engines, Ltd");
        STREQ(items(0), "1.1:+44 20 7946 0001|1.3:555-0100|2.2:ada@example.org|"
                        "3.2:12 Crown St; Flat 2\nLondon N1 9AA\nUK|"
                        "4.0:12/10/1815|5.0:First programmer.\nSecond line");
    }
}

static void test_v21_qp(void)
{
    const char *v =
        "BEGIN:VCARD\nVERSION:2.1\n"
        "N;CHARSET=UTF-8;ENCODING=QUOTED-PRINTABLE:=C3=96zt=C3=BCrk;Sel=C3=A7uk;;;\n"
        "FN;CHARSET=UTF-8;ENCODING=QUOTED-PRINTABLE:Sel=C3=A7uk =C3=96zt=C3=BC=\n"
        "rk\n"
        "TEL;CELL;VOICE:0532 111 22 33\n"
        "TEL;HOME:0212 000 00 00\n"
        "TEL;WORK;FAX:0212 000 00 01\n"
        "NOTE;ENCODING=QUOTED-PRINTABLE:line one=0D=0Aline =\n"
        "two\n"
        "EMAIL;WORK:s@example.com\n"
        "END:VCARD\n";
    size_t chunks[] = { 0, 1, 5 }, i;

    for (i = 0; i < 3; i++)
    {
        parse(P(v), chunks[i]);
        check_file();
        EQ(hdr()->count, 1);
        STREQ(first_label(0), "Selçuk Öztürk");
        STREQ(items(0), "1.1:0532 111 22 33|1.2:0212 000 00 00|1.3:0212 000 00 01|"
                        "2.3:s@example.com|5.0:line one\nline two");
    }
}

static void test_names(void)
{
    /* missing FN: from N; N missing: FN split at its last space; one word;
     * organisation only; nothing but a phone; nothing at all; Latin-1 */
    const char *v =
        "BEGIN:VCARD\nVERSION:3.0\nN:Zed;Zoe;;;\nEND:VCARD\n"
        "BEGIN:VCARD\nVERSION:3.0\nFN:Mary Jane van Dyke\nEND:VCARD\n"
        "BEGIN:VCARD\nVERSION:3.0\nFN:Madonna\nEND:VCARD\n"
        "BEGIN:VCARD\nVERSION:3.0\nORG:Acme Plumbing\nTEL:555-1212\nEND:VCARD\n"
        "BEGIN:VCARD\nVERSION:3.0\nTEL:555-9999\nEND:VCARD\n"
        "BEGIN:VCARD\nVERSION:3.0\nTITLE:Nobody\nEND:VCARD\n"
        "BEGIN:VCARD\nVERSION:2.1\nCHARSET=ISO-8859-1\n"
        "N;CHARSET=ISO-8859-1:M\xfc" "ller;J\xfc" "rgen\nEND:VCARD\n"
        "BEGIN:VCARD\nVERSION:3.0\nFN:Bad \xff\xfe Bytes\nEND:VCARD\n";
    int i;

    parse(P(v), 0);
    check_file();
    EQ(hdr()->count, 7);                /* the empty card is dropped */
    /* First order (the stand-in compare puts digits first; the firmware's
     * ui_sortkey puts them after the letters) */
    STREQ(first_label(0), "555-9999");
    STREQ(first_label(1), "Acme Plumbing");
    STREQ(first_label(2), "Bad ÿþ Bytes");
    STREQ(first_label(3), "J\xc3\xbcrgen M\xc3\xbcller");
    STREQ(first_label(4), "Madonna");
    STREQ(first_label(5), "Mary Jane van Dyke");
    STREQ(first_label(6), "Zoe Zed");
    CHECK(rec(1)->flags & XCONTACT_ORG_ONLY);
    CHECK(rec(0)->flags & XCONTACT_NO_NAME);
    STREQ(org(1), "");
    for (i = 0; i < 7; i++)
        CHECK(ord(i)->card > 0 && ord(i)->card < hdr()->str_bytes);
    /* the organisation card keeps its phone */
    STREQ(items(1), "1.0:555-1212");
    STREQ(items(0), "1.0:555-9999");
}

static void test_sort_orders(void)
{
    const char *v =
        "BEGIN:VCARD\nVERSION:3.0\nN:Young;Alice;;;\nFN:Alice Young\nEND:VCARD\n"
        "BEGIN:VCARD\nVERSION:3.0\nN:Adams;Zed;;;\nFN:Zed Adams\nEND:VCARD\n"
        "BEGIN:VCARD\nVERSION:3.0\nN:Moore;Bob;;;\nFN:Bob Moore\nEND:VCARD\n";

    parse(P(v), 0);
    check_file();
    STREQ(first_label(0), "Alice Young");
    STREQ(first_label(1), "Bob Moore");
    STREQ(first_label(2), "Zed Adams");
    STREQ(last_label(0), "Zed Adams");
    STREQ(last_label(1), "Bob Moore");
    STREQ(last_label(2), "Alice Young");
    EQ(ord(0)->card, rec(2)->card);
    EQ(ord(2)->card, rec(0)->card);
    CHECK(rec(0)->card < rec(1)->card && rec(1)->card < rec(2)->card);
}

static void test_v40(void)
{
    const char *v =
        "BEGIN:VCARD\r\nVERSION:4.0\r\nFN:Grace Hopper\r\nN:Hopper;Grace;;;\r\n"
        "TEL;VALUE=uri;TYPE=\"voice,home\":tel:+1-555-0142\r\n"
        "TEL;VALUE=uri;TYPE=\"cell\":tel:+1-555-0143\r\n"
        "item1.EMAIL;TYPE=work:grace@example.mil\r\n"
        "item1.X-ABLabel:_$!<Other>!$_\r\n"
        "BDAY:--1209\r\n"
        "ORG:US Navy\r\nEND:VCARD\r\n"
        "BEGIN:VCARD\r\nVERSION:4.0\r\nFN:Folded Name With A Very Long\r\n"
        "  Tail\r\nTEL:1\r\n 2\r\n 3\r\nBDAY:19700101\r\nEND:VCARD\r\n";

    parse(P(v), 3);
    check_file();
    EQ(hdr()->count, 2);
    STREQ(first_label(0), "Folded Name With A Very Long Tail");
    STREQ(items(0), "1.0:123|4.0:1/1/1970");
    STREQ(first_label(1), "Grace Hopper");
    STREQ(items(1), "1.2:+1-555-0142|1.1:+1-555-0143|2.3:grace@example.mil|"
                    "4.0:12/9");
    STREQ(org(1), "US Navy");
}

static void test_odd(void)
{
    /* no END, text outside a card, a card inside the file's end, a line
     * past the cap, a lone CR, a BOM */
    static char big[4000];
    char v[8192];
    int n;

    memset(big, 'x', sizeof(big) - 1);
    n = snprintf(v, sizeof(v),
                 "\xef\xbb\xbf" "BEGIN:VCARD\nVERSION:3.0\nFN:Bom Person\n"
                 "NOTE:%s\nTEL:555\nEND:VCARD\nstray line\n"
                 "BEGIN:VCARD\nFN:Unfinished\n", big);
    parse(v, n, 0);
    check_file();
    EQ(hdr()->count, 1);
    STREQ(first_label(0), "Bom Person");
    /* the long note is cut, the phone after it kept */
    CHECK(detail_len(0) <= XC_DETAIL_MAX);
    CHECK(strstr(items(0), "1.0:555") != NULL);
    /* empty input */
    parse("", 0, 0);
    check_file();
    EQ(hdr()->count, 0);
    /* two files in a row share the builder */
    xcb_init(&b, src, XCONTACT_MAX, pool, XC_POOL, OUT_MAX - 64, NULL);
    xcb_feed(&b, P("BEGIN:VCARD\nFN:Cut Mid"));
    xcb_end_file(&b);
    xcb_feed(&b, P("FN:Not A Card\nEND:VCARD\nBEGIN:VCARD\nFN:Real One\nEND:VCARD"));
    xcb_end_file(&b);
    bytes = xcb_emit(&b, out, OUT_MAX, 0x1234, cmp, scratch);
    check_file();
    EQ(hdr()->count, 1);
    STREQ(first_label(0), "Real One");
}

/* n generated cards into v; heavy: every card with an organisation and
 * a long note (the cut case), else "typical": a ~14-byte name, two
 * phones, an e-mail, one in five with an organisation or an address */
static size_t gen(char *v, size_t cap, int n, int heavy)
{
    static const char *given[] = { "Anna", "Ben", "Carla", "David", "Emma",
                                   "Felix", "Grace", "Hugo", "Iris", "Jonas" };
    size_t len = 0;
    int i;

    for (i = 0; i < n; i++)
    {
        int k = (i * 7919) % n;

        len += snprintf(v + len, cap - len,
                        "BEGIN:VCARD\r\nVERSION:3.0\r\nN:Family%04d;%s;;;\r\n"
                        "FN:%s Family%04d\r\nTEL;TYPE=CELL:+1 555 %04d\r\n"
                        "TEL;TYPE=HOME:+1 555 9%03d\r\nEMAIL:p%d@example.org\r\n",
                        k, given[k % 10], given[k % 10], k, k, k % 1000, k);
        if (heavy || i % 10 == 0)
            len += snprintf(v + len, cap - len, "ORG:Company %d\r\n", i % 40);
        if (!heavy && i % 10 == 5)
            len += snprintf(v + len, cap - len,
                            "ADR;TYPE=HOME:;;%d Main St;Springfield;;%05d;\r\n",
                            i, i);
        if (heavy)
            len += snprintf(v + len, cap - len, "NOTE:Some note text number "
                            "%d for padding the card out to its cap\r\n", i);
        len += snprintf(v + len, cap - len, "END:VCARD\r\n");
    }
    return len;
}

static void feed_all(const char *v, size_t len)
{
    size_t at, chunk = 8192;

    xcb_init(&b, src, XCONTACT_MAX, pool, XC_POOL,
             XIDX_CONTACTS_FILE_MAX - 64, NULL);
    for (at = 0; at < len; at += chunk)
        xcb_feed(&b, v + at, len - at < chunk ? len - at : chunk);
    xcb_end_file(&b);
    bytes = xcb_emit(&b, out, OUT_MAX, 0x1234, cmp, scratch);
}

/* The scale (W3-K): 2,000 typical cards fit whole; heavier or more cards
 * are cut at the caps, never failed */
static void test_large(void)
{
    size_t cap = 3000 * 400, len;
    char *v = malloc(cap + 1);
    int i;

    len = gen(v, cap, 2000, 0);
    feed_all(v, len);
    CHECK(bytes > 0);
    check_file();
    CHECK(!b.cut);
    EQ(hdr()->count, 2000);
    CHECK(b.pool_used <= XC_POOL);
    for (i = 1; i < 2000; i++)
    {
        CHECK(cmp(first_label(i - 1), first_label(i)) <= 0);
        CHECK(strchr(last_label(i - 1), ' ') != NULL);
    }
    /* the Last order sorts by "Family Given" */
    STREQ(last_label(0), "Anna Family0000");
    STREQ(last_label(1999), "Jonas Family1999");
    printf("typical: 2000 cards, %ld bytes (%ld per card), pages %u+%u, "
           "header region %u, pool %u\n", (long)bytes, (long)bytes / 2000,
           tab()->npage[0], tab()->npage[1], tab()->hdr_end,
           (unsigned)b.pool_used);
    /* 3,000 typical: the file cap (about 2,500, the count cap) */
    len = gen(v, cap, 3000, 0);
    feed_all(v, len);
    check_file();
    CHECK(b.cut);
    CHECK(hdr()->count >= 2400 && hdr()->count <= XCONTACT_MAX);
    EQ(b.cards, 3000);
    /* 2,000 heavy: the file cap */
    len = gen(v, cap, 2000, 1);
    feed_all(v, len);
    check_file();
    CHECK(b.cut);
    CHECK(hdr()->count > 1000 && hdr()->count < 2000);
    printf("heavy: %u cards of 2000 indexed, %ld bytes\n", hdr()->count,
           (long)bytes);
    free(v);
}

/* the tick: once per 64 cards */
static int ticks;
static void tick(void) { ticks++; }

static void test_tick(void)
{
    char v[200 * 60];
    int i, n = 0;

    for (i = 0; i < 200; i++)
        n += snprintf(v + n, sizeof(v) - n,
                      "BEGIN:VCARD\nFN:A B%d\nEND:VCARD\n", i);
    xcb_init(&b, src, XCONTACT_MAX, pool, XC_POOL, 56 * 1024, tick);
    xcb_feed(&b, v, n);
    xcb_end_file(&b);
    EQ(ticks, 3);
}

static void test_cap_and_fit(void)
{
    /* a card never lets the file pass out_cap */
    static char v[400 * 220];
    int i, n = 0;

    for (i = 0; i < 400; i++)
        n += snprintf(v + n, sizeof(v) - n, "BEGIN:VCARD\nFN:Name %d Surname%d\n"
                      "NOTE:%0150d\nEND:VCARD\n", i, i, i);
    xcb_init(&b, src, XCONTACT_MAX, pool, XC_POOL, 8 * 1024, NULL);
    xcb_feed(&b, v, n);
    xcb_end_file(&b);
    bytes = xcb_emit(&b, out, OUT_MAX, 0x1234, cmp, scratch);
    check_file();
    CHECK(b.cut);
    CHECK(bytes <= 8 * 1024);
}

/* W3-K review: long names. A row's label is cut to XIDX_LABEL_MAX (an
 * ellipsis at a character), every page but the last holds
 * XIDX_PAGE_ROWS_MIN rows, and every reader window (the rows from just
 * above the top to just below the last visible one, screens/contacts.c
 * keep_window) lies in two pages that a cache of two of the largest
 * pages shows whole: no blank row at any top */
static void test_long_names(void)
{
    static char v[300 * 400];
    static uint8_t cb[2 * (XC_PAGE_LIMIT + 3)];
    char gn[80], fn[80];
    int i, n = 0, l, p, np;
    uint32_t top, lo, hi, r, k;

    for (i = 0; i < 300; i++)
    {
        /* 63-byte given and family names (the parser's caps): 127-byte
         * labels, past 200 bytes of FN line with the rest */
        snprintf(gn, sizeof(gn), "Given%03d%055d", (i * 37) % 300, 0);
        snprintf(fn, sizeof(fn), "Family%03d%054d", i, 0);
        n += snprintf(v + n, sizeof(v) - n,
                      "BEGIN:VCARD\nVERSION:3.0\nN:%s;%s;;;\nFN:%s %s and "
                      "a long tail on the formatted name\nTEL:+1 555 %04d\n"
                      "END:VCARD\n", fn, gn, gn, fn, i);
    }
    feed_all(v, n);
    CHECK(bytes > 0);
    check_file();
    EQ(hdr()->count, 300);
    for (l = 0; l < 2; l++)
    {
        const struct xidx_pdir *d = dir(l);
        struct xidx_pcache c;

        np = tab()->npage[l];
        CHECK(np >= 2);
        for (p = 0; p + 1 < np; p++)
            CHECK(xidx_page_rows(d, np, p, 300) >= XIDX_PAGE_ROWS_MIN);
        for (p = 0; p < np; p++)
            CHECK(d[p].bytes <= XC_PAGE_LIMIT);
        xidx_pc_init(&c, cb, sizeof(cb));
        for (k = 0; k < 2 * 300; k++)
        {
            top = k < 300 ? (k * 5) % 300 : 599 - k;
            lo = top ? top - 1 : 0;
            hi = top + 9 < 300 ? top + 9 : 299;
            p = xidx_dir_find(d, np, lo);
            np = tab()->npage[l];
            CHECK(xidx_dir_find(d, np, hi) - p <= 1);
            for (; p <= xidx_dir_find(d, np, hi); p++)
                if (!xidx_pc_get(&c, l, p))
                {
                    uint8_t *at = xidx_pc_room(&c, d[p].bytes);

                    CHECK(at != NULL);
                    if (!at)
                        return;
                    memcpy(at, out + d[p].off, d[p].bytes);
                    xidx_pc_add(&c, l, p, d[p].bytes);
                }
            xidx_pc_get(&c, l, xidx_dir_find(d, np, lo));
            for (r = lo; r <= hi; r++)
            {
                int q = xidx_dir_find(d, np, r);
                const void *pg = xidx_pc_peek(&c, l, q);
                const struct xcontact_rec *e;
                const char *lab;

                CHECK(pg != NULL);
                if (!pg)
                    return;
                e = xidx_page_rec(pg, r - d[q].row0, sizeof(*e));
                lab = e ? xidx_page_str(pg, e->label) : NULL;
                CHECK(lab && !strcmp(lab, label(l, (int)r)));
                CHECK(lab && strlen(lab) == XIDX_LABEL_MAX - 1
                      && !strcmp(lab + strlen(lab) - 3, XIDX_ELLIPSIS));
            }
        }
    }
    /* the label is "Given Family" cut; the order is still the full key */
    CHECK(!strncmp(first_label(0), "Given000", 8));
    CHECK(!strncmp(last_label(0), "Given", 5));
    for (i = 1; i < 300; i++)
        CHECK(cmp(first_label(i - 1), first_label(i)) <= 0);
}

int main(void)
{
    test_v30();
    test_v21_qp();
    test_names();
    test_sort_orders();
    test_v40();
    test_odd();
    test_large();
    test_tick();
    test_cap_and_fit();
    test_long_names();
    CHECK(xcontact_is_file("a.vcf") && xcontact_is_file("B.VCF")
          && !xcontact_is_file("._a.vcf") && !xcontact_is_file("a.vcf.txt")
          && !xcontact_is_file(".vcf"));
    if (fails)
        fprintf(stderr, "%d failure(s)\n", fails);
    else
        printf("ok\n");
    return fails ? 1 : 0;
}
