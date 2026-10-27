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
 * iPod UI shell: album art cache keys (see artkey.h)
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
#include <stdbool.h>
#include <string.h>
#ifdef SIMULATOR
#include <stdlib.h>
#endif
#include "artkey.h"
#include "sortkey.h"     /* ui_casefold_char(): the library's case folding */

#define FNV64_OFFSET 0xcbf29ce484222325ULL
#define FNV64_PRIME  0x100000001b3ULL
#define FNV32_OFFSET 0x811c9dc5u
#define FNV32_PRIME  0x01000193u

static bool is_ws(unsigned char c)
{
    return c == ' ' || (c >= '\t' && c <= '\r');
}

static unsigned char fold(unsigned char c)
{
    return (c >= 'A' && c <= 'Z') ? c + ('a' - 'A') : c;
}

/* The normalizer as a stream: calls emit() for each output byte of
 * norm(s[0..len)). */
struct sink {
    uint64_t h;         /* FNV-1a 64 state */
    char *out;          /* artkey_norm(): copy here, NULL when hashing */
    size_t n, cap;
    bool ascii;         /* the pre-#47 norm(): ASCII A-Z only */
};

static void put(struct sink *k, unsigned char c)
{
    if (k->out)
    {
        if (k->n + 1 < k->cap)
            k->out[k->n] = c;
    }
    else
    {
        k->h ^= c;
        k->h *= FNV64_PRIME;
    }
    k->n++;
}

/* The UTF-8 character at s[i] (n bytes, 2 or 3) as a code point, or 0
 * if it is not a well-formed one (its bytes then pass through). */
static uint32_t utf8_at(const unsigned char *s, size_t i, size_t len, int *n)
{
    unsigned char c = s[i];

    if (c >= 0xc2 && c <= 0xdf && i + 1 < len && (s[i + 1] & 0xc0) == 0x80)
    {
        *n = 2;
        return ((uint32_t)(c & 0x1f) << 6) | (s[i + 1] & 0x3f);
    }
    if ((c & 0xf0) == 0xe0 && i + 2 < len && (s[i + 1] & 0xc0) == 0x80
        && (s[i + 2] & 0xc0) == 0x80)
    {
        uint32_t u = ((uint32_t)(c & 0x0f) << 12)
                   | ((uint32_t)(s[i + 1] & 0x3f) << 6) | (s[i + 2] & 0x3f);

        *n = 3;
        return u >= 0x800 ? u : 0;
    }
    return 0;
}

static void put_cp(struct sink *k, uint32_t u)
{
    if (u < 0x80)
        put(k, (unsigned char)u);
    else if (u < 0x800)
    {
        put(k, (unsigned char)(0xc0 | (u >> 6)));
        put(k, (unsigned char)(0x80 | (u & 0x3f)));
    }
    else
    {
        put(k, (unsigned char)(0xe0 | (u >> 12)));
        put(k, (unsigned char)(0x80 | ((u >> 6) & 0x3f)));
        put(k, (unsigned char)(0x80 | (u & 0x3f)));
    }
}

static void norm_into(struct sink *k, const char *s, size_t len)
{
    const unsigned char *b = (const unsigned char *)s;
    bool any = false, gap = false;
    size_t i;

    for (i = 0; i < len; i++)
    {
        unsigned char c = b[i];
        uint32_t u;
        int n;

        if (is_ws(c))
        {
            gap = any;
            continue;
        }
        if (gap)
            put(k, ' ');
        gap = false;
        any = true;
        if (c < 0x80 || k->ascii)
        {
            put(k, fold(c));
            continue;
        }
        /* #47: one case folding with the library's grouping (sortkey.h);
         * a key changes only where a non-ASCII capital is folded */
        u = utf8_at(b, i, len, &n);
        if (!u || ui_casefold_char(u) == u)
        {
            put(k, c);
            continue;
        }
        put_cp(k, ui_casefold_char(u));
        i += n - 1;
    }
}

/* True if norm(s) is empty or equals norm("<Untagged>"). */
static bool untagged(const char *s)
{
    static const char tag[] = ARTKEY_UNTAGGED;
    size_t i = 0, n;

    if (!s)
        return true;
    while (is_ws((unsigned char)*s))
        s++;
    n = strlen(s);
    while (n > 0 && is_ws((unsigned char)s[n - 1]))
        n--;
    if (n == 0)
        return true;
    if (n != sizeof(tag) - 1)
        return false;
    for (i = 0; i < n; i++)
        if (fold((unsigned char)s[i]) != fold((unsigned char)tag[i]))
            return false;
    return true;
}

static artcache_key_t key_of(const char *albumartist, const char *artist,
                             const char *album, const char *first_track_path,
                             bool ascii)
{
    struct sink k = { FNV64_OFFSET, NULL, 0, 0, ascii };

#ifdef SIMULATOR
    /* tests/perf/ab_oldkey_check.sh: a boot as the firmware before #47 */
    if (getenv("PROKBOX_SIM_ASCII_ARTKEY"))
        k.ascii = true;
#endif

    if (untagged(album))
    {
        const char *slash;

        if (!first_track_path)
            return 0;
        slash = strrchr(first_track_path, '/');
        put(&k, 'd'); put(&k, 'i'); put(&k, 'r');
        put(&k, ARTKEY_SEP);
        if (slash)
            norm_into(&k, first_track_path, slash - first_track_path);
    }
    else
    {
        const char *a = !untagged(albumartist) ? albumartist
                      : !untagged(artist) ? artist : NULL;

        if (a)
            norm_into(&k, a, strlen(a));
        put(&k, ARTKEY_SEP);
        norm_into(&k, album, strlen(album));
    }
    return k.h ? k.h : 1;
}

artcache_key_t artcache_key(const char *albumartist, const char *artist,
                            const char *album, const char *first_track_path)
{
    return key_of(albumartist, artist, album, first_track_path, false);
}

artcache_key_t artcache_key_ascii(const char *albumartist, const char *artist,
                                  const char *album,
                                  const char *first_track_path)
{
    return key_of(albumartist, artist, album, first_track_path, true);
}

size_t artkey_norm(const char *s, char *out, size_t outsize)
{
    struct sink k = { 0, out, 0, outsize, false };

    if (s)
        norm_into(&k, s, strlen(s));
    if (outsize)
        out[k.n < outsize ? k.n : outsize - 1] = '\0';
    return k.n;
}

uint32_t artkey_fnv32(const char *s)
{
    uint32_t h = FNV32_OFFSET;

    while (s && *s)
    {
        h ^= (unsigned char)*s++;
        h *= FNV32_PRIME;
    }
    return h;
}

artcache_key_t artcache_key_chapter(artcache_key_t book, int chapter)
{
    unsigned char b[15];
    uint64_t h = FNV64_OFFSET;
    uint32_t c = (uint32_t)chapter;
    int i;

    if (book == 0 || chapter < 0)
        return 0;
    for (i = 0; i < 8; i++)
        b[i] = (unsigned char)(book >> (8 * i));
    b[8] = (unsigned char)ARTKEY_SEP;
    b[9] = 'c';
    b[10] = 'h';
    for (i = 0; i < 4; i++)
        b[11 + i] = (unsigned char)(c >> (8 * i));
    for (i = 0; i < (int)sizeof(b); i++)
    {
        h ^= b[i];
        h *= FNV64_PRIME;
    }
    return h ? h : 1;
}
