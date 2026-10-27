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

/* Bare-metal ARM workloads for tests/search/insn.sh (docs/phase11-brief.md
 * section 7 criterion 3): dynamic instruction counts of srch_match()
 * (apps/ipodui/search_core.c) over a title corpus, per haystack byte.
 * WL 0 = baseline (the corpus walk without matching). GPL v2+ */
#include <stdint.h>
#include <stddef.h>
#include "search.h"

#ifndef WL
#define WL 0
#endif
#define REPS 4

int memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *x = a, *y = b;
    for (; n; n--, x++, y++)
        if (*x != *y)
            return *x - *y;
    return 0;
}

/* titles of typical length, some accents, mixed case */
static const char *const titles[] = {
    "Bohemian Rhapsody", "Hey Juno", "Smells Like Teen Spirit",
    "Hotel California", "Stairway to Heaven", "Imagine", "One",
    "Billie Jean", "Like a Rolling Stone", "Sweet Child o' Mine",
    "Comfortably Numb", "Purple Haze", "Good Vibrations", "Halcyon",
    "Haiku", "Born to Run", "What's Going On", "Respect",
    "London Calling", "Waterfall Sunset", "Wish You Were Here",
    "Under Pressure", "Paranoid Android", "Café del Mar",
    "Où est la plage", "Les Champs-Élysées", "Strawberry Fields Forever",
    "A Day in the Life", "Heroes", "Life on Mars?", "Space Oddity",
    "The Times They Are a-Changin'", "Blowin' in the Wind",
};
#define N (int)(sizeof(titles) / sizeof(titles[0]))
volatile int sink;
/* WL 5: every title twice ("x x"): the marginal cost of a byte is
 * (WL5 - WL3) / (the added bytes), free of the per-call cost */
static char twice[N][96];

int main(void)
{
    struct srch_query q;
    int r, i;

    for (i = 0; i < N; i++)             /* built in every workload */
    {
        char *d = twice[i];
        const char *s;
        for (s = titles[i]; *s; )
            *d++ = *s++;
        *d++ = ' ';
        for (s = titles[i]; *s; )
            *d++ = *s++;
        *d = '\0';
    }

#if WL == 1
    srch_fold_query("h", &q);           /* first character, common */
#elif WL == 2
    srch_fold_query("ha", &q);          /* a refine */
#elif WL == 3
    srch_fold_query("q", &q);           /* rare: the hot loop alone */
#elif WL == 4
    srch_fold_query("the c", &q);       /* two words */
#elif WL == 5
    srch_fold_query("q", &q);           /* WL 3 over the doubled titles */
#else
    srch_fold_query("h", &q);
#endif
    for (r = 0; r < REPS; r++)
        for (i = 0; i < N; i++)
        {
#if WL == 5
            sink += srch_match(twice[i], &q);
#elif WL
            sink += srch_match(titles[i], &q);
#else
            sink += titles[i][0];
#endif
        }
    return 0;
}
