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

/* Host test of apps/ipodui/search_core.c (docs/phase11-brief.md 3.1):
 * the query fold, word-prefix matching over a corpus (accents, "The",
 * multi-word, UTF-8, separators), and srch_extends(). GPL v2+ */
#include <stdio.h>
#include <string.h>
#include "ipodui/search.h"

static int failures, checks;

#define CHECK(cond, ...) do { \
        checks++; \
        if (!(cond)) { \
            failures++; \
            printf("FAIL %s:%d: ", __FILE__, __LINE__); \
            printf(__VA_ARGS__); \
            putchar('\n'); \
        } \
    } while (0)

static void fold_is(const char *in, const char *want)
{
    struct srch_query q;
    int n = srch_fold_query(in, &q);

    CHECK(!strcmp(q.folded, want) && n == (int)strlen(want) && q.len == n,
          "fold(\"%s\") = \"%s\" (%d), want \"%s\"", in, q.folded, n, want);
}

static void match_is(const char *query, const char *hay, int want)
{
    struct srch_query q;

    srch_fold_query(query, &q);
    CHECK(srch_match(hay, &q) == want, "match(\"%s\", \"%s\") != %d",
          query, hay, want);
}

/* a corpus of labels: query -> the labels it must find (word prefix) */
static const char *const corpus[] = {
    "Haiku", "Halcyon", "Hey Juno", "The Hardest Part", "Oh Happy Day",
    "Thinking", "Bohemian Rhapsody", "Chalk", "Alpha", "AC/DC",
    "Beyoncé", "Édith Piaf", "Mötley Crüe", "Œuvres", "Straße",
    "Silfur Rós", "Björvik", "Ærosmith", "Dvořák", "Łódź",
    "Пётр Чайковский", "坂本龍一", "  Spaced   Out  ", "rock'n'roll",
    "99 Luftballons", "(What's the Story) Morning Glory?",
    "Hello\xc2\xa0World", "\xc3", "a\xc3" "b",
};
#define CORPUS_N (int)(sizeof(corpus) / sizeof(corpus[0]))

static void finds(const char *query, const char *const *want, int n)
{
    struct srch_query q;
    int i, j, got = 0;

    srch_fold_query(query, &q);
    for (i = 0; i < CORPUS_N; i++)
    {
        bool m = srch_match(corpus[i], &q), w = false;

        for (j = 0; j < n; j++)
            if (!strcmp(want[j], corpus[i]))
                w = true;
        CHECK(m == w, "\"%s\" on \"%s\": got %d want %d", query, corpus[i],
              m, w);
        got += m;
    }
    CHECK(got == n, "\"%s\": %d results, want %d", query, got, n);
}

#define FINDS(q, ...) do { \
        static const char *const w_[] = { __VA_ARGS__ }; \
        finds(q, w_, (int)(sizeof(w_) / sizeof(w_[0]))); \
    } while (0)

int main(void)
{
    struct srch_query a, b;
    char longq[80];

    /* fold: case, accents, two-letter folds, separators, no "The" drop */
    fold_is("Hello", "hello");
    fold_is("  The   Beckons ", "the beckons");
    fold_is("Beyoncé", "beyonce");
    fold_is("Ærø", "aero");
    fold_is("Straße", "strasse");
    fold_is("AC/DC", "ac dc");
    fold_is("rock'n'roll", "rock n roll");
    fold_is("Œ", "oe");
    fold_is("", "");
    fold_is("!!!", "");
    fold_is("ДА", "да");                /* #47: case folded (sortkey.h) */
    fold_is("Яблоко", "яблоко");
    fold_is("ΩMEGA", "ωmega");
    fold_is("PHỤ", "phụ");              /* Vietnamese: case, not accent */
    fold_is("日本", "日本");            /* other scripts keep their bytes */
    match_is("яблоко", "Яблоко Красное", 1);
    match_is("красное", "Яблоко КРАСНОЕ", 1);
    match_is("ωμέγα", "ΩΜΈΓΑ", 1);
    match_is("phụ", "Bài PHỤ", 1);
    match_is("ss", "STRAẞE", 0);       /* a word prefix, not inside */
    match_is("strasse", "STRAẞE", 1);
    /* truncation at SRCH_MAX_Q, never inside a character */
    memset(longq, 'x', sizeof(longq));
    longq[sizeof(longq) - 1] = '\0';
    srch_fold_query(longq, &a);
    CHECK(a.len == SRCH_MAX_Q && a.folded[SRCH_MAX_Q] == '\0', "cap %d",
          a.len);
    memset(longq, 'x', SRCH_MAX_Q - 1);
    strcpy(longq + SRCH_MAX_Q - 1, "Ж");  /* 2 bytes: one too many */
    srch_fold_query(longq, &a);
    CHECK(a.len == SRCH_MAX_Q - 1, "cut mid-character: %d", a.len);
    memset(longq, 'x', SRCH_MAX_Q - 1);
    strcpy(longq + SRCH_MAX_Q - 1, "ß");  /* folds to 2 bytes */
    srch_fold_query(longq, &a);
    CHECK(a.len == SRCH_MAX_Q - 1, "cut mid-fold: %d", a.len);

    /* word-prefix (⚠️ S1) */
    FINDS("h", "Haiku", "Halcyon", "Hey Juno", "The Hardest Part",
          "Oh Happy Day", "Hello\xc2\xa0World");
    FINDS("ha", "Haiku", "Halcyon", "The Hardest Part", "Oh Happy Day");
    FINDS("hai", "Haiku");
    FINDS("the", "The Hardest Part", "(What's the Story) Morning Glory?");
    FINDS("th", "The Hardest Part", "Thinking",
          "(What's the Story) Morning Glory?");
    FINDS("dc", "AC/DC");
    FINDS("ac dc", "AC/DC");
    FINDS("e", "Édith Piaf");
    FINDS("beyonce", "Beyoncé");
    FINDS("cr", "Mötley Crüe");
    FINDS("oeu", "Œuvres");
    FINDS("strass", "Straße");
    FINDS("ros", "Silfur Rós");
    FINDS("bjorvik", "Björvik");
    FINDS("aer", "Ærosmith");
    FINDS("dvorak", "Dvořák");
    FINDS("lodz", "Łódź");
    FINDS("Чай", "Пётр Чайковский");
    FINDS("坂本", "坂本龍一");
    FINDS("spaced out", "  Spaced   Out  ");
    FINDS("n roll", "rock'n'roll");
    FINDS("99", "99 Luftballons");
    FINDS("morning glory", "(What's the Story) Morning Glory?");
    FINDS("world", "Hello\xc2\xa0World");
    /* multi-word: in order, distinct words */
    FINDS("hey j", "Hey Juno");
    FINDS("oh day", "Oh Happy Day");
    match_is("day oh", "Oh Happy Day", 0);
    match_is("ha ha", "Haha", 0);
    match_is("ha ha", "Ha Ha Ha", 1);
    match_is("happy", "Oh Happy Day", 1);
    match_is("appy", "Oh Happy Day", 0);        /* not a substring match */
    match_is("hapx", "Oh Happy Day", 0);
    match_is("", "Anything", 0);
    match_is("a", "", 0);
    match_is("b", "a\xc3" "b", 0);              /* malformed: a word char */
    match_is("a", "\xc3", 0);
    CHECK(!srch_match(NULL, &a), "NULL haystack");

    /* the stop table follows the query's first letter */
    match_is("x", "Xylophone", 1);
    match_is("y", "Xylophone", 0);
    match_is("x", "Xylophone", 1);

    /* extends: appended letters or words only */
    srch_fold_query("ha", &a);
    srch_fold_query("hai", &b);
    CHECK(srch_extends(&a, &b), "ha -> hai");
    srch_fold_query("ha k", &b);
    CHECK(srch_extends(&a, &b), "ha -> ha k");
    srch_fold_query("ha ", &b);
    CHECK(srch_extends(&a, &b) && b.len == a.len, "ha -> \"ha \"");
    srch_fold_query("h", &b);
    CHECK(!srch_extends(&a, &b), "ha -> h (delete)");
    srch_fold_query("hb", &b);
    CHECK(!srch_extends(&a, &b), "ha -> hb");
    srch_fold_query("", &a);
    srch_fold_query("h", &b);
    CHECK(!srch_extends(&a, &b), "empty -> h");

    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
