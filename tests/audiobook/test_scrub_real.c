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

/* Now Playing chapter logic (apps/ipodui/np_scrub.c) on a real parse (#48).
 *
 * Reads warble -m output (the chapter table metadata_read_chapters()
 * produced for a fixture) on stdin into a packed struct audiobook_chapters
 * of the shell's size (chapters.h with WARBLE: AB_CH_TABLE_SIZE), points
 * np_chapters at its start_ms column in place, the way Now Playing does,
 * and checks "N of M" for every chapter, the snap scrub and the ticks.
 * Usage: test_scrub_real <expected count> <N> "<N of M text>" < warble.txt */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "chapters.h"
#include "np_scrub.h"

static uint32_t ab_mem[(AB_CH_TABLE_SIZE + 3) / 4]; /* too big for the stack */
static int fails;

#define CHECK_EQ(a, b) do { long _a = (long)(a), _b = (long)(b); \
    if (_a != _b) { fails++; fprintf(stderr, "%s:%d: %s == %ld, expected " \
        "%ld\n", __FILE__, __LINE__, #a, _a, _b); } } while (0)

int main(int argc, char **argv)
{
    char line[256], text[32];
    unsigned long ms;
    int idx, i, ai, want, n_of;
    uint8_t bits[(204 + 7) / 8];

    struct audiobook_chapters *ab = (struct audiobook_chapters *)ab_mem;

    if (argc != 4)
        return 2;
    ab_ch_init(ab, AB_MAX_CHAPTERS, AB_CH_POOL);
    want = atoi(argv[1]);
    n_of = atoi(argv[2]);

    while (fgets(line, sizeof(line), stdin))
    {
        if (sscanf(line, "Chapter %d: %lu ms", &idx, &ms) != 2)
            continue;
        if (idx != ab->chapter_count + 1 || idx > AB_MAX_CHAPTERS)
        {
            fprintf(stderr, "bad chapter line: %s", line);
            return 1;
        }
        ab_ch_put(ab, ab->chapter_count, (uint32_t)ms, "", 0);
        ab->chapter_count++;
    }
    CHECK_EQ(ab->chapter_count, want);

    struct np_chapters ch = {
        &ab_ch_start(ab, 0), sizeof(struct ab_ch_entry), ab->chapter_count
    };
    unsigned long len = ab_ch_start(ab, ab->chapter_count - 1) + 200;

    /* every chapter, at its start and inside it */
    for (i = 0; i < ch.count; i++)
    {
        int at = np_chapter_at(&ch, ab_ch_start(ab, i), &ai);
        int in = np_chapter_at(&ch, ab_ch_start(ab, i) + 1, NULL);
        if (at != i || ai != i || in != i)
        {
            CHECK_EQ(at, i);
            CHECK_EQ(ai, i);
            CHECK_EQ(in, i);
            break;
        }
    }
    CHECK_EQ(np_chapter_at(&ch, len, NULL), ch.count - 1);

    /* "73 of 648" as the header shows it */
    snprintf(text, sizeof(text), "%d of %d",
             np_chapter_at(&ch, ab_ch_start(ab, n_of - 1) + 1, NULL) + 1,
             ch.count);
    if (strcmp(text, argv[3]))
    {
        fails++;
        fprintf(stderr, "got \"%s\", expected \"%s\"\n", text, argv[3]);
    }

    /* the fast wheel snaps through every chapter start, then to the end */
    {
        struct np_scrub_ctx sc = { .length_ms = len, .ch = ch, .snap = true };
        unsigned long pos = 0;
        for (i = 1; i < ch.count; i++)
        {
            pos = np_scrub_wheel(&sc, pos, 1, NP_SCRUB_SNAP_V);
            if (pos != ab_ch_start(ab, i))
            {
                CHECK_EQ(pos, ab_ch_start(ab, i));
                break;
            }
        }
        CHECK_EQ(np_scrub_wheel(&sc, pos, 1, NP_SCRUB_SNAP_V), len);
    }

    /* ticks on a 204 px bar merge, all strictly inside */
    {
        int n = np_chapter_ticks(&ch, len, 204, bits);
        if (n <= 0 || n > 203 || (bits[0] & 1))
        {
            fails++;
            fprintf(stderr, "ticks: %d pixels\n", n);
        }
    }

    printf("   %s: %d chapters, AB_CH_TABLE_SIZE %u B "
           "(host)\n", text, ch.count, (unsigned)AB_CH_TABLE_SIZE);
    return fails != 0;
}
