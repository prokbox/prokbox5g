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
 * iPod UI shell: SORTKEY_VERSION guard
 *
 * The database stores its tag files in ui_sortkey() order, and its magic
 * ends in SORTKEY_VERSION (apps/tagcache.c), so a firmware whose keys sort
 * differently must bump the version, or devices keep a database sorted the
 * old way. This test hashes ui_sortkey() output over a fixed corpus (at
 * the key length the commit sort uses, 96) and fails when the hash is not
 * the one recorded for the current SORTKEY_VERSION.
 *
 * When it fails after an intended change to sortkey.c: bump
 * SORTKEY_VERSION in apps/ipodui/sortkey.h, then set EXPECTED_VERSION and
 * EXPECTED_HASH below to the values this test prints.
 *
 * Built and run by tests/ipodui/run.sh against apps/ipodui/sortkey.c.
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
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include SORTKEY_H

#define EXPECTED_VERSION 1
#define EXPECTED_HASH    0xa0982d78u
/* #47: the grouping rules (ui_casefold_char() over U+0000..U+1FFF,
 * ui_groupkey() and ui_casefold_cmp() over the corpus), recorded with
 * UI_GROUPING_VERSION */
#define EXPECTED_GROUPING_VERSION 1
#define EXPECTED_GROUPING_HASH    0x9c7a5c48u

#define KEYLEN 96   /* UI_SORTKEY_LEN in apps/tagcache.c */

static const char *const corpus[] = {
    /* the test library's edge cases (tests/ui/setup_library.py) */
    "!!!", "10dd", "2Gem", "The Bellwethers", "The The", "The Zephyrs",
    "Theory of Tides", "Björvik", "Silfur Rós", "Émile Ardent", "Ångström Lane",
    "Zoë Marchetti", "aurora drift", "#9 Dream Machine", "(Untitled)",
    "¿Dónde Estás?", "The Alder Trio", "Amber Coast", "Basalt",
    "Various Artists", "Summer Tones Vol. 1",
    /* case, space and "the" handling */
    "ABRA", "abra", "  Leading Space", "Trailing Space  ",
    "Many    Inner\tSpaces", "the", "The", "The ", "THE END", "Them Crooked",
    "theme", "The  Double Space", "Thé Accent",
    /* Latin-1 and Latin Extended-A folds, two-letter folds */
    "Æsop", "æther", "Straße", "Þorn", "þing", "Ĳssel", "Œuvre", "œil",
    "Ölfus", "Ísjaki", "Úlfur", "Café Ñandú", "Frédéric Lamoureux",
    "Łódź", "Čajka", "Šárka", "Žižkov", "Ğazal", "Ħamrun", "ſ long s",
    "× times", "÷ divide", "ÿ y-diaeresis",
    /* digits and symbols, the "#" group and inner symbols */
    "0", "9 Lives", "007", "{brace}", "|pipe|", "}close", "~tilde",
    "a{b}c", "a|b", "a~b", "a-b", "a_b", "a.b", "a'b", "a\"b", "@home",
    "$money", "%percent", "&and", "*star", "+plus", "=equals", "?what",
    "[bracket]", "\\backslash", "^caret", "`grave",
    /* other scripts and malformed UTF-8 */
    "Ελληνικά", "Русский", "日本語", "한국어", "עברית", "العربية",
    "\xff\xfe broken", "abc\xc3", "\xc3\x28 bad", "\xe2\x82 cut",
    "tab\there", "new\nline", "\x01control",
    /* long names: truncation at the key length */
    "A very long album title that goes on and on well past the key length "
    "limit used by the database commit sort, to check truncation",
    "The Very Long Artist Name With The Article In Front That Keeps Going "
    "And Going Beyond Ninety Six Bytes Of Key",
    "Ééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééé",
    "",
};

static uint32_t fnv1a(uint32_t h, const void *data, size_t n)
{
    const unsigned char *p = data;
    while (n--)
    {
        h ^= *p++;
        h *= 16777619u;
    }
    return h;
}

int main(void)
{
    uint32_t h = 2166136261u;
    char key[KEYLEN];
    unsigned i;

    for (i = 0; i < sizeof(corpus) / sizeof(corpus[0]); i++)
    {
        int len = ui_sortkey(corpus[i], key, sizeof(key));
        char letter = ui_sortletter(corpus[i]);

        h = fnv1a(h, key, (size_t)len + 1);     /* with the terminator */
        h = fnv1a(h, &letter, 1);
    }
    /* NULL input */
    h = fnv1a(h, key, (size_t)ui_sortkey(NULL, key, sizeof(key)) + 1);

    {
        uint32_t g = 2166136261u;
        uint32_t c;
        unsigned j;

        for (c = 0; c < 0x2000; c++)
        {
            uint32_t f = ui_casefold_char(c);
            g = fnv1a(g, &f, sizeof(f));
        }
        for (i = 0; i < sizeof(corpus) / sizeof(corpus[0]); i++)
        {
            int len = ui_groupkey(corpus[i], key, sizeof(key));
            g = fnv1a(g, key, (size_t)len + 1);
            for (j = 0; j < sizeof(corpus) / sizeof(corpus[0]); j += 7)
            {
                int8_t r = (int8_t)ui_casefold_cmp(corpus[i], corpus[j]);
                r = r < 0 ? -1 : r > 0;
                g = fnv1a(g, &r, 1);
            }
            c = ui_casefold_hash(corpus[i]);
            g = fnv1a(g, &c, sizeof(c));
        }
        if (UI_GROUPING_VERSION != EXPECTED_GROUPING_VERSION
            || g != EXPECTED_GROUPING_HASH)
        {
            printf("FAIL: grouping hash 0x%08x, UI_GROUPING_VERSION %d; "
                   "recorded 0x%08x for version %d.\n"
                   "If the case folding or ui_groupkey() changed on purpose, "
                   "bump UI_GROUPING_VERSION in apps/ipodui/sortkey.h and "
                   "LIBFILE_GROUPING in apps/ipodui/libfile.h (lists "
                   "rewritten and tag files re-sorted once, no rebuild) and "
                   "record the new version and hash in %s.\n",
                   (unsigned)g, UI_GROUPING_VERSION,
                   (unsigned)EXPECTED_GROUPING_HASH,
                   EXPECTED_GROUPING_VERSION, __FILE__);
            return 1;
        }
        printf("grouping version %d, hash 0x%08x\n", UI_GROUPING_VERSION,
               (unsigned)g);
    }
    if (SORTKEY_VERSION != EXPECTED_VERSION || h != EXPECTED_HASH)
    {
        printf("FAIL: ui_sortkey() corpus hash 0x%08x, SORTKEY_VERSION %d; "
               "recorded 0x%08x for version %d.\n"
               "If the key order changed on purpose, bump SORTKEY_VERSION in "
               "apps/ipodui/sortkey.h (the database is rebuilt) and record "
               "the new version and hash in %s.\n",
               (unsigned)h, SORTKEY_VERSION, (unsigned)EXPECTED_HASH,
               EXPECTED_VERSION, __FILE__);
        return 1;
    }
    printf("sortkey version %d, corpus hash 0x%08x (%u names)\n",
           SORTKEY_VERSION, (unsigned)h,
           (unsigned)(sizeof(corpus) / sizeof(corpus[0])));
    return 0;
}
