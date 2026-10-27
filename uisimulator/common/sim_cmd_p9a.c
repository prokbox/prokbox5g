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
 * Phase 9 Part 1 sim script commands (agent A); see sim_cmd_p9a.h.
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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "config.h"
#include "system.h"
#include "kernel.h"
#include "sim_cmd_p9a.h"

#ifdef HAVE_TAGCACHE
#include "tagcache.h"

static long marked;

static void unesc(char *p)
{
    char *r = p, *w = p;

    while (*r)
    {
        if (r[0] == '%' && r[1] == '2' && r[2] == '0')
        {
            *w++ = ' ';
            r += 3;
        }
        else
            *w++ = *r++;
    }
    *w = '\0';
}

static int tag_of(const char *name)
{
    static const struct { const char *name; int tag; } tags[] = {
        { "playcount",   tag_playcount },
        { "playtime",    tag_playtime },
        { "lastplayed",  tag_lastplayed },
        { "lastelapsed", tag_lastelapsed },
        { "lastoffset",  tag_lastoffset },
        { "rating",      tag_rating },
    };
    for (unsigned i = 0; i < sizeof(tags) / sizeof(tags[0]); i++)
        if (!strcmp(name, tags[i].name))
            return tags[i].tag;
    return -1;
}

static long db_numeric(const char *path, int tag)
{
    struct tagcache_search tcs;
    long v;

    if (!tagcache_find_index(&tcs, path))
        return -2;
    v = tagcache_get_numeric(&tcs, tag);
    tagcache_search_finish(&tcs);
    return v;
}

/* expect_stat / expect_stat_delta <path> <tag>=<lo>..<hi|max> [wait_ms]
 * (sim_script.c passes at most 3 arguments) */
static void expect(const char *cmd, char *argv[], int argc, int lineno,
                   bool delta)
{
    char *spec = argv[2], *eq = strchr(spec, '='), *dots;
    long lo, hi, wait = argc >= 4 ? atol(argv[3]) : 3000, waited = 0;
    long got, v;
    int tag;

    dots = eq ? strstr(eq + 1, "..") : NULL;
    if (!eq || !dots)
    {
        fprintf(stderr, "sim_script:%d: %s failed: bad spec %s "
                "(want tag=lo..hi)\n", lineno, cmd, spec);
        return;
    }
    *eq = '\0';
    tag = tag_of(spec);
    lo = atol(eq + 1);
    hi = strcmp(dots + 2, "max") ? atol(dots + 2) : 0x7fffffffL;
    if (tag < 0)
    {
        fprintf(stderr, "sim_script:%d: %s failed: unknown tag %s\n",
                lineno, cmd, spec);
        return;
    }
    unesc(argv[1]);
    for (;;)
    {
        got = db_numeric(argv[1], tag);
        v = (delta && got >= 0) ? got - marked : got;
        if ((got >= 0 && v >= lo && v <= hi) || waited >= wait)
            break;
        sleep(HZ / 20);
        waited += 50;
    }
    if (got >= 0 && v >= lo && v <= hi)
        fprintf(stderr, "sim_script: %s ok: %s %s = %ld%s\n", cmd, argv[1],
                spec, v, delta ? " (delta)" : "");
    else
        fprintf(stderr, "sim_script:%d: %s failed: %s %s is %ld, want "
                "%ld..%ld (-2: not in the database)\n", lineno, cmd, argv[1],
                spec, v, lo, hi);
}

bool sim_cmd_p9a(const char *cmd, char *argv[], int argc, int lineno)
{
    if (!strcmp(cmd, "stat_mark") && argc >= 3)
    {
        int tag = tag_of(argv[2]);

        unesc(argv[1]);
        marked = tag < 0 ? -1 : db_numeric(argv[1], tag);
        fprintf(stderr, "sim_script: stat_mark %s %s = %ld\n", argv[1],
                argv[2], marked);
        return true;
    }
    if (!strcmp(cmd, "expect_stat") && argc >= 3)
    {
        expect(cmd, argv, argc, lineno, false);
        return true;
    }
    if (!strcmp(cmd, "expect_stat_delta") && argc >= 3)
    {
        expect(cmd, argv, argc, lineno, true);
        return true;
    }
    if (!strcmp(cmd, "expect_in_db") && argc >= 2)
    {
        long wait = argc >= 3 ? atol(argv[2]) : 30000, waited = 0;
        struct tagcache_search tcs;
        bool found;

        unesc(argv[1]);
        while (!(found = tagcache_find_index(&tcs, argv[1])) && waited < wait)
        {
            sleep(HZ / 10);
            waited += 100;
        }
        if (found)
        {
            tagcache_search_finish(&tcs);
            fprintf(stderr, "sim_script: expect_in_db ok: %s after %ld ms\n",
                    argv[1], waited);
        }
        else
            fprintf(stderr, "sim_script:%d: expect_in_db failed: %s\n",
                    lineno, argv[1]);
        return true;
    }
    /* expect_not_in_db <path> [ms]: the library has no live entry for
     * path (deleted and marked, #89 H2) within ms (default 30000) */
    if (!strcmp(cmd, "expect_not_in_db") && argc >= 2)
    {
        long wait = argc >= 3 ? atol(argv[2]) : 30000, waited = 0;
        struct tagcache_search tcs;
        bool found;

        unesc(argv[1]);
        while ((found = tagcache_find_index(&tcs, argv[1])) && waited < wait)
        {
            tagcache_search_finish(&tcs);
            sleep(HZ / 10);
            waited += 100;
        }
        if (found)
        {
            tagcache_search_finish(&tcs);
            fprintf(stderr, "sim_script:%d: expect_not_in_db failed: %s\n",
                    lineno, argv[1]);
        }
        else
            fprintf(stderr, "sim_script: expect_not_in_db ok: %s after %ld "
                    "ms\n", argv[1], waited);
        return true;
    }
    return false;
}

#else /* !HAVE_TAGCACHE */

bool sim_cmd_p9a(const char *cmd, char *argv[], int argc, int lineno)
{
    (void)cmd; (void)argv; (void)argc; (void)lineno;
    return false;
}

#endif /* HAVE_TAGCACHE */
