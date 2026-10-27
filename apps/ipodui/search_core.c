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
 * iPod UI shell
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
/* Search matching (docs/phase11-brief.md section 3.1, search.h). Pure C:
 * stdint/stdbool/string only, so tests/search compiles it on the host and
 * tests/search/insn.sh counts its ARM instructions (lean rule 8).
 *
 * Folding (the ui_sortkey() rules of sortkey.c, without dropping "The "
 * and without the '#' group): ASCII letters lower-cased, Latin-1 and
 * Latin Extended-A letters folded to ASCII (two-letter folds: ae, ss, th,
 * ij, oe), every other ASCII or Latin-1 character (punctuation, symbols,
 * white space, no-break spaces) a word separator; Latin Extended-B, Greek,
 * Cyrillic and Vietnamese case folded by sortkey.h ui_casefold_char()
 * (#47: "яблоко" finds "Яблоко"); other UTF-8 characters (CJK, ...) keep
 * their bytes and are word characters.
 *
 * Matching is word-prefix (⚠️ S1): a query word matches where a haystack
 * word starts (position 0, or after a separator) with it; a query of
 * several words needs each to prefix-match a later word than the one
 * before (in order; greedy, which is exact for prefixes).
 *
 * Cost (the hot loop of srch_match()): a haystack byte is one table
 * lookup in 'stop', a 256-byte table built once per query's first
 * character (the query's first folded byte in both cases, the Latin
 * lead bytes C3..C5, and NUL): ldrb, ldrb, cmp, beq per byte. Only those
 * bytes go to the slow path (word start, then the whole query). The
 * table is the one static: Search runs on the UI thread only. */
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "search.h"
#include "sortkey.h"     /* ui_casefold_char(): the shared case folding */

/* U+00C0..U+00FF, as sortkey.c ('*': a symbol, x and the division sign) */
static const char latin1[64 + 1] =
    "aaaaaaa" "c" "eeee" "iiii" "dn" "ooooo" "*" "o" "uuuu" "y" "t" "s"
    "aaaaaaa" "c" "eeee" "iiii" "dn" "ooooo" "*" "o" "uuuu" "y" "t" "y";
/* U+0100..U+017F, as sortkey.c */
static const char latin_ext_a[128 + 1] =
    "aaaaaa" "cccccccc" "dddd" "eeeeeeeeee" "gggggggg" "hhhh"
    "iiiiiiiiii" "ii" "jj" "kkk" "llllllllll" "nnnnnnnnn"
    "oooooo" "oo" "rrrrrr" "ssssssss" "tttttt" "uuuuuuuuuuuu"
    "ww" "yyy" "zzzzzz" "s";
typedef char srch_latin1_check[sizeof(latin1) == 65 ? 1 : -1];
typedef char srch_ext_a_check[sizeof(latin_ext_a) == 129 ? 1 : -1];

#define SEP ' '

/* Folds one character at *p (advancing *p) into out[0..1]: returns the
 * number of folded bytes (1 or 2), with out[0] == SEP for a separator,
 * or 0 at the end of the string. */
static int fold_char(const unsigned char **p, unsigned char out[4])
{
    const unsigned char *s = *p;
    unsigned c = s[0];

    if (c == 0)
        return 0;
    if (c < 0x80)
    {
        *p = s + 1;
        if (c >= 'A' && c <= 'Z')
            c += 'a' - 'A';
        else if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')))
            c = SEP;
        out[0] = c;
        return 1;
    }
    if (c >= 0xc2 && c <= 0xc5 && (s[1] & 0xc0) == 0x80)
    {
        unsigned u = ((c & 0x1f) << 6) | (s[1] & 0x3f);    /* U+0080.. */
        const char *two = NULL;
        char f;

        *p = s + 2;
        switch (u)
        {
        case 0xc6: case 0xe6:   two = "ae"; break;
        case 0xdf:              two = "ss"; break;
        case 0xde: case 0xfe:   two = "th"; break;
        case 0x132: case 0x133: two = "ij"; break;
        case 0x152: case 0x153: two = "oe"; break;
        default: break;
        }
        if (two)
        {
            out[0] = two[0];
            out[1] = two[1];
            return 2;
        }
        if (u < 0xc0)
            f = SEP;                    /* Latin-1 symbols, no-break space */
        else if (u < 0x100)
            f = latin1[u - 0xc0];
        else
            f = latin_ext_a[u - 0x100];
        out[0] = f == '*' ? SEP : f;
        return 1;
    }
    /* #47: Latin Extended-B, Greek, Cyrillic (U+0180..U+052F) and Latin
     * Extended Additional (U+1E00..U+1EFF, Vietnamese): case folded by the
     * library's rules (sortkey.h), accents kept; a word character */
    if (c >= 0xc6 && c <= 0xd4 && (s[1] & 0xc0) == 0x80)
    {
        unsigned u = ui_casefold_char(((c & 0x1f) << 6) | (s[1] & 0x3f));

        if (u >= 0x80 && u < 0x800)
        {
            *p = s + 2;
            out[0] = (unsigned char)(0xc0 | (u >> 6));
            out[1] = (unsigned char)(0x80 | (u & 0x3f));
            return 2;
        }
    }
    if (c == 0xe1 && s[1] >= 0xb8 && s[1] <= 0xbb && (s[2] & 0xc0) == 0x80)
    {
        unsigned u = ui_casefold_char(0x1000 | ((s[1] & 0x3f) << 6)
                                      | (s[2] & 0x3f));

        *p = s + 3;
        if (u == 0xdf)                  /* capital sharp s: as sharp s */
        {
            out[0] = out[1] = 's';
            return 2;
        }
        out[0] = 0xe1;
        out[1] = (unsigned char)(0x80 | ((u >> 6) & 0x3f));
        out[2] = (unsigned char)(0x80 | (u & 0x3f));
        return 3;
    }
    /* any other byte (another script's lead or continuation byte, or a
     * malformed one) is kept: a word character */
    *p = s + 1;
    out[0] = c;
    return 1;
}

int srch_fold_query(const char *utf8, struct srch_query *q)
{
    const unsigned char *p = (const unsigned char *)utf8;
    unsigned char f[4];
    int n = 0, k;
    bool space = false;

    q->folded[0] = '\0';
    q->len = 0;
    if (!utf8)
        return 0;
    while ((k = fold_char(&p, f)) > 0)
    {
        const unsigned char *next = p;
        int extra = 0;

        if (f[0] == SEP)
        {
            space = n > 0;              /* collapse; drop leading space */
            continue;
        }
        /* keep a multi-byte character whole: its continuation bytes are
         * folded one by one, so count them in before the bound check */
        while (k == 1 && f[0] >= 0xc0 && (*next & 0xc0) == 0x80)
            next++, extra++;
        if (n + (space ? 1 : 0) + k + extra > SRCH_MAX_Q)
            break;                      /* truncated, never mid-character */
        if (space)
            q->folded[n++] = SEP;
        space = false;
        memcpy(q->folded + n, f, k);
        n += k;
        if (extra)
        {
            memcpy(q->folded + n, p, extra);
            n += extra;
            p = next;
        }
    }
    q->folded[n] = '\0';
    q->len = n;
    return n;
}

bool srch_extends(const struct srch_query *prev, const struct srch_query *q)
{
    /* appending to the last word, or adding words, can only shrink the
     * set; an empty query matches nothing, so nothing to refine */
    return prev->len > 0 && q->len >= prev->len
        && !memcmp(prev->folded, q->folded, prev->len);
}

/* Whether the character before 'at' (in 'hay') is a separator: true at
 * the start. Looks one or two bytes back (a two-byte Latin character). */
static bool word_start(const unsigned char *hay, const unsigned char *at)
{
    const unsigned char *b;
    unsigned char f[4];

    if (at == hay)
        return true;
    b = at - 1;
    /* the lead byte of the character before (2 or 3 bytes) */
    while (b > hay && (*b & 0xc0) == 0x80 && at - b < 3)
        b--;
    return fold_char(&b, f) > 0 && f[0] == SEP;
}

/* Whether the folded text at 'at' starts with word w (len bytes, no
 * separator in it). *end gets the position after the matched text. */
static bool word_at(const unsigned char *at, const unsigned char *w, int len,
                    const unsigned char **end)
{
    unsigned char f[4];
    int i = 0, k, j;

    while (i < len)
    {
        k = fold_char(&at, f);
        if (k == 0)
            return false;
        for (j = 0; j < k && i < len; j++, i++)
            if (f[j] != w[i])
                return false;
    }
    *end = at;
    return true;
}

/* Rest of the query (words after the first) from 'from' on. */
static bool rest_at(const unsigned char *hay, const unsigned char *from,
                    const unsigned char *w, int len)
{
    while (len > 0)
    {
        int wl = 0;

        while (w[0] == SEP)
            w++, len--;
        while (wl < len && w[wl] != SEP)
            wl++;
        for (;;)
        {
            const unsigned char *end, *p = from;
            unsigned char f[4];

            if (!*from)
                return false;
            if (word_start(hay, from) && word_at(from, w, wl, &end))
            {
                from = end;
                break;
            }
            fold_char(&p, f);
            from = p;
        }
        w += wl;
        len -= wl;
    }
    return true;
}

/* The query's first byte (folded) -> the bytes that stop the scan */
static uint8_t stop[256];
static int stop_for = -1;               /* the byte it is built for */

static void stop_build(unsigned c0)
{
    unsigned i;

    memset(stop, 0, sizeof(stop));
    stop[0] = 1;
    stop[c0] = 1;
    if (c0 >= 'a' && c0 <= 'z')
        stop[c0 - 'a' + 'A'] = 1;
    for (i = 0xc2; i <= 0xd4; i++)      /* Latin letters fold to ASCII; */
        stop[i] = 1;                    /* #47: other cases (sortkey.h) */
    stop[0xe1] = 1;                     /* Vietnamese */
    stop_for = c0;
}

/* The slow path, at a stop byte: a word start that begins the query. */
static bool __attribute__((noinline))
match_at(const unsigned char *hay, const unsigned char *at,
         const struct srch_query *q)
{
    const unsigned char *w = (const unsigned char *)q->folded;
    const unsigned char *end;
    int wl = 0;

    while (wl < q->len && w[wl] != SEP)
        wl++;
    return word_start(hay, at) && word_at(at, w, wl, &end)
        && rest_at(hay, end, w + wl, q->len - wl);
}

bool srch_match(const char *hay, const struct srch_query *q)
{
    const unsigned char *p = (const unsigned char *)hay;
    const uint8_t *t = stop;
    unsigned c0 = (unsigned char)q->folded[0];

    if ((int)c0 != stop_for)
    {
        if (!c0)
            return false;               /* an empty query matches nothing */
        stop_build(c0);
    }
    if (!p)
        return false;
    for (;;)
    {
        /* the hot loop, unrolled: ldrb, ldrb, cmp, bne per byte; the
         * offset of the stop is added once, on the way out */
        for (;; p += 4)
        {
            if (t[p[0]])
                goto at0;
            if (t[p[1]])
                goto at1;
            if (t[p[2]])
                goto at2;
            if (t[p[3]])
                goto at3;
        }
at3:    p++;
at2:    p++;
at1:    p++;
at0:
        if (!*p)
            return false;
        /* inside an ASCII word: not a start (most stops, no call) */
        if (p != (const unsigned char *)hay)
        {
            unsigned b = p[-1];

            if ((b | 0x20) - 'a' < 26u || b - '0' < 10u)
            {
                p++;
                continue;
            }
        }
        /* a stop inside a two-byte character continues after it */
        if (match_at((const unsigned char *)hay, p, q))
            return true;
        p++;
    }
}
