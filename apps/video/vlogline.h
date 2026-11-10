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

/* The video player's log line: its two numbers before its legend (#224)
 *
 * ui_log keeps 62 bytes of a line (uilog.h UI_LOG_LINE) and cuts the
 * rest.  The player's lines used to end in their numbers ("video: first
 * picture (ms after the start, boot ms) (203, 4810)"), so a long legend
 * cost exactly the values a diagnostics report needs.  Now the numbers
 * follow the line's short name and the legend comes last, so a cut takes
 * the legend, never a value:
 *
 *   "video: thread late (ms past its wait, audio ms)", 296, 0
 *       -> "video: thread late 296 0 (ms past its wait, audio ms)"
 *   "video: still: posts, data waits", 0, 3
 *       -> "video: still: 0 3 (posts, data waits)"
 *   "video: playing", 320, 240
 *       -> "video: playing 320 240"
 *
 * The name is the text before the first " (" (the legend is the rest, as
 * written); without one, the text up to the last ": " after the tag's
 * own ("video: ", "sdram: "), and what follows is put in parentheses;
 * without either, the whole text, and the numbers end the line.  "hex"
 * prints the numbers as 32-bit register words (%08lx, the sdram: lines;
 * the same width on the 64-bit simulator).
 */
#ifndef VLOGLINE_H
#define VLOGLINE_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static inline int vlog_line(char *out, size_t size, const char *what,
                            long a, long b, bool hex)
{
    const char *legend = strstr(what, " (");
    const char *tag = strstr(what, ": ");
    const char *open = "", *close = "";
    char nums[2 * 21 + 2];
    int name;

    if (legend)
    {
        name = legend - what;
        legend++;                           /* "(...)", as written */
    }
    else
    {
        const char *p, *last = NULL;

        for (p = tag ? strstr(tag + 2, ": ") : NULL; p;
             p = strstr(p + 2, ": "))
            last = p;
        if (last)
        {
            name = last + 1 - what;         /* the name keeps its ':' */
            legend = last + 2;
            open = "(";
            close = ")";
        }
        else
        {
            name = strlen(what);
            legend = NULL;
        }
    }
    if (hex)
        snprintf(nums, sizeof(nums), "%08lx %08lx",
                 (unsigned long)(uint32_t)a, (unsigned long)(uint32_t)b);
    else
        snprintf(nums, sizeof(nums), "%ld %ld", a, b);
    if (!legend)
        return snprintf(out, size, "%.*s %s", name, what, nums);
    return snprintf(out, size, "%.*s %s %s%s%s", name, what, nums, open,
                    legend, close);
}

#endif /* VLOGLINE_H */
