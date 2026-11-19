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
 * iPod UI shell: host tests for the sortkey.h contract (docs/phase4-brief.md)
 *
 *   int  ui_sortkey(const char *name, char *key, int keylen);
 *   char ui_sortletter(const char *name);
 *
 * Built and run by tests/ipodui/run.sh against apps/ipodui/sortkey.c.
 * SORTKEY_H names the header to include (set by run.sh).
 *
 * Checked: "The " is ignored (case-insensitive, "Theory" is not touched),
 * case and accents fold, names starting with a digit/symbol get '#' and
 * sort after 'z' (the 2.0.5 software lists them after Z, #34),
 * a shuffled list always sorts to the same order with non-decreasing
 * quick-scroll letters, NULL/empty input, truncation to keylen, malformed
 * UTF-8, and that the return value is the key length.
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
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include SORTKEY_H

#define KEYLEN 128

static int failures, checks;

#define CHECK(cond, ...) do { \
        checks++; \
        if (!(cond)) { \
            if (failures++ < 40) { \
                printf("FAIL %s:%d: ", __func__, __LINE__); \
                printf(__VA_ARGS__); \
                printf("\n"); \
            } \
        } \
    } while (0)

static unsigned rng = 0x9E3779B9u;
static int rnd(int n) /* 0..n-1, deterministic xorshift */
{
    rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
    return n > 0 ? (int)(rng % (unsigned)n) : 0;
}

/* key of name in a static ring of buffers (several live at once) */
static const char *K(const char *name)
{
    static char ring[8][KEYLEN];
    static int i;
    char *k = ring[i++ & 7];
    int len = ui_sortkey(name, k, KEYLEN);
    CHECK(len == (int)strlen(k), "ui_sortkey(\"%s\") returned %d, key has "
          "length %d", name ? name : "(null)", len, (int)strlen(k));
    return k;
}

static bool is_letter_or_hash(char c)
{
    return c == '#' || (c >= 'A' && c <= 'Z');
}

static void test_the(void)
{
    const char *k = K("The Beckons");
    CHECK(k[0] == 'b', "\"The Beckons\" key starts with 0x%02x, want 'b'",
          (unsigned char)k[0]);
    CHECK(!strcmp(K("The Beckons"), K("Beckons")),
          "\"The Beckons\" and \"Beckons\" differ");
    CHECK(!strcmp(K("the beckons"), K("Beckons")),
          "lower-case \"the \" not removed");
    CHECK(!strcmp(K("THE BECKONS"), K("beckons")),
          "upper-case \"THE \" not removed");
    CHECK(ui_sortletter("The Beckons") == 'B', "letter(The Beckons) = '%c'",
          ui_sortletter("The Beckons"));
    CHECK(ui_sortletter("The Zephyrs") == 'Z', "letter(The Zephyrs) = '%c'",
          ui_sortletter("The Zephyrs"));
    /* only a leading "the " (with the space) is dropped */
    CHECK(ui_sortletter("Theory of Tides") == 'T',
          "letter(Theory of Tides) = '%c'", ui_sortletter("Theory of Tides"));
    CHECK(strcmp(K("Theory of Tides"), K("ory of Tides")) != 0,
          "\"Theory\" lost its \"The\"");
    CHECK(ui_sortletter("The The") == 'T', "letter(The The) = '%c'",
          ui_sortletter("The The"));
    CHECK(ui_sortletter("Them Crooked") == 'T', "letter(Them Crooked) = '%c'",
          ui_sortletter("Them Crooked"));
    CHECK(strcmp(K("Basalt"), K("The Beckons")) < 0 &&
          strcmp(K("The Beckons"), K("Björvik")) < 0,
          "\"The Beckons\" not between Basalt and Björvik");
    CHECK(strcmp(K("The Beckons"), K("Tin Sparrow")) < 0,
          "\"The Beckons\" sorts under T");
}

static void test_case_accents(void)
{
    static const char *same[][2] = {
        { "ABRA", "abra" },
        { "Björvik", "Bjorvik" },
        { "BJÖRVIK", "bjorvik" },
        { "Silfur Rós", "Silfur Ros" },
        { "Émile Ardent", "emile ardent" },
        { "Ångström Lane", "Angstrom Lane" },
        { "Zoë Marchetti", "Zoe Marchetti" },
        { "Ölfus", "Olfus" },
        { "Ísjaki", "Isjaki" },
        { "Café Ñandú", "cafe nandu" },
        { "Frédéric Lamoureux", "Frederic Lamoureux" },
    };
    for (unsigned i = 0; i < sizeof(same) / sizeof(same[0]); i++)
        CHECK(!strcmp(K(same[i][0]), K(same[i][1])),
              "\"%s\" and \"%s\" have different keys", same[i][0],
              same[i][1]);

    static const struct { const char *name; char letter; } letters[] = {
        { "Émile Ardent", 'E' }, { "Ångström Lane", 'A' },
        { "Ölfus", 'O' }, { "Ísjaki Tones", 'I' }, { "aurora drift", 'A' },
        { "zoë", 'Z' }, { "Björvik", 'B' }, { "Úlfur", 'U' },
    };
    for (unsigned i = 0; i < sizeof(letters) / sizeof(letters[0]); i++)
        CHECK(ui_sortletter(letters[i].name) == letters[i].letter,
              "letter(%s) = '%c', want '%c'", letters[i].name,
              ui_sortletter(letters[i].name), letters[i].letter);

    /* folded names sort among their base letter */
    CHECK(strcmp(K("Echo Harbor"), K("Émile Ardent")) < 0 &&
          strcmp(K("Émile Ardent"), K("Fennel")) < 0,
          "Émile not between Echo and Fennel");
    CHECK(strcmp(K("Amber Coast"), K("Ångström Lane")) < 0 &&
          strcmp(K("Ångström Lane"), K("aurora drift")) < 0,
          "Ångström not between Amber and aurora");
}

static void test_symbols(void)
{
    static const char *hash[] = { "10dd", "!!!", "2Gem", "#9 Dream Machine",
                                  "99 Red Kites", "(Untitled)", "...And",
                                  "1, 2, 3", "@home", "~tilde", "{brace}",
                                  "\xce\xa9mega" /* Greek capital omega */ };
    static const char *alpha[] = { "a", "Aardvark", "ABRA", "Ångström",
                                   "The Beckons", "aurora drift", "zz",
                                   "Zoë", "Ölfus" };
    for (unsigned i = 0; i < sizeof(hash) / sizeof(hash[0]); i++)
    {
        CHECK(ui_sortletter(hash[i]) == '#', "letter(%s) = '%c', want '#'",
              hash[i], ui_sortletter(hash[i]));
        for (unsigned j = 0; j < sizeof(alpha) / sizeof(alpha[0]); j++)
            CHECK(strcmp(K(hash[i]), K(alpha[j])) > 0,
                  "\"%s\" does not sort after \"%s\"", hash[i], alpha[j]);
    }
}

/* Names with pairwise different keys, in their expected order, except that
 * the last N_HASH (the '#' group) may come in any order among themselves. */
#define N_HASH 4
static const char *ordered[] = {
    "Aardvark", "Amber Coast", "Ångström Lane", "aurora drift",
    "Basalt", "The Beckons", "The Bellwethers", "Björvik", "Cedar & Pine",
    "Clara Ostrander", "Echo Harbor", "Émile Ardent", "Fennel",
    "Glass Orchard", "Ísjaki", "Kestrel", "Ölfus", "Orchid Engine",
    "Quartz Lake", "Saltmarsh", "Silfur Rós", "The The", "Theory of Tides",
    "Tin Sparrow", "Umber", "Xylem", "Zephyr Lane", "The Zephyrs",
    "Zoë Marchetti",
    "!!!", "10dd", "2Gem", "#9 Dream Machine",
};
#define N_ORDERED ((int)(sizeof(ordered) / sizeof(ordered[0])))

struct row { const char *name; char key[KEYLEN]; int pos; };

static int cmp_row(const void *a, const void *b)
{
    const struct row *x = a, *y = b;
    int c = strcmp(x->key, y->key);
    return c ? c : x->pos - y->pos;     /* stable: input order on ties */
}

static void test_order(void)
{
    struct row rows[N_ORDERED];
    const char *first[N_ORDERED];

    for (int i = 0; i < N_ORDERED; i++)
        for (int j = i + 1; j < N_ORDERED; j++)
            CHECK(strcmp(K(ordered[i]), K(ordered[j])) != 0,
                  "\"%s\" and \"%s\" share a key", ordered[i], ordered[j]);

    for (int round = 0; round < 50; round++)
    {
        int perm[N_ORDERED];
        for (int i = 0; i < N_ORDERED; i++)
            perm[i] = i;
        for (int i = N_ORDERED - 1; i > 0; i--)
        {
            int j = rnd(i + 1), t = perm[i];
            perm[i] = perm[j]; perm[j] = t;
        }
        for (int i = 0; i < N_ORDERED; i++)
        {
            rows[i].name = ordered[perm[i]];
            rows[i].pos = i;
            ui_sortkey(rows[i].name, rows[i].key, KEYLEN);
        }
        qsort(rows, N_ORDERED, sizeof(rows[0]), cmp_row);

        if (round == 0)
            for (int i = 0; i < N_ORDERED; i++)
                first[i] = rows[i].name;
        bool same = true;
        for (int i = 0; i < N_ORDERED; i++)
            same &= rows[i].name == first[i];
        CHECK(same, "round %d: shuffled list sorted to a different order",
              round);

        for (int i = 0; i < N_ORDERED - N_HASH; i++)
            CHECK(rows[i].name == ordered[i], "round %d: position %d is "
                  "\"%s\", want \"%s\"", round, i, rows[i].name, ordered[i]);
        for (int i = N_ORDERED - N_HASH; i < N_ORDERED; i++)
            CHECK(ui_sortletter(rows[i].name) == '#',
                  "round %d: \"%s\" in the '#' group", round, rows[i].name);

        /* quick-scroll letters never go backwards down the list, and
         * '#' comes last */
        char prev = 'A';
        for (int i = 0; i < N_ORDERED; i++)
        {
            char l = ui_sortletter(rows[i].name);
            CHECK(is_letter_or_hash(l), "letter(%s) = 0x%02x", rows[i].name,
                  (unsigned char)l);
            CHECK(prev == '#' ? l == '#' : (l == '#' || l >= prev),
                  "letter goes back from '%c' to '%c' at \"%s\"",
                  prev, l, rows[i].name);
            prev = l;
        }
    }

    /* equal keys (Björvik/Bjorvik) keep their input order under a stable sort */
    struct row tie[3] = { { "Bjorvik", "", 0 }, { "Björvik", "", 1 },
                          { "BJORVIK", "", 2 } };
    for (int i = 0; i < 3; i++)
        ui_sortkey(tie[i].name, tie[i].key, KEYLEN);
    qsort(tie, 3, sizeof(tie[0]), cmp_row);
    CHECK(tie[0].pos == 0 && tie[1].pos == 1 && tie[2].pos == 2,
          "equal keys reordered");
}

static void test_edges(void)
{
    char key[KEYLEN];
    int len;

    memset(key, 'X', sizeof(key));
    len = ui_sortkey(NULL, key, KEYLEN);
    CHECK(len == 0 && key[0] == '\0', "NULL: len %d, key[0] 0x%02x", len,
          (unsigned char)key[0]);
    memset(key, 'X', sizeof(key));
    len = ui_sortkey("", key, KEYLEN);
    CHECK(len == 0 && key[0] == '\0', "empty: len %d, key[0] 0x%02x", len,
          (unsigned char)key[0]);
    CHECK(is_letter_or_hash(ui_sortletter(NULL)), "letter(NULL) = 0x%02x",
          (unsigned char)ui_sortletter(NULL));
    CHECK(is_letter_or_hash(ui_sortletter("")), "letter(\"\") = 0x%02x",
          (unsigned char)ui_sortletter(""));
    CHECK(strcmp(K(""), K("a")) < 0, "empty key not first");

    /* truncation: NUL inside keylen, nothing written past it */
    static const char *names[] = { "The Beckons", "Björvik Guðmundsdóttir",
                                   "!!!", "Zoë", "ÅÅÅÅÅÅÅÅ" };
    for (unsigned n = 0; n < sizeof(names) / sizeof(names[0]); n++)
        for (int kl = 1; kl <= 8; kl++)
        {
            memset(key, 'X', sizeof(key));
            len = ui_sortkey(names[n], key, kl);
            CHECK(len >= 0 && len < kl && key[len] == '\0' &&
                  (int)strlen(key) == len, "\"%s\" keylen %d: len %d",
                  names[n], kl, len);
            bool untouched = true;
            for (int i = kl; i < KEYLEN; i++)
                untouched &= key[i] == 'X';
            CHECK(untouched, "\"%s\" keylen %d: wrote past keylen", names[n],
                  kl);
        }

    /* long names */
    char longname[1000];
    memset(longname, 'q', sizeof(longname) - 1);
    longname[sizeof(longname) - 1] = '\0';
    len = ui_sortkey(longname, key, 64);
    CHECK(len > 0 && len < 64 && key[len] == '\0', "long name: len %d", len);

    /* malformed UTF-8 must not crash or overrun */
    static const char *bad[] = { "\xff\xfe" "abc", "\xc3", "ab\xc3",
                                 "\xe2\x82", "\x80\x80\x80", "The \xc3" };
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
    {
        memset(key, 'X', sizeof(key));
        len = ui_sortkey(bad[i], key, 16);
        CHECK(len >= 0 && len < 16 && key[len] == '\0',
              "malformed input %u: len %d", i, len);
        CHECK(is_letter_or_hash(ui_sortletter(bad[i])),
              "malformed input %u: letter 0x%02x", i,
              (unsigned char)ui_sortletter(bad[i]));
    }
}

/* #47: the shared grouping rules (ui_casefold_char/cmp, ui_groupkey) */
static int put8(unsigned c, char *o)
{
    if (c < 0x80) { o[0] = (char)c; o[1] = 0; return 1; }
    if (c < 0x800) { o[0] = (char)(0xc0 | (c >> 6));
                     o[1] = (char)(0x80 | (c & 0x3f)); o[2] = 0; return 2; }
    o[0] = (char)(0xe0 | (c >> 12)); o[1] = (char)(0x80 | ((c >> 6) & 0x3f));
    o[2] = (char)(0x80 | (c & 0x3f)); o[3] = 0; return 3;
}

static bool geq(const char *a, const char *b)
{
    char ka[KEYLEN], kb[KEYLEN];
    ui_groupkey(a, ka, sizeof(ka));
    ui_groupkey(b, kb, sizeof(kb));
    return !strcmp(ka, kb);
}

static void test_casefold(void)
{
    static const unsigned pairs[][2] = {
        { 'A', 'a' }, { 'Z', 'z' }, { 0xc0, 0xe0 }, { 0xc9, 0xe9 },
        { 0xd8, 0xf8 }, { 0xde, 0xfe }, { 0x100, 0x101 }, { 0x141, 0x142 },
        { 0x178, 0xff }, { 0x17d, 0x17e }, { 0x17f, 's' }, { 0x181, 0x253 },
        { 0x1c4, 0x1c6 }, { 0x1c5, 0x1c6 }, { 0x1cd, 0x1ce }, { 0x1f1, 0x1f3 },
        { 0x218, 0x219 }, { 0x23a, 0x2c65 }, { 0x24e, 0x24f }, { 0xb5, 0x3bc },
        { 0x386, 0x3ac }, { 0x391, 0x3b1 }, { 0x3a9, 0x3c9 }, { 0x3c2, 0x3c3 },
        { 0x401, 0x451 }, { 0x410, 0x430 }, { 0x42f, 0x44f }, { 0x462, 0x463 },
        { 0x4c0, 0x4cf }, { 0x4c1, 0x4c2 }, { 0x52e, 0x52f },
        { 0x1e00, 0x1e01 }, { 0x1ea0, 0x1ea1 }, { 0x1ef8, 0x1ef9 },
        { 0x1e9e, 0xdf }, { 0x345, 0x3b9 }, { 0x42f, 0x44f },
    };
    static const unsigned same[] = {
        'a', '0', ' ', 0xd7, 0xdf, 0xe9, 0xf7, 0x101, 0x130, 0x131, 0x138,
        0x149, 0x180, 0x1c6, 0x3b1, 0x3c9, 0x430, 0x451, 0x530, 0x2000,
    };
    static const char *eq[][2] = {
        { "\xc3\x89lan", "\xc3\xa9lan" }, { "\xc3\x89LAN", "\xc3\xa9lan" },
        { "Elan", "ELAN" }, { "Stra\xc3\x9f" "e", "STRASSE" },
        { "\xc3\x98rjan", "\xc3\xb8rjan" }, { "\xc5\x81ukasz", "\xc5\x82UKASZ" },
        { "\xc7\x84" "emal", "\xc7\x86" "emal" }, { "\xc7\x85" "emal", "\xc7\x86" "EMAL" },
        { "\xce\xa9mega", "\xcf\x89MEGA" }, { "\xd0\x9f\xd1\x80\xd0\xb8", "\xd0\xbf\xd0\xa0\xd0\x98" },
        { "The Elan", "the elan" }, { NULL, "" },
        { "\xd0\xaf\xd0\xb1\xd0\xbb\xd0\xbe\xd0\xba\xd0\xbe", "\xd1\x8f\xd0\xb1\xd0\xbb\xd0\xbe\xd0\xba\xd0\xbe" },
        { "\xe1\xba\x9e" "e", "sse" }, { "PH\xe1\xbb\xa4", "ph\xe1\xbb\xa5" },
    };
    static const char *ne[][2] = {
        { "Elan", "\xc3\x89lan" }, { "Elan", "The Elan" }, { "I", "\xc4\xb1" },
        { "i", "\xc4\xb0" }, { "a b", "a  b" }, { "Elan", "Elan " },
        { "Bjorvik", "Bj\xc3\xb6rvik" },
    };
    char a[8], b[8];
    unsigned i, c;

    for (i = 0; i < sizeof(pairs) / sizeof(pairs[0]); i++)
        CHECK(ui_casefold_char(pairs[i][0]) == pairs[i][1],
              "casefold U+%04X = U+%04X, want U+%04X", pairs[i][0],
              (unsigned)ui_casefold_char(pairs[i][0]), pairs[i][1]);
    for (i = 0; i < sizeof(same) / sizeof(same[0]); i++)
        CHECK(ui_casefold_char(same[i]) == same[i], "casefold U+%04X moved",
              same[i]);
    /* every character: folding is idempotent, and a character and its
     * fold compare equal and share a group key */
    for (c = 0x20; c < 0x2000; c++)
    {
        if (c >= 0x600 && c < 0x1e00)
            continue;
        unsigned f = ui_casefold_char(c);
        if (c == 0x7f || (c >= 0x80 && c < 0xa0))
            continue;
        CHECK(ui_casefold_char(f) == f, "casefold U+%04X not idempotent", c);
        put8(c, a);
        put8(f, b);
        CHECK(!ui_casefold_cmp(a, b), "cmp U+%04X vs its fold", c);
        CHECK(geq(a, b), "group key U+%04X vs its fold", c);
    }
    for (i = 0; i < sizeof(eq) / sizeof(eq[0]); i++)
    {
        CHECK(!ui_casefold_cmp(eq[i][0], eq[i][1]), "cmp %s = %s", eq[i][0] ? eq[i][0] : "NULL", eq[i][1]);
        CHECK(geq(eq[i][0], eq[i][1]), "group key %s = %s", eq[i][0] ? eq[i][0] : "NULL", eq[i][1]);
    }
    for (i = 0; i < sizeof(ne) / sizeof(ne[0]); i++)
        CHECK(ui_casefold_cmp(ne[i][0], ne[i][1])
              == -ui_casefold_cmp(ne[i][1], ne[i][0])
              && ui_casefold_cmp(ne[i][0], ne[i][1]),
              "cmp %s != %s", ne[i][0], ne[i][1]);
    /* the group key is ui_sortkey() for Latin: the list order is kept */
    CHECK(!strcmp(K("The \xc3\x89lan"), K("elan")) && geq("The \xc3\x89lan", "elan"),
          "group key of a Latin name");
}

/* -- #235: CJK collation ------------------------------------------------ */

/* each name's key strictly below the next one's */
static void check_increasing(const char *what, const char *const *names,
                             int n)
{
    for (int i = 0; i + 1 < n; i++)
        CHECK(strcmp(K(names[i]), K(names[i + 1])) < 0,
              "%s: \"%s\" not before \"%s\"", what, names[i], names[i + 1]);
}

#define N_OF(a) ((int)(sizeof(a) / sizeof((a)[0])))

static void test_kana(void)
{
    /* gojuon: every base syllable, in order (the weight table) */
    static const char *const gojuon[] = {
        "あ", "ああ", "あい", "い", "う", "え", "お",
        "か", "き", "く", "け", "こ", "さ", "し", "す", "せ", "そ",
        "た", "ち", "つ", "て", "と", "な", "に", "ぬ", "ね", "の",
        "は", "ひ", "ふ", "へ", "ほ", "ま", "み", "む", "め", "も",
        "や", "ゆ", "よ", "ら", "り", "る", "れ", "ろ",
        "わ", "ゐ", "ゑ", "を", "ん",
    };
    /* names with the same key (the later levels of ui_sortcmp() order
     * them) */
    static const char *const same[][2] = {
        { "カタカナ", "かたかな" },          /* Katakana = Hiragana */
        { "ガ", "か" }, { "が", "か" },      /* voiced = base */
        { "は", "ば" }, { "ば", "ぱ" },      /* ha = ba = pa */
        { "ぁ", "あ" }, { "ッ", "つ" },      /* small = full size */
        { "ゃゅょ", "やゆよ" }, { "ゎ", "わ" }, { "ゕゖ", "かけ" },
        { "ｶﾀｶﾅ", "かたかな" },              /* half width */
        { "ｶﾞｷﾞ", "がぎ" },                  /* + voicing mark */
        { "ﾊﾟﾝ", "ぱん" }, { "ｯ", "つ" }, { "ｦ", "を" },
        { "か\xe3\x82\x99", "か" },          /* combining voicing mark */
        { "カー", "かあ" },                  /* long vowel: the vowel */
        { "ラーメン", "らあめん" }, { "ｶﾞｰ", "かあ" },
        { "キー", "きい" }, { "ユーロ", "ゆうろ" }, { "ヨー", "よお" },
        { "ルー", "るう" }, { "ンー", "んん" }, { "ヲー", "をお" },
        { "ーあ", "あ" },                    /* dropped at the start */
        { "いすゞ", "いすす" }, { "こゝろ", "こころ" },  /* iteration */
        { "ヽあ", "あ" }, { "バナナヾ", "ばななな" },
        { "ヴ", "う" }, { "ゔ", "う" },
        { "ヷヸヹヺ", "わゐゑを" },
    };
    for (int i = 0; i < N_OF(same); i++)
        CHECK(!strcmp(K(same[i][0]), K(same[i][1])),
              "\"%s\" and \"%s\" have different keys", same[i][0],
              same[i][1]);
    check_increasing("gojuon", gojuon, N_OF(gojuon));
    /* the key's units */
    const unsigned char *k = (const unsigned char *)K("ゆず");
    CHECK(k[0] == 0xf8 && k[1] == 0x80 + 37 && k[2] == 0xf8
          && k[3] == 0x80 + 13 && k[4] == 0, "key of ゆず");
    k = (const unsigned char *)K("ん");
    CHECK(k[0] == 0xf8 && k[1] == 0x80 + 48 && k[2] == 0, "key of ん");
    /* long-vowel mark after a non-kana: dropped */
    CHECK(!strcmp(K("aー"), K("a")), "aー");
    CHECK(!strcmp(K("あ ーい"), K("あ い")), "ー after a space");
    /* ties: code-point order (small < full, unvoiced < voiced <
     * semi-voiced, Hiragana < Katakana) */
    CHECK(ui_sortcmp("ぁ", NULL, "あ", NULL) < 0, "ぁ < あ");
    CHECK(ui_sortcmp("は", NULL, "ば", NULL) < 0, "は < ば");
    CHECK(ui_sortcmp("ば", NULL, "ぱ", NULL) < 0, "ば < ぱ");
    CHECK(ui_sortcmp("か", NULL, "カ", NULL) < 0, "か < カ");
    /* code-point order puts every Hiragana before every Katakana */
    CHECK(ui_sortcmp("が", NULL, "カ", NULL) < 0, "が < カ");
}

static void test_hangul(void)
{
    static const char *const order[] = {
        "ㄱ", "가", "각", "갂", "간", "개", "거", "고", "기", "깋",
        "ㄲ", "까", "ㄴ", "나", "ㄷ", "다", "ㄸ", "따", "ㄹ", "라", "ㅁ", "마",
        "ㅂ", "바", "ㅃ", "빠", "ㅅ", "사", "ㅆ", "싸", "ㅇ", "아", "애", "야",
        "이", "ㅈ", "자", "ㅉ", "짜", "ㅊ", "차", "ㅋ", "카", "ㅌ", "타",
        "ㅍ", "파", "ㅎ", "하", "힣",
    };
    static const char *const same[][2] = {
        { "ㅏ", "아" }, { "ㅣ", "이" }, { "ㅢ", "의" },  /* bare vowels */
        { "ㄳ", "ㄱ" }, { "ㄶ", "ㄴ" }, { "ㅀ", "ㄹ" },  /* final clusters */
        { "ㄺ", "ㄹ" }, { "ㅄ", "ㅂ" },
    };
    check_increasing("hangul", order, N_OF(order));
    for (int i = 0; i < N_OF(same); i++)
        CHECK(!strcmp(K(same[i][0]), K(same[i][1])),
              "\"%s\" and \"%s\" have different keys", same[i][0],
              same[i][1]);
    const unsigned char *k = (const unsigned char *)K("한");
    /* U+D55C: L 18, V 0, T 4 */
    CHECK(k[0] == 0xf9 && k[1] == 0x80 + 18 && k[2] == 0x81 && k[3] == 0x84
          && k[4] == 0, "key of 한");
    /* conjoining jamo stay raw, in the '#' group */
    CHECK(((const unsigned char *)K("\xe1\x84\x80"))[0] == 0xfe,
          "U+1100 not in the '#' group");
}

static void test_kanji_fullwidth(void)
{
    /* kanji: code-point order (radical-stroke) */
    static const char *const order[] = {
        "\xe3\x90\x80" /* U+3400 */, "一", "東京", "東京事変", "椎名林檎",
        "\xe9\xbf\xbf" /* U+9FFF */, "\xef\xa4\x80" /* U+F900 */,
    };
    check_increasing("kanji", order, N_OF(order));
    const unsigned char *k = (const unsigned char *)K("東");
    CHECK(k[0] == 0xfa && !strcmp((const char *)k + 1, "東"), "key of 東");
    /* full-width ASCII */
    CHECK(!strcmp(K("ＡＢＣ"), K("abc")), "ＡＢＣ != abc");
    CHECK(!strcmp(K("Ｂ＇ｚ"), K("B'z")), "Ｂ＇ｚ != B'z");
    CHECK(!strcmp(K("１２３"), K("123")), "１２３ != 123");
    CHECK(ui_sortletter("Ｚｅｎ") == 'Z', "letter(Ｚｅｎ)");
    CHECK(!strcmp(K("あ　い"), K("あ い")), "ideographic space");
}

static void test_groups(void)
{
    /* A-Z < kana < Hangul < kanji < '#' */
    static const char *const groups[5][6] = {
        { "AKB48", "B'z", "Zoë", "zzz", NULL },
        { "あいみょん", "ゆず", "カーネーション", "ｱｲ", "ん", NULL },
        { "ㄱ", "가수", "힙합", "ㅣ", NULL },
        { "東京事変", "一", "\xef\xa4\x80", NULL },
        { "123", "!!!", "\xce\xa9mega", "\xe1\x84\x80", "\xff bad", NULL },
    };
    static const unsigned char lead[5] = { 0, 0xf8, 0xf9, 0xfa, 0xfe };
    for (int g = 0; g < 5; g++)
        for (int i = 0; groups[g][i]; i++)
        {
            unsigned char b = (unsigned char)K(groups[g][i])[0];
            CHECK(g == 0 ? (b >= 'a' && b <= 'z') : b == lead[g],
                  "\"%s\": group byte 0x%02x", groups[g][i], b);
            for (int h = g + 1; h < 5; h++)
                for (int j = 0; groups[h][j]; j++)
                    CHECK(strcmp(K(groups[g][i]), K(groups[h][j])) < 0,
                          "\"%s\" not before \"%s\"", groups[g][i],
                          groups[h][j]);
        }
    /* mixed names, each character on its own */
    CHECK(!strcmp(K("AKB48"), "akb48"), "AKB48 key \"%s\"", K("AKB48"));
    CHECK(!strcmp(K("B'z"), "b'z"), "B'z key");
    static const char *const inside[] = {
        "a1", "az", "a\xce\xa9", "aあ", "a가", "a東",
    };
    check_increasing("inside a key", inside, N_OF(inside));
    /* a malformed byte never makes a group lead */
    const unsigned char *k = (const unsigned char *)K("a\xf8\xfa\xfe");
    CHECK(k[1] == 0xf7 && k[2] == 0xf7 && k[3] == 0xf7, "raw bytes kept");
    /* quick-scroll letters of CJK names */
    CHECK(ui_sortletter("ゆず") == '#' && ui_sortletter("가수") == '#'
          && ui_sortletter("東京事変") == '#', "CJK letter not '#'");
}

static const char *const cjk_corpus[] = {
    "ゆず", "ユズ", "ﾕｽﾞ", "カーネーション", "かあねえしょん", "東京事変",
    "東京", "가수", "ㄱ", "ㅏ", "아", "ＡＢＣ", "abc", "ABC", "B'z", "AKB48",
    "いすゞ", "いすず", "Ωmega", "ωMEGA", "Élan", "élan", "Elan", "1東",
};

static void test_cjk_grouping(void)
{
    /* #47: names equal under ui_casefold_cmp() share a group key */
    for (int i = 0; i < N_OF(cjk_corpus); i++)
        for (int j = 0; j < N_OF(cjk_corpus); j++)
            if (!ui_casefold_cmp(cjk_corpus[i], cjk_corpus[j]))
                CHECK(geq(cjk_corpus[i], cjk_corpus[j]),
                      "group key %s = %s", cjk_corpus[i], cjk_corpus[j]);
    CHECK(geq("ゆず", "ユズ") && geq("ＡＢＣ", "abc"), "group key folds");
    /* every code point of the CJK ranges: a character and its fold
     * share a group key, and the key is a whole unit */
    static const unsigned ranges[][2] = {
        { 0x3000, 0x31ff }, { 0xac00, 0xad00 }, { 0xd700, 0xd7ff },
        { 0x4e00, 0x4e40 }, { 0xf900, 0xf920 }, { 0xff00, 0xffef },
    };
    char a[8], ka[KEYLEN], kb[KEYLEN];
    for (int r = 0; r < N_OF(ranges); r++)
        for (unsigned c = ranges[r][0]; c <= ranges[r][1]; c++)
        {
            put8(c, a);
            int la = ui_sortkey(a, ka, sizeof(ka));
            int lb = ui_groupkey(a, kb, sizeof(kb));
            CHECK(la == lb && !strcmp(ka, kb), "U+%04X: group key", c);
            CHECK(la == 0 || (unsigned char)ka[0] != 0xfe
                  || ((unsigned char)ka[1] < 0xf8),
                  "U+%04X: '#' then a group lead", c);
        }
}

static void test_sortcmp(void)
{
    /* the sort value wins over the display order: 東京事変 (U+6771)
     * displays before 椎名林檎 (U+690E) by code point, sorts after it
     * by reading (to < shi) */
    CHECK(strcmp(K("東京事変"), K("椎名林檎")) < 0, "kanji code-point order");
    CHECK(ui_sortcmp("東京事変", "とうきょうじへん", "椎名林檎",
                     "しいなりんご") > 0, "sort value not used");
    CHECK(ui_sortcmp("椎名林檎", "しいなりんご", "東京事変",
                     "とうきょうじへん") < 0, "sort value not used (swapped)");
    CHECK(ui_sortcmp("椎名林檎", NULL, "東京事変", NULL) > 0,
          "display fallback");
    CHECK(ui_sortcmp("Beatles", "Beatles", "Abba", "") > 0, "empty sort");
    /* level 1 equal, level 2: the case-folded sort values */
    CHECK(ui_sortcmp("x", "Elan", "y", "Élan") < 0, "level 2");
    CHECK(ui_sortcmp("y", "ゆず", "x", "ユズ") < 0, "level 2 (kana)");
    /* levels 1-2 equal, level 3: the case-folded display names */
    CHECK(ui_sortcmp("b", "same", "a", "SAME") > 0, "level 3");
    /* levels 1-3 equal, level 4: the bytes */
    CHECK(ui_sortcmp("ELAN", "e", "elan", "E") < 0, "level 4");
    CHECK(ui_sortcmp("elan", "e", "elan", "e") == 0, "equal");
    CHECK(ui_sortcmp("x", "", "x", NULL) == 0, "\"\" sort = NULL sort");
    CHECK(ui_sortcmp(NULL, NULL, "", NULL) == 0, "NULL display");
    CHECK(ui_sortcmp(NULL, NULL, "a", NULL) < 0, "NULL display first");
    /* antisymmetric over the corpus */
    for (int i = 0; i < N_OF(cjk_corpus); i++)
        for (int j = 0; j < N_OF(cjk_corpus); j++)
        {
            int x = ui_sortcmp(cjk_corpus[i], NULL, cjk_corpus[j], NULL);
            int y = ui_sortcmp(cjk_corpus[j], NULL, cjk_corpus[i], NULL);
            CHECK((x < 0) == (y > 0) && (x == 0) == (y == 0)
                  && (x == 0) == (i == j || !strcmp(cjk_corpus[i],
                                                     cjk_corpus[j])),
                  "sortcmp %s / %s", cjk_corpus[i], cjk_corpus[j]);
        }

    /* ui_sort_of(): "display\0sort\0" */
    static const char both[] = "Beatles\0Beatles, The\0";
    static const char none[] = "Abba\0\0";
    static const char nodisp[] = "\0x\0";
    CHECK(!strcmp(ui_sort_of(both), "Beatles, The"), "sort_of(both)");
    CHECK(ui_sort_of(none) == none, "sort_of(none)");
    CHECK(!strcmp(ui_sort_of(nodisp), "x"), "sort_of(no display)");
    CHECK(ui_sort_of(NULL) == NULL, "sort_of(NULL)");
}

static void test_sortindex(void)
{
    static const struct { const char *name; int id; } ids[] = {
        { "Abba", 0 }, { "The Zephyrs", 25 }, { "Ｚｅｎ", 25 },
        { "あいみょん", 26 }, { "ぁ", 26 }, { "がっこう", 27 }, { "ｶ", 27 },
        { "サ", 28 }, { "ぢ", 29 }, { "な", 30 }, { "ぱ", 31 }, { "も", 32 },
        { "ゃ", 33 }, { "ユズ", 33 }, { "ろ", 34 }, { "わ", 35 },
        { "ゐ", 35 }, { "を", 35 }, { "ん", 35 }, { "ーア", 26 },
        { "ㄱ", 36 }, { "까", 36 }, { "ㄳ", 36 }, { "나", 37 }, { "따", 38 },
        { "라", 39 }, { "마", 40 }, { "빠", 41 }, { "싸", 42 }, { "ㅏ", 43 },
        { "아", 43 }, { "짜", 44 }, { "차", 45 }, { "카", 46 }, { "타", 47 },
        { "파", 48 }, { "힣", 49 },
        { "東京事変", 50 }, { "123", 50 }, { "\xce\xa9mega", 50 },
        { "", 50 }, { NULL, 50 },
    };
    for (int i = 0; i < N_OF(ids); i++)
        CHECK(ui_sortindex(ids[i].name) == ids[i].id,
              "sortindex(%s) = %d, want %d", ids[i].name ? ids[i].name
              : "NULL", ui_sortindex(ids[i].name), ids[i].id);
    static const struct { int id; const char *label; } labels[] = {
        { 0, "A" }, { 25, "Z" }, { 26, "あ" }, { 27, "か" }, { 33, "や" },
        { 35, "わ" }, { 36, "ㄱ" }, { 43, "ㅇ" }, { 49, "ㅎ" }, { 50, "#" },
        { -1, "#" }, { 51, "#" },
    };
    for (int i = 0; i < N_OF(labels); i++)
        CHECK(!strcmp(ui_sortindex_label(labels[i].id), labels[i].label),
              "label(%d) = %s, want %s", labels[i].id,
              ui_sortindex_label(labels[i].id), labels[i].label);
    /* every label is its own row */
    for (int id = 0; id < UI_SORTINDEX_COUNT - 1; id++)
        CHECK(ui_sortindex(ui_sortindex_label(id)) == id,
              "sortindex(label(%d)) = %d", id,
              ui_sortindex(ui_sortindex_label(id)));
    /* ids never go down along a list in key order */
    static const char *const list[] = {
        "Abba", "Zoë", "あ", "か", "が", "ゃ", "ん", "ㄱ", "가", "ㄲ", "나",
        "하", "힣", "一", "東京", "!!!", "123",
    };
    for (int i = 0; i + 1 < N_OF(list); i++)
        CHECK(strcmp(K(list[i]), K(list[i + 1])) <= 0
              && ui_sortindex(list[i]) <= ui_sortindex(list[i + 1]),
              "index goes back at %s / %s", list[i], list[i + 1]);
}

static void test_cjk_truncation(void)
{
    static const char *const names[] = {
        "東京事変", "가나다라", "ああああ", "1東京", "カーカー", "ｶﾞｶﾞｶﾞ",
        "a東b가cあ",
    };
    char full[KEYLEN], key[KEYLEN];
    for (int i = 0; i < N_OF(names); i++)
    {
        int fl = ui_sortkey(names[i], full, sizeof(full));
        for (int kl = 1; kl <= 16; kl++)
        {
            memset(key, 'X', sizeof(key));
            int len = ui_sortkey(names[i], key, kl);
            CHECK(len >= 0 && len < kl && key[len] == '\0'
                  && len <= fl && !memcmp(key, full, len),
                  "\"%s\" keylen %d: len %d, not a prefix", names[i], kl,
                  len);
            CHECK(key[kl] == 'X', "\"%s\" keylen %d: wrote past", names[i],
                  kl);
            /* never mid-unit: the cut is where the full key has a unit
             * boundary (the next byte is a lead, ' ', or the end) */
            unsigned char nx = (unsigned char)full[len];
            CHECK(len == 0 || nx == 0 || nx == ' ' || nx < 0x80
                  || nx >= 0xf8, "\"%s\" keylen %d: cut mid-unit at %d",
                  names[i], kl, len);
        }
    }
    /* 96 bytes of kanji: whole 4-byte units */
    char longname[3 * 40 + 1];
    for (int i = 0; i < 40; i++)
        memcpy(longname + 3 * i, "東", 3);
    longname[120] = '\0';
    int len = ui_sortkey(longname, key, 96);
    CHECK(len == 92, "40 kanji at 96: len %d, want 92", len);
}

int main(void)
{
    test_the();
    test_case_accents();
    test_symbols();
    test_order();
    test_edges();
    test_casefold();
    test_kana();
    test_hangul();
    test_kanji_fullwidth();
    test_groups();
    test_cjk_grouping();
    test_sortcmp();
    test_sortindex();
    test_cjk_truncation();
    printf("   sortkey: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
