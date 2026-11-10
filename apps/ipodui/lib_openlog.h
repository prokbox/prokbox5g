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

/* #230: the device's library open timing line (library.c
 * lib_open_held()). One ui_log("lib", LIB_OPENLOG_FMT, ...) line when an
 * open took LIB_OPENLOG_MS or more of wall time, or is the first of its
 * kind (category and filter) this boot: a device number to calibrate
 * scripts/check_trace.py's SIM_FACTOR, without a line for every quick
 * open. Numbers first, at most LIB_OPENLOG_MAX characters with the
 * "lib " tag (the log ring keeps about 55 of a line). No file I/O of its
 * own; the decision is here, header-only, for tests/libopenlog. */
#ifndef _IPODUI_LIB_OPENLOG_H_
#define _IPODUI_LIB_OPENLOG_H_

#include <stdbool.h>
#include <stdint.h>

#define LIB_OPENLOG_MS      50
#define LIB_OPENLOG_CATS    16      /* enum lib_cat values tracked */
#define LIB_OPENLOG_MAX     54      /* characters, "lib " included */
/* ms, rows, category, filter kind, src (file | slice | scan) */
#define LIB_OPENLOG_FMT     "%lums n=%d %s%s %s"

/* the filter's kind: suffix of the category in the line */
enum lib_openlog_kind { LOL_ALL, LOL_ARTIST, LOL_ALBUM, LOL_GENRE,
                        LOL_COMPOSER, LOL_OTHER, LOL_KINDS };
static const char * const lib_openlog_kinds[LOL_KINDS] = {
    "", "/art", "/alb", "/gen", "/cmp", "/mix"
};

struct lib_openlog {
    uint8_t seen[LIB_OPENLOG_CATS];     /* bit k: kind k logged this boot */
};

/* Whether an open of 'cat' with filter kind 'kind' that took 'ms' is
 * logged; notes its kind as seen. */
static inline bool lib_openlog_due(struct lib_openlog *s, unsigned cat,
                                   unsigned kind, unsigned long ms)
{
    bool first = false;

    if (kind >= LOL_KINDS)
        kind = LOL_OTHER;
    if (cat < LIB_OPENLOG_CATS)
    {
        first = !(s->seen[cat] & (1u << kind));
        s->seen[cat] |= (uint8_t)(1u << kind);
    }
    return first || ms >= LIB_OPENLOG_MS;
}

#endif /* _IPODUI_LIB_OPENLOG_H_ */
