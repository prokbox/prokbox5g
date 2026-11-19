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
 *  - full-width ASCII (U+FF01..U+FF5E) is ASCII first ("ＡＢＣ" = "abc");
 *    the ideographic space U+3000 is white space;
 *  - script groups, by the key's first byte (#235; bytes 0xF8 and above
 *    never occur in UTF-8, and a malformed input byte of that value is
 *    kept as 0xF7): Latin 'a'..'z', then kana 0xF8, Hangul 0xF9, kanji
 *    0xFA, then the "#" group 0xFE. A name whose first character is none
 *    of those (digit, symbol, other script) is in the "#" group, which
 *    sorts after every other as in the 2.0.5 software (numbers and
 *    symbols after Z); its key is 0xFE then the name's key;
 *  - inside a key every character maps on its own, so mixed names work
 *    (B'z = "b'z", AKB48, a kanji name, a kana name): digits and ASCII
 *    symbols below 'a' ('{' '|' '}' '~' map to ':'), letters, then other
 *    scripts' raw UTF-8 (Greek, Cyrillic, ...), then kana, Hangul, kanji;
 *  - kana (the documented order, owner-decided): gojuon order, one unit
 *    0xF8, 0x80 + w per kana (no shared lead for a run: a unit is 2
 *    bytes, so truncation never splits one), w the gojuon index of its
 *    base syllable: a=1 i u e o, ka=6 .. ko, sa, ta, na, ha, ma=31 .. mo
 *    =35, ya=36 yu yo, ra=39 .. ro=43, then the wa row wa=44 wi=45 we=46
 *    wo=47 and n=48 (wi and we stay in the wa row, as in the traditional
 *    table, so n is 48, not 46). Katakana folds to Hiragana (ka = KA),
 *    half-width Katakana to full width; small kana weigh as full size,
 *    voiced and semi-voiced as their base (ha = ba = pa), a following
 *    voicing mark (U+3099/309A, U+FF9E/FF9F) merges into the kana; va vi
 *    ve vo (U+30F7..30FA) are wa wi we wo, vu is u. The long-vowel mark
 *    U+30FC (and half-width U+FF70) takes the previous kana's vowel (KA +
 *    long = ka a; after n, n); the iteration marks U+309D/309E/30FD/30FE
 *    repeat the previous kana. Either mark is dropped when no kana is
 *    right before it (at the start, after a space or another script).
 *    Equal kana keys (ka/KA/ga, small/full) are told apart by
 *    ui_sortcmp()'s later levels, in code-point order;
 *  - Hangul syllables U+AC00..U+D7A3: 0xF9, 0x80 + L, 0x81 + V, 0x80 + T
 *    (initial, medial, final indices: code-point order, the South Korean
 *    dictionary order). Compatibility jamo standing alone: a consonant
 *    that can be an initial is that initial with V byte 0x80 (so the
 *    consonant sorts just before its first syllable); a final-only
 *    cluster is its first consonant; a vowel U+314F..U+3163 is that vowel
 *    with the silent initial (ㅏ = 아). Old Hangul and the conjoining jamo
 *    (U+1100..) keep their raw bytes ("#" group when first);
 *  - kanji (U+4E00..U+9FFF, extension A U+3400..U+4DBF, compatibility
 *    U+F900..U+FAFF): 0xFA plus their UTF-8 bytes, code-point (radical-
 *    stroke) order. This is the documented fallback: the firmware has no
 *    readings, so kanji sort by reading only through sort tags (Sort
 *    Artist, Sort Album: ui_sortcmp() keys on the sort value).
 * Keys are cut at whole units (a character's bytes, a kana / Hangul /
 * kanji unit), so a cut key is a prefix of the full one.
 * NULL or empty input gives an empty key. */

/* Version of the key order. The database stores its tag files in this
 * order, and its magic (apps/tagcache.c) ends in this number, so a firmware
 * whose keys sort differently rebuilds the database (play counts and
 * resume points come back through the changelog). Bump it whenever
 * ui_sortkey() output changes: tests/ipodui/test_sortkey_version.c fails
 * until the version and the corpus hash stored there are updated. Bump it
 * too when the class flags stored at scan time change meaning (2: #146,
 * books by genre and folder, ipodui/bookclass.h). 1..95.
 * 3: the 0.9.0 format (docs/changes-from-rockbox.md 3.1), ONE bump shared
 * by every 0.9.0 change that needs each file read again: the unplayable
 * reason in the flag word (#286, tagcache.h TAGCACHE_UI_UNPLAYABLE_*), ID3
 * genres 148-191 and joined multi-value artists (#163, #167), the sort
 * values inside the tag entries (#235). A 0.8.x library (2) is rebuilt
 * once, its statistics exported first (tagcache.c ui_upgrade_export()).
 * Until 0.9.0 ships no released firmware carries 3: a 0.9.0 package that
 * changes the key order or the scan-time data updates the guards' hashes
 * at 3 and does NOT bump again. */
#define SORTKEY_VERSION 3

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
/* Quick-scroll letter for a name: 'A'..'Z', or '#' for digits/symbols
 * (and kana, Hangul, kanji: ui_sortindex() has their rows). */
char ui_sortletter(const char *name);

/* #235: the one list order (design brief 4.2). Each row has a display
 * name and a sort value (Sort Artist ...; NULL or "" falls back to the
 * display name; a NULL display name compares as ""). Levels:
 * strcmp(ui_groupkey(sort)), then ui_casefold_cmp(sort), then
 * ui_casefold_cmp(display), then strcmp(display). 0 only for equal
 * display names with equal effective sort values. */
int  ui_sortcmp(const char *disp_a, const char *sort_a, const char *disp_b,
                const char *sort_b);
/* The sort value of a stored "display\0sort\0" tag string (the database
 * keeps both; an empty sort is "display\0\0"): the sort value, or
 * 'stored' itself when it is empty. NULL gives NULL. The caller
 * guarantees at least one readable byte after the first NUL. */
const char *ui_sort_of(const char *stored);
/* Quick-scroll index of a name: UI_SORTINDEX_LATIN + 0..25 (A..Z), then
 * the 10 gojuon rows (a ka sa ta na ha ma ya ra wa; ga under ka, small ya
 * under ya, wi/we/wo/n under wa), then the 14 basic Hangul initials (g n
 * d r m b s ng j ch k t p h; doubles under their single), then
 * UI_SORTINDEX_HASH last (kanji without a sort value, digits, symbols,
 * other scripts). Ids never go down along a list in key order. */
#define UI_SORTINDEX_LATIN   0
#define UI_SORTINDEX_KANA    26
#define UI_SORTINDEX_HANGUL  36
#define UI_SORTINDEX_HASH    50
#define UI_SORTINDEX_COUNT   51
int  ui_sortindex(const char *name);
/* The UTF-8 label of an index id ("A", "か", "ㄱ", "#"); "#" when out of
 * range. */
const char *ui_sortindex_label(int id);

#endif /* _IPODUI_SORTKEY_H_ */
