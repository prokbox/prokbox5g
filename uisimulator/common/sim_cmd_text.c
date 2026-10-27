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
 ****************************************************************************/

/* Runtime native-illusion watch (P12 close; the "Rockbox" rows the owner
 * found in Settings > Main Menu, where an unlabeled checklist bit read
 * str(0), LANG id 0).
 *
 * Two sim-only hooks, both silent until something is wrong:
 *
 *  - sim_text_drawn(): every string the LCD draws (lcd_putsxy and the
 *    scrolling lines all end in putsxyofs, firmware/drivers/
 *    lcd-bitmap-common.c, so list rows, header titles and splashes all
 *    pass here). A string containing "rockbox" (any case, ".rockbox"
 *    included) is logged once per distinct text:
 *        sim text: forbidden x=<x> y=<y> text="<text>"
 *  - sim_lang_str(): str() itself in the shell's own files (the generated
 *    lang.h routes str() here in a SIMULATOR + HAVE_IPOD_UI build,
 *    tools/genlang). Id 0 or an id outside the core array is logged once
 *    per call site:
 *        lang: str id=<id> zero|out-of-range at <file>:<line>
 *    An out-of-range id returns "" instead of reading past the array.
 *
 * scripts/check_trace.py fails a script on either line (rule "illusion").
 * The first draw logs "sim text: watch on", so a log without it is a build
 * without the watch (a note, not a pass). */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include "config.h"

#ifdef HAVE_IPOD_UI
#include "lang.h"

#define SITES 64

static bool watch_on;
static unsigned text_hash[SITES];
static int text_n;
static struct { const char *file; int line; } lang_site[SITES];
static int lang_n;

static unsigned hash_str(const unsigned char *s)
{
    unsigned h = 2166136261u;
    while (*s)
        h = (h ^ *s++) * 16777619u;
    return h;
}

static bool has_rockbox(const unsigned char *s)
{
    static const char key[] = "rockbox";
    for (; *s; s++)
    {
        size_t i = 0;
        while (key[i] && s[i] && tolower(s[i]) == key[i])
            i++;
        if (!key[i])
            return true;
    }
    return false;
}

void sim_text_drawn(int x, int y, const unsigned char *str)
{
    if (!watch_on)
    {
        watch_on = true;
        fprintf(stderr, "sim text: watch on\n");
    }
    if (!str || !has_rockbox(str))
        return;
    unsigned h = hash_str(str);
    for (int i = 0; i < text_n; i++)
        if (text_hash[i] == h)
            return;
    if (text_n < SITES)
        text_hash[text_n++] = h;
    fprintf(stderr, "sim text: forbidden x=%d y=%d text=\"%.80s\"\n",
            x, y, (const char *)str);
}

unsigned char *sim_lang_str(int id, const char *file, int line)
{
    bool oob = id < 0 || id >= LANG_LAST_INDEX_IN_ARRAY;

    if ((oob || id == 0) && file && strstr(file, "/ipodui/"))
    {
        int i;
        for (i = 0; i < lang_n; i++)
            if (lang_site[i].line == line && !strcmp(lang_site[i].file, file))
                break;
        if (i == lang_n)
        {
            if (lang_n < SITES)
            {
                lang_site[lang_n].file = file;
                lang_site[lang_n].line = line;
                lang_n++;
            }
            const char *rel = strstr(file, "apps/");
            fprintf(stderr, "lang: str id=%d %s at %s:%d\n", id,
                    oob ? "out-of-range" : "zero", rel ? rel : file, line);
        }
    }
    return oob ? (unsigned char *)"" : language_strings[id];
}
#endif /* HAVE_IPOD_UI */
