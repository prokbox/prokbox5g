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

/* Host tests for apps/ipodui/np_lyrics.c (Now Playing lyrics wrap). */
#include <string.h>
#include "np_lyrics.h"
#include "check.h"

/* a fixed-pitch "font": every code point 7 px, a space 4 */
static int adv(void *ctx, unsigned cp)
{
    (void)ctx;
    return cp == ' ' ? 4 : 7;
}

static int lines(const char *t, int width, uint16_t *st, int max)
{
    struct np_lyr_font f = { adv, NULL, width };
    return np_lyrics_wrap(t, (int)strlen(t), &f, st, max);
}

static int line_is(const char *t, const uint16_t *st, int n, int i,
                   const char *want)
{
    int b, e;
    np_lyrics_line(t, (int)strlen(t), st, n, i, &b, &e);
    return (int)strlen(want) == e - b && !memcmp(t + b, want, e - b);
}

int main(void)
{
    uint16_t st[64];
    const char *t;
    int n;

    /* line breaks: LF, CRLF, CR; a blank line is kept */
    t = "one\ntwo\r\nthree\rfour\n\nfive\n";
    n = lines(t, 300, st, 64);
    CHECK_EQ(n, 6);
    CHECK(line_is(t, st, n, 0, "one"));
    CHECK(line_is(t, st, n, 1, "two"));
    CHECK(line_is(t, st, n, 2, "three"));
    CHECK(line_is(t, st, n, 3, "four"));
    CHECK(line_is(t, st, n, 4, ""));
    CHECK(line_is(t, st, n, 5, "five"));

    /* word wrap at the last space that fits: 7 px chars, 4 px spaces,
     * 60 px = "aaaa bbbb" is 7*8 + 4 = 60 */
    t = "aaaa bbbb cccc dddd";
    n = lines(t, 60, st, 64);
    CHECK_EQ(n, 2);
    CHECK(line_is(t, st, n, 0, "aaaa bbbb"));
    CHECK(line_is(t, st, n, 1, "cccc dddd"));

    /* a word wider than the line breaks between characters */
    t = "abcdefghijkl";
    n = lines(t, 35, st, 64);
    CHECK_EQ(n, 3);
    CHECK(line_is(t, st, n, 0, "abcde"));
    CHECK(line_is(t, st, n, 2, "kl"));

    /* LRC time tags are skipped, also for the width */
    t = "[00:12.34]hello\n[01:02]world [x]";
    n = lines(t, 300, st, 64);
    CHECK_EQ(n, 2);
    CHECK(line_is(t, st, n, 0, "hello"));
    CHECK(line_is(t, st, n, 1, "world [x]"));
    t = "[00:12.34][00:40.00] x\n";
    n = lines(t, 7, st, 64);
    CHECK_EQ(n, 1);
    CHECK(line_is(t, st, n, 0, "x"));

    /* UTF-8: a multi-byte character is one advance, never split */
    t = "\xc3\xa9\xc3\xa9\xc3\xa9";     /* three e-acute */
    n = lines(t, 14, st, 64);
    CHECK_EQ(n, 2);
    CHECK_EQ(st[1], 4);

    /* the line cap and empty text */
    n = lines("a\nb\nc\nd\n", 300, st, 3);
    CHECK_EQ(n, 3);
    CHECK_EQ(lines("", 300, st, 64), 0);
    CHECK_EQ(np_lyrics_max_top(20, 9), 11);
    CHECK_EQ(np_lyrics_max_top(5, 9), 0);
    CHECK_DONE("test_lyrics");
}
