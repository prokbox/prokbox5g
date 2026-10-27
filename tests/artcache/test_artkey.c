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

/* Host test: artcache_key() against the golden keys (tests/artcache/keys.txt,
 * made by gen_keys.py, the reference the Mac tool must match) and norm().
 * Usage: test_artkey keys.txt */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include "artkey.h"

static int fails;

#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", \
    __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* Unescapes one field in place; returns NULL for \N. */
static char *field(char *s)
{
    char *r = s, *w = s;

    if (!strcmp(s, "\\N"))
        return NULL;
    while (*r)
    {
        if (*r == '\\' && r[1])
        {
            r++;
            *w++ = *r == 't' ? '\t' : *r == 'n' ? '\n' : *r == 'r' ? '\r' : *r;
            r++;
        }
        else
            *w++ = *r++;
    }
    *w = '\0';
    return s;
}

static void golden(const char *path)
{
    char line[2048];
    FILE *f = fopen(path, "r");
    int n = 0;

    CHECK(f != NULL, "cannot open %s", path);
    if (!f)
        return;
    while (fgets(line, sizeof(line), f))
    {
        char *fl[5], *p = line;
        int i;
        uint64_t want, got;

        line[strcspn(line, "\n")] = '\0';
        if (line[0] == '#' || !line[0])
            continue;
        for (i = 0; i < 5; i++)
        {
            fl[i] = p;
            p = strchr(p, '\t');
            if (!p && i < 4)
                break;
            if (p)
                *p++ = '\0';
        }
        CHECK(i == 5, "bad line %d", n + 1);
        if (i != 5)
            continue;
        want = strtoull(fl[4], NULL, 16);
        for (i = 0; i < 4; i++)
            fl[i] = field(fl[i]);
        got = artcache_key(fl[0], fl[1], fl[2], fl[3]);
        CHECK(got == want, "key %016" PRIx64 " != golden %016" PRIx64
              " (aa=%s ar=%s al=%s p=%s)", got, want,
              fl[0] ? fl[0] : "NULL", fl[1] ? fl[1] : "NULL",
              fl[2] ? fl[2] : "NULL", fl[3] ? fl[3] : "NULL");
        n++;
    }
    fclose(f);
    CHECK(n >= 20, "only %d golden keys", n);
    printf("   %d golden keys\n", n);
}

static void norms(void)
{
    static const struct { const char *in, *out; } t[] = {
        { "", "" }, { "   ", "" }, { "A", "a" }, { "  Hello   World  ", "hello world" },
        { "\tTab\nNew\rLine\v\f", "tab new line" }, { "ÄBC", "äbc" },
        { "STRAẞE", "straße" }, { "Straße", "straße" }, { "СЕВЕР", "север" },
        { "\xc3", "\xc3" }, { "Ä\xff", "ä\xff" },
        { "Mixed CASE 123 !?", "mixed case 123 !?" }, { "a\x1f" "b", "a\x1f" "b" },
    };
    char buf[64];
    size_t i, n;

    for (i = 0; i < sizeof(t) / sizeof(t[0]); i++)
    {
        n = artkey_norm(t[i].in, buf, sizeof(buf));
        CHECK(!strcmp(buf, t[i].out) && n == strlen(t[i].out),
              "norm(\"%s\") = \"%s\"", t[i].in, buf);
    }
    /* truncation keeps the full length and terminates */
    n = artkey_norm("ABCDEFGH", buf, 4);
    CHECK(n == 8 && !strcmp(buf, "abc"), "truncated norm \"%s\" %zu", buf, n);
    CHECK(artkey_norm(NULL, buf, sizeof(buf)) == 0 && !buf[0], "NULL norm");
    /* FNV-1a 32 reference values */
    CHECK(artkey_fnv32("") == 0x811c9dc5u, "fnv32 empty");
    CHECK(artkey_fnv32("a") == 0xe40c292cu, "fnv32 a");
    CHECK(artkey_fnv32("foobar") == 0xbf9cf968u, "fnv32 foobar");
    /* keys are never 0 for a tagged album, and differ across fields */
    CHECK(artcache_key("x", NULL, "y", NULL) != 0, "zero key");
    CHECK(artcache_key("ab", NULL, "c", NULL) != artcache_key("a", NULL, "bc", NULL),
          "separator must split fields");
}

int main(int argc, char **argv)
{
    norms();
    golden(argc > 1 ? argv[1] : "tests/artcache/keys.txt");
    if (fails)
    {
        printf("%d failure(s)\n", fails);
        return 1;
    }
    return 0;
}
