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
/* 7G list collation keys. Pure C: stdint/stdbool/string only. */
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "sortkey.h"

/* U+00C0..U+00FF folded to ASCII. '*' marks a symbol (x, division sign);
 * upper case is folded to lower case below. Two-letter folds are in
 * fold2(). */
static const char latin1[64 + 1] =
    "aaaaaaa" "c" "eeee" "iiii" "dn" "ooooo" "*" "o" "uuuu" "y" "t" "s"
    "aaaaaaa" "c" "eeee" "iiii" "dn" "ooooo" "*" "o" "uuuu" "y" "t" "y";

/* U+0100..U+017F (Latin Extended-A), one letter each. */
static const char latin_ext_a[128 + 1] =
    "aaaaaa" "cccccccc" "dddd" "eeeeeeeeee" "gggggggg" "hhhh"
    "iiiiiiiiii" "ii" "jj" "kkk" "llllllllll" "nnnnnnnnn"
    "oooooo" "oo" "rrrrrr" "ssssssss" "tttttt" "uuuuuuuuuuuu"
    "ww" "yyy" "zzzzzz" "s";

typedef char latin1_size_check[sizeof(latin1) == 65 ? 1 : -1];
typedef char latin_ext_a_size_check[sizeof(latin_ext_a) == 129 ? 1 : -1];

/* Two-letter folds; returns NULL for everything else. */
static const char *fold2(uint32_t c)
{
    switch (c)
    {
    case 0xc6: case 0xe6:   return "ae";
    case 0xdf:              return "ss";
    case 0xde: case 0xfe:   return "th";
    case 0x132: case 0x133: return "ij";
    case 0x152: case 0x153: return "oe";
    default:                return NULL;
    }
}

/* Decodes one UTF-8 character at *p and advances *p. Malformed bytes come
 * back one by one, unchanged, with *raw set (they keep their byte value). */
static uint32_t next_char(const unsigned char **p, bool *raw)
{
    const unsigned char *s = *p;
    uint32_t c = s[0];
    int n, i;

    *raw = false;
    if (c < 0x80)
    {
        *p = s + 1;
        return c;
    }
    if ((c & 0xe0) == 0xc0)
        n = 1, c &= 0x1f;
    else if ((c & 0xf0) == 0xe0)
        n = 2, c &= 0x0f;
    else if ((c & 0xf8) == 0xf0)
        n = 3, c &= 0x07;
    else
        n = -1;

    for (i = 0; i < n; i++)
    {
        if ((s[1 + i] & 0xc0) != 0x80)
            break;
        c = (c << 6) | (s[1 + i] & 0x3f);
    }
    if (n < 0 || i < n)
    {
        *raw = true;
        *p = s + 1;
        return s[0];
    }
    *p = s + 1 + n;
    return c;
}

/* -- simple case folding (#47) ------------------------------------------
 * Upper case to lower case for U+0041..U+024F (Basic Latin, Latin-1,
 * Latin Extended-A and -B: the fonts' range), Greek (U+0370..U+03FF),
 * Cyrillic (U+0400..U+052F) and Latin Extended Additional (U+1E00..U+1EFF,
 * Vietnamese; capital sharp s U+1E9E folds to U+00DF); everything else
 * unchanged. Unicode simple
 * case folding (CaseFolding.txt, status C and S), except the Turkish
 * dotted / dotless i (U+0130, U+0131), which stay as they are. sharp s is
 * the one full fold ("ss"), made by the callers (fold2() for keys,
 * cf_next() for comparisons). Accents are kept: folding case never merges
 * e and e-acute (ui_sortkey() folds accents for the order only). */

/* Ranges of alternating case pairs: [first, last] with the upper case at
 * the first's parity, its lower case the next code point. */
static const uint16_t cf_alt[][2] = {
    { 0x0100, 0x012f }, { 0x0132, 0x0137 }, { 0x0139, 0x0148 },
    { 0x014a, 0x0177 }, { 0x0179, 0x017e }, { 0x01cd, 0x01dc },
    { 0x01de, 0x01ef }, { 0x01f8, 0x021f }, { 0x0222, 0x0233 },
    { 0x0246, 0x024f }, { 0x03d8, 0x03ef }, { 0x0460, 0x0481 },
    { 0x048a, 0x04bf }, { 0x04c1, 0x04ce }, { 0x04d0, 0x052f },
    { 0x1e00, 0x1e95 }, { 0x1ea0, 0x1eff },     /* Vietnamese and more */
};

/* Single pairs (upper, lower) outside those ranges and offsets. */
static const uint16_t cf_one[][2] = {
    { 0x00b5, 0x03bc }, { 0x0178, 0x00ff }, { 0x017f, 0x0073 },
    { 0x0181, 0x0253 }, { 0x0182, 0x0183 }, { 0x0184, 0x0185 },
    { 0x0186, 0x0254 }, { 0x0187, 0x0188 }, { 0x0189, 0x0256 },
    { 0x018a, 0x0257 }, { 0x018b, 0x018c }, { 0x018e, 0x01dd },
    { 0x018f, 0x0259 }, { 0x0190, 0x025b }, { 0x0191, 0x0192 },
    { 0x0193, 0x0260 }, { 0x0194, 0x0263 }, { 0x0196, 0x0269 },
    { 0x0197, 0x0268 }, { 0x0198, 0x0199 }, { 0x019c, 0x026f },
    { 0x019d, 0x0272 }, { 0x019f, 0x0275 }, { 0x01a0, 0x01a1 },
    { 0x01a2, 0x01a3 }, { 0x01a4, 0x01a5 }, { 0x01a6, 0x0280 },
    { 0x01a7, 0x01a8 }, { 0x01a9, 0x0283 }, { 0x01ac, 0x01ad },
    { 0x01ae, 0x0288 }, { 0x01af, 0x01b0 }, { 0x01b1, 0x028a },
    { 0x01b2, 0x028b }, { 0x01b3, 0x01b4 }, { 0x01b5, 0x01b6 },
    { 0x01b7, 0x0292 }, { 0x01b8, 0x01b9 }, { 0x01bc, 0x01bd },
    { 0x01c4, 0x01c6 }, { 0x01c5, 0x01c6 }, { 0x01c7, 0x01c9 },
    { 0x01c8, 0x01c9 }, { 0x01ca, 0x01cc }, { 0x01cb, 0x01cc },
    { 0x01f1, 0x01f3 }, { 0x01f2, 0x01f3 }, { 0x01f4, 0x01f5 },
    { 0x01f6, 0x0195 }, { 0x01f7, 0x01bf }, { 0x0220, 0x019e },
    { 0x023a, 0x2c65 }, { 0x023b, 0x023c }, { 0x023d, 0x019a },
    { 0x023e, 0x2c66 }, { 0x0241, 0x0242 },
    { 0x0243, 0x0180 }, { 0x0244, 0x0289 }, { 0x0245, 0x028c },
    { 0x0376, 0x0377 }, { 0x037f, 0x03f3 }, { 0x0386, 0x03ac },
    { 0x038c, 0x03cc }, { 0x038e, 0x03cd }, { 0x038f, 0x03ce },
    { 0x03c2, 0x03c3 }, { 0x03cf, 0x03d7 }, { 0x03f4, 0x03b8 },
    { 0x03f7, 0x03f8 }, { 0x03f9, 0x03f2 }, { 0x03fa, 0x03fb },
    { 0x03fd, 0x037b }, { 0x03fe, 0x037c }, { 0x03ff, 0x037d },
    { 0x04c0, 0x04cf }, { 0x0345, 0x03b9 }, { 0x1e9b, 0x1e61 },
    { 0x1e9e, 0x00df },
};

uint32_t ui_casefold_char(uint32_t c)
{
    unsigned i;

    if (c < 0x41)
        return c;
    if (c <= 0x5a)
        return c + 0x20;
    if (c < 0xb5 || (c >= 0xdf && c <= 0xff))
        return c;                       /* the common lower case: fast */
    if (c >= 0xc0 && c <= 0xde && c != 0xd7)
        return c + 0x20;
    if ((c >= 0x0430 && c <= 0x045f) || (c >= 0x03ac && c <= 0x03c1))
        return c;                       /* lower case Cyrillic, Greek */
    if ((c > 0x052f && c < 0x1e00) || c > 0x1eff)
        return c;
    if ((c >= 0x0388 && c <= 0x038a))
        return c + 0x25;
    if ((c >= 0x0391 && c <= 0x03a1) || (c >= 0x03a3 && c <= 0x03ab)
        || (c >= 0x0410 && c <= 0x042f))
        return c + 0x20;
    if (c >= 0x0400 && c <= 0x040f)
        return c + 0x50;
    for (i = 0; i < sizeof(cf_alt) / sizeof(cf_alt[0]); i++)
        if (c >= cf_alt[i][0] && c <= cf_alt[i][1])
            return ((c ^ cf_alt[i][0]) & 1) ? c : c + 1;
    for (i = 0; i < sizeof(cf_one) / sizeof(cf_one[0]); i++)
        if (c == cf_one[i][0])
            return cf_one[i][1];
    return c;
}

/* UTF-8 of c (< 0x10000 here) into out; returns its length. */
static int utf8_put(uint32_t c, char *out)
{
    if (c < 0x80)
    {
        out[0] = (char)c;
        return 1;
    }
    if (c < 0x800)
    {
        out[0] = (char)(0xc0 | (c >> 6));
        out[1] = (char)(0x80 | (c & 0x3f));
        return 2;
    }
    out[0] = (char)(0xe0 | (c >> 12));
    out[1] = (char)(0x80 | ((c >> 6) & 0x3f));
    out[2] = (char)(0x80 | (c & 0x3f));
    return 3;
}

/* Appends the folded form of one character to 'out' (at most 4 bytes).
 * Returns the number of bytes, 0 to drop the character. 'src'/'len' are
 * the original UTF-8 bytes, kept for characters that do not fold. */
static int fold(uint32_t c, bool raw, const unsigned char *src, int len,
                char *out)
{
    const char *two;

    if (raw)
    {
        out[0] = (char)c;
        return 1;
    }
    if (c < 0x20 || c == 0x7f)
        return 0;                       /* control characters */
    if (c >= 'A' && c <= 'Z')
    {
        out[0] = (char)(c - 'A' + 'a');
        return 1;
    }
    if (c > 'z' && c < 0x80)
    {
        out[0] = ':';                   /* { | } ~: below HASH_GROUP */
        return 1;
    }
    if (c < 0x80)
    {
        out[0] = (char)c;
        return 1;
    }
    if (c == 0xa0 || c == 0x2007 || c == 0x202f)
    {
        out[0] = ' ';                   /* no-break spaces */
        return 1;
    }
    two = fold2(c);
    if (two)
    {
        out[0] = two[0];
        out[1] = two[1];
        return 2;
    }
    if (c >= 0xc0 && c <= 0xff)
    {
        out[0] = latin1[c - 0xc0];
        return 1;
    }
    if (c < 0xc0)
    {
        out[0] = '*';                   /* Latin-1 symbols: the '#' group */
        return 1;
    }
    if (c >= 0x100 && c <= 0x17f)
    {
        out[0] = latin_ext_a[c - 0x100];
        return 1;
    }
    memcpy(out, src, len);              /* sorts after 'z' */
    return len;
}

/* Leads every key whose name does not start with a letter: the '#' group
 * sorts after 'z' (iPod classic 2.0.5 user guide: numbers and symbols
 * after Z, as "#"). Above every letter; ASCII symbols inside a key are
 * kept below it. */
#define HASH_GROUP  '{'

/* Drops the key's last character (all of its UTF-8 bytes) and a space
 * left in front of it. Returns the new length. */
static int drop_last(char *key, int n)
{
    n--;
    while (n > 0 && ((unsigned char)key[n] & 0xc0) == 0x80)
        n--;
    if (n > 0 && key[n - 1] == ' ')
        n--;
    return n;
}

static bool is_space(uint32_t c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r'
        || c == 0xa0 || c == 0x2007 || c == 0x202f;
}

static int sortkey_impl(const char *name, char *key, int keylen, bool cf)
{
    const unsigned char *p = (const unsigned char *)name;
    int n = 0;
    bool space = false;

    if (!key || keylen <= 0)
        return 0;
    key[0] = '\0';
    if (!name)
        return 0;

    while (*p == ' ' || *p == '\t')
        p++;
    /* one leading "the ", if a name follows */
    if ((p[0] | 0x20) == 't' && (p[1] | 0x20) == 'h' && (p[2] | 0x20) == 'e'
        && p[3] == ' ')
    {
        const unsigned char *q = p + 4;
        while (*q == ' ')
            q++;
        if (*q)
            p = q;
    }

    while (*p)
    {
        const unsigned char *start = p;
        char buf[4];
        bool raw;
        uint32_t c = next_char(&p, &raw);
        char low[4];
        int len;

        if (!raw && is_space(c))
        {
            space = n > 0;              /* drop leading space */
            continue;
        }
        if (cf && !raw && ui_casefold_char(c) != c
            && (c >= 0x180 || ui_casefold_char(c) >= 0x180))
        {
            /* outside Latin-1 / Extended-A the key keeps the bytes:
             * those of the lower case (fold() lowers the rest itself) */
            c = ui_casefold_char(c);
            len = fold(c, false, (const unsigned char *)low,
                       utf8_put(c, low), buf);
        }
        else
            len = fold(c, raw, start, (int)(p - start), buf);
        if (len == 0)
            continue;
        if (n + len + (space ? 1 : 0) > keylen - 1)
            break;                      /* truncated, never mid-character */
        if (space)
            key[n++] = ' ';
        space = false;
        memcpy(key + n, buf, len);
        n += len;
    }
    if (n > 0 && !(key[0] >= 'a' && key[0] <= 'z'))
    {
        if (n + 1 > keylen - 1)
            n = drop_last(key, n);
        if (keylen > 1)
        {
            memmove(key + 1, key, n);
            key[0] = HASH_GROUP;
            n++;
        }
    }
    key[n] = '\0';
    return n;
}

int ui_sortkey(const char *name, char *key, int keylen)
{
    return sortkey_impl(name, key, keylen, false);
}

int ui_groupkey(const char *name, char *key, int keylen)
{
    return sortkey_impl(name, key, keylen, true);
}

/* One comparison unit of a name: a case-folded code point, a malformed
 * byte (above every code point), or -1 at the end. sharp s gives two
 * 's' (s->pend holds the second). */
struct cf_stream {
    const unsigned char *p;
    int32_t pend;
};

static int32_t cf_next(struct cf_stream *s)
{
    uint32_t c;
    bool raw;

    if (s->pend >= 0)
    {
        c = s->pend;
        s->pend = -1;
        return c;
    }
    if (!*s->p)
        return -1;
    c = next_char(&s->p, &raw);
    if (raw)
        return 0x200000 + c;
    c = ui_casefold_char(c);
    if (c == 0xdf)
    {
        s->pend = 's';
        return 's';
    }
    return c;
}

int ui_casefold_cmp(const char *a, const char *b)
{
    struct cf_stream x = { (const unsigned char *)(a ? a : ""), -1 };
    struct cf_stream y = { (const unsigned char *)(b ? b : ""), -1 };

    for (;;)
    {
        int32_t ca = cf_next(&x), cb = cf_next(&y);

        if (ca != cb)
            return ca < cb ? -1 : 1;
        if (ca < 0)
            return 0;
    }
}

uint32_t ui_casefold_hash(const char *name)
{
    struct cf_stream x = { (const unsigned char *)(name ? name : ""), -1 };
    uint32_t h = 0;
    int32_t c;

    while ((c = cf_next(&x)) >= 0)
        h = h * 31 + (uint32_t)c;
    return h;
}

char ui_sortletter(const char *name)
{
    char key[8];

    ui_sortkey(name, key, sizeof(key));
    if (key[0] >= 'a' && key[0] <= 'z')
        return (char)(key[0] - 'a' + 'A');
    return '#';
}
