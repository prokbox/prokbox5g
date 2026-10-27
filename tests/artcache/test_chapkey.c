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

/* Host test: artcache_key_chapter() (apps/ipodui/artkey.c, #39) against
 * the golden file tests/artcache/chapter_keys.txt (gen_keys.py --chapters,
 * the reference the Phase 7b Mac tool must match). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include "artkey.h"

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "tests/artcache/chapter_keys.txt";
    FILE *f = fopen(path, "r");
    char line[256];
    int n = 0, fails = 0;

    if (!f)
    {
        printf("cannot open %s\n", path);
        return 1;
    }
    while (fgets(line, sizeof(line), f))
    {
        unsigned long long book, want;
        int ch;
        artcache_key_t got;

        if (line[0] == '#')
            continue;
        if (sscanf(line, "%llx %d %llx", &book, &ch, &want) != 3)
        {
            printf("bad line: %s", line);
            fails++;
            continue;
        }
        got = artcache_key_chapter((artcache_key_t)book, ch);
        if (got != (artcache_key_t)want)
        {
            printf("chapter key %016llx/%d: %016" PRIx64 ", want %016llx\n",
                   book, ch, got, want);
            fails++;
        }
        n++;
    }
    fclose(f);
    /* distinct chapters of one book never collide in the goldens' range */
    if (artcache_key_chapter(0xb60d10106ed9f8ceull, 0)
        == artcache_key_chapter(0xb60d10106ed9f8ceull, 1))
        fails++;
    if (n < 10)
    {
        printf("only %d golden chapter keys\n", n);
        fails++;
    }
    return fails ? 1 : 0;
}
