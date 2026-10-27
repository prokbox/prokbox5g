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

/* Host test: the Phase 5 memory layout (apps/ipodui/np_layout.h: the
 * strip offsets under UI_STRIP_OWNER_NP and the "np" block) and the art
 * cache's fit rectangle for non-square Now Playing art (artfmt.h,
 * Revision 1 item 3; owner photo 1222). */
#include <stdio.h>
#include <stdbool.h>
#include "np_layout.h"
#include "artfmt.h"
#include "check.h"

struct span { const char *name; unsigned off, size; };

static void no_overlap(const struct span *s, int n, unsigned limit)
{
    int i, j;

    for (i = 0; i < n; i++)
    {
        CHECK_EQ(s[i].off % 4, 0);
        CHECK(s[i].off + s[i].size <= limit);
        for (j = i + 1; j < n; j++)
            CHECK(s[i].off + s[i].size <= s[j].off
                  || s[j].off + s[j].size <= s[i].off);
    }
}

static void test_strip(void)
{
    const struct span s[] = {
        { "marquee", NP_STRIP_MARQUEE_OFF, NP_STRIP_MARQUEE_SIZE },
        { "row old", NP_STRIP_ROW_OLD_OFF, NP_STRIP_ROW_SIZE },
        { "row new", NP_STRIP_ROW_NEW_OFF, NP_STRIP_ROW_SIZE },
        { "lyrics",  NP_STRIP_LYRICS_OFF,  NP_STRIP_LYRICS_SIZE },
        { "lines",   NP_STRIP_LINES_OFF,   NP_STRIP_LINES_SIZE },
    };

    /* the sizes Revision 1 item 5 states */
    CHECK_EQ(NP_STRIP_MARQUEE_SIZE, 50112);
    CHECK_EQ(NP_STRIP_ROW_SIZE, 21760);
    CHECK_EQ(NP_STRIP_LYRICS_SIZE + NP_STRIP_LINES_SIZE, 9216);
    CHECK_EQ(NP_STRIP_END, 152064);
    CHECK(NP_STRIP_END <= NP_STRIP_LIMIT);
    CHECK_EQ(NP_STRIP_LIMIT, 300 * 300 * 2);        /* anim.h UI_STRIP_SIZE */
    no_overlap(s, (int)(sizeof(s) / sizeof(s[0])), NP_STRIP_LIMIT);
}

static void test_block(void)
{
    const struct span s[] = {
        { "hdr",    NP_BLK_HDR_OFF,    NP_BLK_HDR_SIZE },
        { "art",    NP_BLK_ART_OFF,    NP_BLK_ART_SIZE },
        { "digits", NP_BLK_DIGITS_OFF, NP_BLK_DIGITS_SIZE },
        { "glyphs", NP_BLK_GLYPHS_OFF, NP_BLK_GLYPHS_SIZE },
        { "track",  NP_BLK_TRACK_OFF,  NP_BLK_TRACK_SIZE },
        { "state",  NP_BLK_STATE_OFF,  NP_BLK_STATE_SIZE },
        { "ticks",  NP_BLK_TICKS_OFF,  NP_BLK_TICKS_SIZE },
    };

    /* QW2 (Rev 1 M-4): the composed art, the bundle header before it
     * (one read), the reflection band aliased onto its rows 165-186 (P11: geometry and the glass floor) */
    CHECK_EQ(NP_BLK_SIZE, 58368);
    CHECK_EQ(NP_BLK_ART_SIZE, 51238);
    CHECK_EQ(NP_BLK_ART_OFF, NP_BLK_HDR_OFF + NP_BLK_HDR_SIZE);
    CHECK(NP_BLK_TICKS_SIZE >= (NP_BAR_W + 7) / 8);
    CHECK_EQ(NP_BLK_END, 57540);
    CHECK_EQ(NP_REFL_BAND_ROW, 165);
    CHECK_EQ((NP_REFL_BAND_ROW + NP_REFL_BAND_H) * NP_COMP_W * 2,
             NP_BLK_ART_SIZE);
    /* struct np_scr (nowplaying.c, asserted there) still fits after it */
    CHECK(NP_BLK_SIZE - NP_BLK_END >= 656);
    no_overlap(s, (int)(sizeof(s) / sizeof(s[0])), NP_BLK_SIZE);
    /* host pointers are wider than the target's: if it fits here, it fits
     * there (the target build has the same static asserts) */
    CHECK(sizeof(struct np_track) <= NP_BLK_TRACK_SIZE);
    CHECK(sizeof(struct np_state) <= NP_BLK_STATE_SIZE);
    /* any fitted image fits the art slot */
    CHECK(NP_ART_BOX * NP_ART_BOX * 2 <= NP_BLK_ART_SIZE);
}

static void fit(unsigned sw, unsigned sh, unsigned w, unsigned h,
                unsigned x, unsigned y)
{
    struct artcache_fit f;

    artcache_fit_rect(sw, sh, NP_ART_BOX, NP_ART_BOX, &f);
    if (f.w != w || f.h != h || f.x != x || f.y != y)
    {
        check_fails++;
        fprintf(stderr, "fit %ux%u: %ux%u at %u,%u, want %ux%u at %u,%u\n",
                sw, sh, f.w, f.h, f.x, f.y, w, h, x, y);
    }
}

static void test_fit(void)
{
    struct artcache_fit f;
    unsigned sw, sh;

    CHECK(artcache_size_fits(ARTCACHE_NOWPLAYING));
    CHECK(!artcache_size_fits(ARTCACHE_THUMB));
    CHECK(!artcache_size_fits(ARTCACHE_PANEL));
    fit(600, 600, 142, 142, 0, 0);          /* square */
    fit(100, 100, 142, 142, 0, 0);          /* small: upscaled */
    fit(1920, 1080, 142, 80, 0, 31);        /* 16:9, the "Knight Bus" case */
    fit(1280, 720, 142, 80, 0, 31);
    fit(1000, 1500, 95, 142, 23, 0);        /* 2:3: centred horizontally */
    fit(2000, 500, 142, 36, 0, 53);         /* 4:1: 35.5 rounds up */
    fit(500, 2000, 36, 142, 53, 0);         /* 1:4 */
    fit(10000, 1, 142, 1, 0, 70);           /* extreme: at least 1 px */
    fit(0, 0, 142, 142, 0, 0);              /* unknown: the box */
    fit(65535, 65535, 142, 142, 0, 0);      /* no overflow */
    /* every fit keeps the aspect within half a pixel and stays in the box */
    for (sw = 1; sw < 3000; sw += 37)
        for (sh = 1; sh < 3000; sh += 41)
        {
            long err;
            artcache_fit_rect(sw, sh, NP_ART_BOX, NP_ART_BOX, &f);
            CHECK(f.w >= 1 && f.h >= 1);
            CHECK(f.x + f.w <= NP_ART_BOX && f.y + f.h <= NP_ART_BOX);
            CHECK(f.w == NP_ART_BOX || f.h == NP_ART_BOX);
            /* |w/h - sw/sh| in units of 1/2 px on the short side */
            if (f.w == NP_ART_BOX)
                err = (long)f.h * 2 * sw - (long)NP_ART_BOX * 2 * sh;
            else
                err = (long)f.w * 2 * sh - (long)NP_ART_BOX * 2 * sw;
            if (err < 0)
                err = -err;
            CHECK(err <= (long)(f.w == NP_ART_BOX ? sw : sh)
                  || f.w == 1 || f.h == 1);
        }
}

int main(void)
{
    test_strip();
    test_block();
    test_fit();
    CHECK_DONE("test_np_layout");
}
