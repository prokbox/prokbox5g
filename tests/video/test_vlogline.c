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

/* Host test of the video player's log line (#224, apps/video/vlogline.h):
 * the numbers come before the legend, so ui_log's cut (UI_LOG_LINE - 2
 * bytes, read from apps/ipodui/uilog.h) takes the legend, never a value.
 * Reads tests/video/vlog_names.py's list on stdin: every line the player
 * logs must keep both numbers at six characters each.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vlogline.h"

#define NAMES_MIN   100

static int fails, checks;
static int slot;                /* ui_log's text bytes, NUL included */

static void check(bool ok, const char *what, const char *got)
{
    checks++;
    if (!ok)
    {
        fails++;
        printf("FAIL: %s\n      got \"%s\"\n", what, got);
    }
}

/* the line as ui_log("video", "%s", line) stores it (uilog.c format()) */
static void stored(char *out, const char *line)
{
    int n = snprintf(out, slot, "%s ", "video");

    snprintf(out + n, slot - n, "%s", line);
}

static int read_slot(void)
{
    FILE *f = fopen("apps/ipodui/uilog.h", "r");
    char l[256];
    int v = 0;

    if (!f)
        return 0;
    while (fgets(l, sizeof(l), f))
        if (sscanf(l, "#define UI_LOG_LINE %d", &v) == 1)
            break;
    fclose(f);
    return v > 2 ? v - 2 : 0;   /* struct ui_log_line: seq, text[] */
}

static void exact(const char *what, long a, long b, bool hex,
                  const char *want)
{
    char line[128], msg[256];

    vlog_line(line, sizeof(line), what, a, b, hex);
    snprintf(msg, sizeof(msg), "\"%s\" -> \"%s\"", what, want);
    check(!strcmp(line, want), msg, line);
}

/* both numbers whole in the stored line, right after the name */
static void kept(const char *what, long a, long b, bool hex)
{
    char line[128], s[128], want[160], msg[256];
    const char *p = strstr(what, " (");
    int name = p ? (int)(p - what) : (int)strlen(what);

    if (!p)                     /* the name ends at the last ": " if any */
    {
        const char *t = strstr(what, ": "), *q, *last = NULL;

        for (q = t ? strstr(t + 2, ": ") : NULL; q; q = strstr(q + 2, ": "))
            last = q;
        if (last)
            name = last + 1 - what;
    }
    vlog_line(line, sizeof(line), what, a, b, hex);
    stored(s, line);
    if (hex)
        snprintf(want, sizeof(want), "video %.*s %08lx %08lx", name, what,
                 (unsigned long)(uint32_t)a, (unsigned long)(uint32_t)b);
    else
        snprintf(want, sizeof(want), "video %.*s %ld %ld", name, what, a, b);
    snprintf(msg, sizeof(msg), "\"%s\": both numbers in the %d-byte slot "
             "(\"%s\")", what, slot - 1, want);
    check(!strncmp(s, want, strlen(want)) &&
          (s[strlen(want)] == ' ' || s[strlen(want)] == '\0'), msg, s);
}

int main(void)
{
    char l[256], s[128], line[128];
    int names = 0;

    slot = read_slot();
    if (slot < 16)
    {
        printf("FAIL: no UI_LOG_LINE in apps/ipodui/uilog.h\n");
        return 1;
    }

    /* the shapes (vlogline.h's comment) */
    exact("video: thread late (ms past its wait, audio ms)", 296, 0, false,
          "video: thread late 296 0 (ms past its wait, audio ms)");
    exact("video: still: posts, data waits", 0, 3, false,
          "video: still: 0 3 (posts, data waits)");
    exact("video: playing", 320, 240, false, "video: playing 320 240");
    exact("video: resumed past the pictures (ms), seeking", 1200, 0, false,
          "video: resumed past the pictures 1200 0 (ms), seeking");
    exact("video: params: bytes, NAL types (bit n: type n)", 25, 384, false,
          "video: params: bytes, NAL types 25 384 (bit n: type n)");
    exact("video: audio jumped (from, to ms)", -5, 12, false,
          "video: audio jumped -5 12 (from, to ms)");
    exact("sdram: guard: hit at, by check", 0x1234, 2, true,
          "sdram: guard: 00001234 00000002 (hit at, by check)");
    exact("sdram: probe read", 0xdeadbeefL, 1, true,
          "sdram: probe read deadbeef 00000001");

    /* the owner's cut line (#218): its values are kept now */
    vlog_line(line, sizeof(line),
              "video: first picture (ms after the start, boot ms)", 203,
              4810, false);
    stored(s, line);
    check(strstr(s, " 203 4810") != NULL,
          "first picture: 203 and 4810 survive the cut", s);

    /* a legend longer than the whole slot: both numbers still there */
    kept("video: x (a legend far longer than the log's line, which ui_log "
         "cuts at sixty-two bytes)", 123456, -12345, false);
    vlog_line(line, sizeof(line), "video: x (a legend far longer than the "
              "log's line, which ui_log cuts at sixty-two bytes)", 123456,
              -12345, false);
    check(strlen(line) + 6 > (size_t)slot - 1, "the long legend is cut",
          line);

    /* every line the player logs, at six characters a number */
    while (fgets(l, sizeof(l), stdin))
    {
        l[strcspn(l, "\n")] = '\0';
        if (strlen(l) < 3 || l[1] != '\t')
            continue;
        kept(l + 2, -12345, 123456, l[0] == 'x');
        names++;
    }
    printf("test_vlogline: %d names, %d checks, %d failed (%d characters a "
           "line)\n", names, checks, fails, slot - 1);
    if (names < NAMES_MIN)
    {
        printf("FAIL: %d names read, expected %d or more\n", names,
               NAMES_MIN);
        return 1;
    }
    return fails ? 1 : 0;
}
