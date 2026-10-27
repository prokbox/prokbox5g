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

/* Host test: the Phase 5 contracts' pure parts (apps/ipodui/np_pbcore.c,
 * np_scrub.c np_chapter_step(), np_state.c np_set_lyrics()):
 * rating mapping and write guard (Revision 1 item 1), the "N of M"
 * counter source (Revision 2), the seqlock rule (Revision 1 item 4). */
#include <stdlib.h>
#include <string.h>
#include "np_pbcore.h"
#include "np_scrub.h"
#include "np_state.h"
#include "check.h"

static void test_rating_map(void)
{
    int s;

    for (s = 0; s <= NP_STARS_MAX; s++)
    {
        CHECK_EQ(np_rating_to_tag(s), s * 2);
        CHECK_EQ(np_rating_from_tag(np_rating_to_tag(s)), s);   /* round trip */
    }
    CHECK_EQ(np_rating_to_tag(-1), 0);
    CHECK_EQ(np_rating_to_tag(9), 10);
    CHECK_EQ(np_rating_from_tag(-3), 0);
    CHECK_EQ(np_rating_from_tag(1), 0);         /* odd values round down */
    CHECK_EQ(np_rating_from_tag(7), 3);
    CHECK_EQ(np_rating_from_tag(10), 5);
    CHECK_EQ(np_rating_from_tag(100), 5);       /* out of range: clamped */
}

static void test_rating_target(void)
{
    long idx = 99;

    /* 1-based tagcache_idx -> 0-based master index */
    CHECK_EQ(np_rating_target(1, 7, 7, &idx), NP_RATE_WRITE);
    CHECK_EQ(idx, 0);
    CHECK_EQ(np_rating_target(4242, 7, 7, &idx), NP_RATE_WRITE);
    CHECK_EQ(idx, 4241);
    /* not in the database */
    CHECK_EQ(np_rating_target(0, 7, 7, &idx), NP_RATE_NO_ENTRY);
    CHECK_EQ(idx, -1);
    CHECK_EQ(np_rating_target(-5, 7, 7, &idx), NP_RATE_NO_ENTRY);
    /* the database was reloaded since the snapshot: never the wrong file */
    CHECK_EQ(np_rating_target(4242, 7, 8, &idx), NP_RATE_STALE);
    CHECK_EQ(idx, -1);
    /* no entry wins over stale (nothing to name either way) */
    CHECK_EQ(np_rating_target(0, 7, 8, &idx), NP_RATE_NO_ENTRY);
}

static void test_rating_override(void)
{
    struct np_rating_ovr o;

    memset(&o, 0, sizeof(o));
    CHECK_EQ(np_rating_shown(&o, 12, 3, 2), 2);         /* none: db value */
    np_rating_ovr_set(&o, 12, 3, 4);
    CHECK_EQ(np_rating_shown(&o, 12, 3, 2), 4);         /* same entry */
    CHECK_EQ(np_rating_shown(&o, 13, 3, 1), 1);         /* other track */
    CHECK_EQ(np_rating_shown(&o, 12, 4, 2), 2);         /* reloaded db */
    np_rating_ovr_set(&o, 12, 3, 0);
    CHECK_EQ(np_rating_shown(&o, 12, 3, 5), 0);         /* cleared to 0 */
    np_rating_ovr_set(&o, 12, 3, 8);
    CHECK_EQ(np_rating_shown(&o, 12, 3, 0), 5);         /* clamped */
    np_rating_ovr_set(&o, 0, 3, 3);                     /* no entry */
    CHECK_EQ(np_rating_shown(&o, 0, 3, 1), 1);
}

static void test_counter(void)
{
    struct np_counter c;

    /* music: queue */
    c = np_counter_pick(false, false, 4, 12, -1, 0);
    CHECK_EQ(c.src, NP_COUNT_QUEUE); CHECK_EQ(c.n, 5); CHECK_EQ(c.m, 12);
    /* music file that happens to have chapters: still the queue */
    c = np_counter_pick(false, false, 0, 3, 2, 9);
    CHECK_EQ(c.src, NP_COUNT_QUEUE); CHECK_EQ(c.n, 1); CHECK_EQ(c.m, 3);
    /* audiobook file with chapters: chapter 73 of 648 */
    c = np_counter_pick(true, false, 0, 1, 72, 648);
    CHECK_EQ(c.src, NP_COUNT_CHAPTER); CHECK_EQ(c.n, 73); CHECK_EQ(c.m, 648);
    /* before the first chapter start / unknown: the first */
    c = np_counter_pick(true, false, 0, 1, -1, 648);
    CHECK_EQ(c.src, NP_COUNT_CHAPTER); CHECK_EQ(c.n, 1);
    c = np_counter_pick(true, false, 0, 1, 900, 648);
    CHECK_EQ(c.n, 648);                                 /* clamped */
    /* multi-file book without chapters: file within the book = queue */
    c = np_counter_pick(true, false, 2, 7, -1, 0);
    CHECK_EQ(c.src, NP_COUNT_QUEUE); CHECK_EQ(c.n, 3); CHECK_EQ(c.m, 7);
    /* book started from a user playlist: chapters if the file has them */
    c = np_counter_pick(true, false, 40, 120, 3, 10);
    CHECK_EQ(c.src, NP_COUNT_CHAPTER); CHECK_EQ(c.n, 4); CHECK_EQ(c.m, 10);
    c = np_counter_pick(true, false, 40, 120, -1, 0);
    CHECK_EQ(c.src, NP_COUNT_QUEUE); CHECK_EQ(c.n, 41); CHECK_EQ(c.m, 120);
    /* podcasts keep the queue counter, chapters or not */
    c = np_counter_pick(true, true, 5, 30, 2, 12);
    CHECK_EQ(c.src, NP_COUNT_QUEUE); CHECK_EQ(c.n, 6); CHECK_EQ(c.m, 30);
    /* nothing queued */
    c = np_counter_pick(false, false, -1, 0, -1, 0);
    CHECK_EQ(c.src, NP_COUNT_NONE); CHECK_EQ(c.n, 0); CHECK_EQ(c.m, 0);
    c = np_counter_pick(false, false, 9, 5, -1, 0);     /* index past end */
    CHECK_EQ(c.n, 5);
}

static void test_seqlock(void)
{
    CHECK(np_seq_stable(0));
    CHECK(!np_seq_stable(1));
    CHECK(np_seq_read_ok(4, 4));
    CHECK(!np_seq_read_ok(5, 5));       /* being rewritten */
    CHECK(!np_seq_read_ok(4, 5));       /* writer started during the copy */
    CHECK(!np_seq_read_ok(4, 6));       /* track changed during the copy */
    CHECK(np_seq_read_ok(0xfffffffeu, 0xfffffffeu));
}

/* np_chapter_step() == np_chapter_at() on sorted tables, any hint */
static void test_chapter_step(void)
{
    uint32_t st[700];
    struct np_chapters ch = { st, sizeof(st[0]), 0 };
    int n, k, prev = 0;
    unsigned long pos;

    srand(5);
    for (n = 0; n <= 648; n += (n < 8 ? 1 : 97))
    {
        unsigned long t = (unsigned long)(rand() % 3000);
        int i;

        for (i = 0; i < n; i++)
        {
            st[i] = t;
            if (rand() % 5)                 /* some equal starts (ties) */
                t += 1 + (unsigned long)(rand() % 60000);
        }
        ch.count = n;
        if (n == 0)
        {
            CHECK_EQ(np_chapter_step(&ch, 0, 0), -1);
            continue;
        }
        /* forward play, step by step from the previous result */
        prev = 0;
        for (pos = 0; pos <= t + 2000; pos += 997)
        {
            int a, want = np_chapter_at(&ch, pos, &a);
            prev = np_chapter_step(&ch, pos, prev);
            CHECK_EQ(prev, a);
            CHECK_EQ(prev, want);           /* sorted: rank == index */
        }
        /* seeks: random positions from random hints */
        for (k = 0; k < 300; k++)
        {
            int a;
            pos = (unsigned long)(rand() % (int)(t + 5000));
            np_chapter_at(&ch, pos, &a);
            CHECK_EQ(np_chapter_step(&ch, pos, rand() % (n + 20) - 10), a);
        }
    }
}

static void test_set_lyrics(void)
{
    struct np_state st;
    struct np_scrub_ctx sc;

    memset(&sc, 0, sizeof(sc));
    sc.length_ms = 200000;
    np_init(&st, 100, NP_SHUFFLE_OFF);
    np_set_track(&st, false, true, 0, 0, &sc, 0);
    CHECK(st.has_lyrics);
    np_set_lyrics(&st, 30);
    CHECK_EQ(st.lyrics_max, 30);
    st.lyrics_top = 25;
    np_set_lyrics(&st, 10);                 /* shorter: scroll clamped */
    CHECK_EQ(st.lyrics_top, 10);
    np_set_lyrics(&st, 0);
    CHECK_EQ(st.lyrics_top, 0);
    CHECK(st.has_lyrics);
    np_set_lyrics(&st, -1);                 /* the read failed */
    CHECK(!st.has_lyrics);
    CHECK_EQ(st.lyrics_max, 0);
}

int main(void)
{
    test_rating_map();
    test_rating_target();
    test_rating_override();
    test_counter();
    test_seqlock();
    test_chapter_step();
    test_set_lyrics();
    CHECK_DONE("test_pbcore");
}
