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

/* #230: the library open timing line's rate limit and length
 * (apps/ipodui/lib_openlog.h). */
#include <stdio.h>
#include <string.h>
#include "ipodui/lib_openlog.h"

static int fails, checks;

#define CHECK(c) do { checks++; if (!(c)) { \
    fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #c); \
    fails++; } } while (0)

/* many quick opens: only the first of each category and kind logs */
static void test_quick(void)
{
    struct lib_openlog s;
    unsigned cat, kind;
    int i, logged = 0;

    memset(&s, 0, sizeof(s));
    for (i = 0; i < 100; i++)
        for (cat = 0; cat < 14; cat++)
            for (kind = 0; kind < LOL_KINDS; kind++)
                logged += lib_openlog_due(&s, cat, kind, i % 50);
    CHECK(logged == 14 * LOL_KINDS);
    /* a second quick one of a seen kind: nothing */
    CHECK(!lib_openlog_due(&s, 1, LOL_COMPOSER, 0));
    CHECK(!lib_openlog_due(&s, 1, LOL_COMPOSER, LIB_OPENLOG_MS - 1));
}

/* a slow one always logs, seen or not */
static void test_slow(void)
{
    struct lib_openlog s;

    memset(&s, 0, sizeof(s));
    CHECK(lib_openlog_due(&s, 1, LOL_GENRE, 3));        /* first */
    CHECK(!lib_openlog_due(&s, 1, LOL_GENRE, 3));
    CHECK(lib_openlog_due(&s, 1, LOL_GENRE, LIB_OPENLOG_MS));
    CHECK(lib_openlog_due(&s, 1, LOL_GENRE, 1004));
    CHECK(!lib_openlog_due(&s, 1, LOL_GENRE, 49));
    /* the first of another kind of the same category */
    CHECK(lib_openlog_due(&s, 1, LOL_COMPOSER, 0));
    /* out of range: only when slow */
    CHECK(!lib_openlog_due(&s, 200, LOL_ALL, 10));
    CHECK(lib_openlog_due(&s, 200, LOL_ALL, 60));
    CHECK(lib_openlog_due(&s, 2, 99, 0));               /* -> OTHER, first */
    CHECK(!lib_openlog_due(&s, 2, LOL_OTHER, 0));
}

/* the widest line fits the ring's ~55 characters, "lib " included */
static void test_length(void)
{
    char b[128];
    int n, widest = 0;
    unsigned k;

    for (k = 0; k < LOL_KINDS; k++)
    {
        n = snprintf(b, sizeof(b), "lib " LIB_OPENLOG_FMT, 4294967295ul,
                     -2147483647 - 1, "compilations", lib_openlog_kinds[k],
                     "slice");
        CHECK(n <= LIB_OPENLOG_MAX);
        if (n > widest)
            widest = n;
    }
    n = snprintf(b, sizeof(b), "lib " LIB_OPENLOG_FMT, 1004ul, 62, "albums",
                 lib_openlog_kinds[LOL_COMPOSER], "scan");
    CHECK(!strcmp(b, "lib 1004ms n=62 albums/cmp scan"));
    printf("widest line %d chars (max %d); e.g. \"%s\"\n", widest,
           LIB_OPENLOG_MAX, b);
}

int main(void)
{
    test_quick();
    test_slow();
    test_length();
    printf("%d checks, %d failed\n", checks, fails);
    return fails || !checks;
}
