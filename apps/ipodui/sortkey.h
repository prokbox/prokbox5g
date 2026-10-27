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
 * iPod UI shell
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
#ifndef _IPODUI_SORTKEY_H_
#define _IPODUI_SORTKEY_H_

#include <stdint.h>

/* 7G list collation. Pure C (only stdint/stdbool/string), so the host
 * tests in tests/ipodui/ compile it as is.
 *
 * Key rules (keys compare with strcmp, which compares unsigned bytes):
 *  - one leading "The " is dropped (case-insensitive, only if something
 *    follows), so "The Beckons" sorts as "beckons";
 *  - letters are lower-cased and Latin-1 / Latin Extended-A accents are
 *    folded (e -> e for e-acute, "ss" for sharp s, "ae" for ash, ...);
 *  - runs of white space become one ' ', leading/trailing space is dropped,
 *    control characters are dropped;
 *  - a name that does not start with a letter (after the above) is in the
 *    "#" group, which sorts after 'z' as in the 2.0.5 software (its key
 *    starts with '{'); digits and ASCII symbols map below 'a' inside a
 *    key ('{' '|' '}' '~' map to ':');
 *  - other characters (Greek, Cyrillic, CJK, ...) keep their UTF-8 bytes,
 *    which sort after 'z'; a name starting with one is in the "#" group.
 * NULL or empty input gives an empty key. */

/* Version of the key order. The database stores its tag files in this
 * order, and its magic (apps/tagcache.c) ends in this number, so a firmware
 * whose keys sort differently rebuilds the database (play counts and
 * resume points come back through the changelog). Bump it whenever
 * ui_sortkey() output changes: tests/ipodui/test_sortkey_version.c fails
 * until the version and the corpus hash stored there are updated. 1..95. */
#define SORTKEY_VERSION 1

/* Fills 'key' with a collation key for 'name': lower-cased, accents folded
 * (é->e), leading "the " removed, the "#" group (digits/symbols) after 'z'.
 * Keys compare with strcmp. Returns key length. */
int  ui_sortkey(const char *name, char *key, int keylen);
/* #47: the library's grouping rules, shared by every caller that decides
 * whether two names are one artist, album, genre or composer (library.c
 * str_eq(), the book matching of artcache.c):
 *  - ui_casefold_char(): simple Unicode case folding to lower case for
 *    U+0041..U+024F, Greek and Cyrillic (Turkish dotted / dotless i kept);
 *  - ui_casefold_cmp(): names compared by case-folded code points (sharp s
 *    = "ss"), accent-sensitive: "Élan" = "élan" = "ÉLAN", "Elan" = "ELAN",
 *    but "Elan" != "Élan" (they sort together, as on the 7G, and stay
 *    two rows). NULL compares as "". 0 when equal; the sign is an order;
 *  - ui_groupkey(): ui_sortkey() of the case-folded name: the same key for
 *    Basic Latin, Latin-1 and Extended-A (which ui_sortkey() folds
 *    anyway); Extended-B, Greek and Cyrillic upper case lowered, so names
 *    equal under ui_casefold_cmp() always get equal group keys (list
 *    order: group key, then ui_casefold_cmp(), keeps them adjacent). */
/* The grouping rules' version (#47): bump it with any change to the
 * case folding or ui_groupkey() (test_sortkey_version.c hashes both): the
 * database's tag files are re-sorted once (tagcache.c, no rebuild) and
 * the library's lists rewritten once (libfile.h LIBFILE_GROUPING, kept
 * equal to it). */
#define UI_GROUPING_VERSION 1
uint32_t ui_casefold_char(uint32_t c);
int  ui_casefold_cmp(const char *a, const char *b);
int  ui_groupkey(const char *name, char *key, int keylen);
/* A hash equal for names equal under ui_casefold_cmp() */
uint32_t ui_casefold_hash(const char *name);
/* Quick-scroll letter for a name: 'A'..'Z', or '#' for digits/symbols. */
char ui_sortletter(const char *name);

#endif /* _IPODUI_SORTKEY_H_ */
