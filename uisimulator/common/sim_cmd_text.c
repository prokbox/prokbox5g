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
 * without the watch (a note, not a pass).
 *
 * p19-relvideos: sim_text_drawn() also records every distinct drawn string
 * (up to TEXT_REC strings, TEXT_LEN bytes each) for three script commands
 * (sim_cmd_text.h): text_mark, expect_drawn, expect_not_drawn. */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include "config.h"
#include "sim_cmd_text.h"

#ifdef HAVE_IPOD_UI
#include "lang.h"

#define SITES 64

static bool watch_on;
static unsigned text_hash[SITES];
static int text_n;
static struct { const char *file; int line; } lang_site[SITES];
static int lang_n;

/* every distinct string drawn since the last text_mark */
#define TEXT_REC 512
#define TEXT_LEN 96
static char text_rec[TEXT_REC][TEXT_LEN];
static unsigned text_rec_hash[TEXT_REC];
static int text_rec_n;
static bool text_rec_full;

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
    if (!str)
        return;
    unsigned h = hash_str(str);
    if (!text_rec_full)
    {
        int i;
        for (i = 0; i < text_rec_n; i++)
            if (text_rec_hash[i] == h)
                break;
        if (i == text_rec_n)
        {
            if (text_rec_n < TEXT_REC)
            {
                text_rec_hash[text_rec_n] = h;
                snprintf(text_rec[text_rec_n], TEXT_LEN, "%s",
                         (const char *)str);
                text_rec_n++;
            }
            else
                text_rec_full = true;
        }
    }
    if (!has_rockbox(str))
        return;
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

/* the first recorded string containing needle, or NULL */
static const char *text_find(const char *needle)
{
    for (int i = 0; i < text_rec_n; i++)
        if (strstr(text_rec[i], needle))
            return text_rec[i];
    return NULL;
}

bool sim_cmd_text(const char *cmd, char *argv[], int argc, int lineno)
{
    char needle[TEXT_LEN] = "";
    const char *hit;
    bool want;

    if (!strcmp(cmd, "text_mark"))
    {
        text_rec_n = 0;
        text_rec_full = false;
        fprintf(stderr, "sim_script: text_mark\n");
        return true;
    }
    if (strcmp(cmd, "expect_drawn") && strcmp(cmd, "expect_not_drawn"))
        return false;
    want = !strcmp(cmd, "expect_drawn");
    for (int i = 1; i < argc; i++)      /* the words, joined by one space */
    {
        size_t n = strlen(needle);
        snprintf(needle + n, sizeof(needle) - n, "%s%s", i > 1 ? " " : "",
                 argv[i]);
    }
    if (!needle[0])
    {
        fprintf(stderr, "sim_script:%d: %s failed: no text given\n",
                lineno, cmd);
        return true;
    }
    hit = text_find(needle);
    if (!hit && text_rec_full)
        fprintf(stderr, "sim_script:%d: %s failed: \"%s\": over %d distinct "
                "strings drawn (text_mark sooner)\n", lineno, cmd, needle,
                TEXT_REC);
    else if (!!hit == want)
        fprintf(stderr, "sim_script: %s ok \"%s\"%s%s%s\n", cmd, needle,
                hit ? " in \"" : "", hit ? hit : "", hit ? "\"" : "");
    else
        fprintf(stderr, "sim_script:%d: %s failed: \"%s\"%s%s%s (%d strings "
                "since text_mark)\n", lineno, cmd, needle,
                hit ? " drawn in \"" : " never drawn", hit ? hit : "",
                hit ? "\"" : "", text_rec_n);
    return true;
}
#else /* !HAVE_IPOD_UI */
bool sim_cmd_text(const char *cmd, char *argv[], int argc, int lineno)
{
    (void)cmd; (void)argv; (void)argc; (void)lineno;
    return false;
}
#endif /* HAVE_IPOD_UI */
