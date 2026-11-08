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

/* #146 host test: audiobook/podcast classification (apps/ipodui/bookclass.h) */
#include <stdio.h>
#include <stdint.h>
#include <strings.h>
#include "bookclass.h"
#include "sortkey.h"

/* The class flags are stored at scan time; a change of the rules must bump
 * SORTKEY_VERSION (the database magic) so devices rebuild once. The hash
 * below covers every row of the table; when it changes on purpose, bump
 * SORTKEY_VERSION and set both values to what this test prints. */
#define EXPECTED_VERSION 2
#define EXPECTED_HASH    0xfce5c0ffu

#define M BOOKCLASS_MUSIC
#define B BOOKCLASS_BOOK
#define P BOOKCLASS_PODCAST

struct row {
    int is_book;            /* mp3entry.is_audiobook (m4b, MP4 book flag) */
    const char *genre, *path;
    enum bookclass want;
};

static const struct row rows[] = {
    /* type */
    { 1, NULL,          "/Books/Dune.m4b",                         B },
    { 1, "Fiction",     "/Music/x/Dune.m4b",                       B },
    /* genre: every English name, mixed case, padded */
    { 0, "Audiobook",   "/Music/a/01.mp3",                         B },
    { 0, "audiobooks",  "/Music/a/01.mp3",                         B },
    { 0, "AUDIO BOOK",  "/Music/a/01.mp3",                         B },
    { 0, "Audio Books", "/Music/a/01.mp3",                         B },
    { 0, "book",        "/Music/a/01.mp3",                         B },
    { 0, "Books",       "/Music/a/01.mp3",                         B },
    { 0, "Spoken Word", "/Music/a/01.mp3",                         B },
    { 0, "spoken & audio", "/Music/a/01.mp3",                      B },
    { 0, "Books & Spoken", "/Music/a/01.mp3",                      B },
    { 0, "  Audiobook ", "/Music/a/01.mp3",                        B },
    { 0, "Audiobook\r\n", "/Music/a/01.mp3",                       B },
    { 0, "\tBooks",     "/Music/a/01.mp3",                         B },
    /* genre: de / nl, literal UTF-8 and ASCII spellings */
    { 0, "H\xc3\xb6rbuch", "/Musik/a/01.mp3",                      B },
    { 0, "h\303\266rb\303\274cher", "/Musik/a/01.mp3",          B },
    { 0, "Hoerbuch",    "/Musik/a/01.mp3",                         B },
    { 0, "hoerbuecher", "/Musik/a/01.mp3",                         B },
    { 0, "Gesprochenes Wort", "/Musik/a/01.mp3",                   B },
    { 0, "Luisterboek", "/Muziek/a/01.mp3",                        B },
    { 0, "LUISTERBOEKEN", "/Muziek/a/01.mp3",                      B },
    { 0, "Gesproken Woord", "/Muziek/a/01.mp3",                    B },
    /* genre: non-matches (exact only, no substrings) */
    { 0, "Bookshelf",   "/Music/a/01.mp3",                         M },
    { 0, "Audiobook Club", "/Music/a/01.mp3",                      M },
    { 0, "Book of Love", "/Music/a/01.mp3",                        M },
    { 0, "Hoerspiel",   "/Musik/a/01.mp3",                         M },
    { 0, "H\xc3\xb6rspiel", "/Musik/a/01.mp3",                     M },
    { 0, "Speech",      "/Music/a/01.mp3",                         M },
    { 0, "Rock",        "/Music/a/01.mp3",                         M },
    { 0, "",            "/Music/a/01.mp3",                         M },
    { 0, NULL,          "/Music/a/01.mp3",                         M },
    { 0, NULL,          NULL,                                      M },
    /* folder: any depth, any case, both names; tag ignored */
    { 0, "Fiction",     "/audiobooks/x/01.mp3",                    B },
    { 0, NULL,          "/Audiobooks/Author/Book/01.mp3",          B },
    { 0, "Rock",        "/Audiobooks/Album/01.mp3",                B },
    { 0, NULL,          "/Audio Books/Book/CD1/01.mp3",            B },
    { 0, NULL,          "/Audio Books/Book/CD2/01.mp3",            B },
    { 0, NULL,          "/Music/AUDIOBOOKS/Book/01.mp3",           B },
    { 0, NULL,          "/Audiobooks/01.mp3",                      B },
    /* folder: non-matches (a component, not a substring) */
    { 0, NULL,          "/My Audiobooks/x/01.mp3",                 M },
    { 0, NULL,          "/Audiobooks2/x/01.mp3",                   M },
    { 0, NULL,          "/Music/Audiobooks.mp3",                   M },
    { 0, NULL,          "/Audiobook/x/01.mp3",                     M },
    { 0, NULL,          "/AudioBooksX/01.mp3",                     M },
    /* podcast wins */
    { 0, "Podcast",     "/Audiobooks/Show/01.mp3",                 P },
    { 1, "podcasts",    "/Books/ep.m4b",                           P },
    { 0, "Audiobook",   "/Podcasts/Show/01.mp3",                   P },
    { 0, NULL,          "/podcasts/Show/01.mp3",                   P },
    { 0, NULL,          "/Audiobooks/Podcasts/01.mp3",             P },
    /* a podcast in /Audiobooks/ without a podcast tag is a book */
    { 0, "News",        "/Audiobooks/Show/01.mp3",                 B },
    { 0, "Podcast Talk", "/Music/a/01.mp3",                        M },
};

int main(void)
{
    unsigned i, fails = 0, n = sizeof(rows) / sizeof(rows[0]);
    uint32_t h = 2166136261u;

    for (i = 0; i < n; i++)
    {
        const struct row *r = &rows[i];
        enum bookclass got = bookclass_of(r->is_book, r->genre, r->path);

        if (got != r->want)
        {
            printf("FAIL: row %u: book=%d genre='%s' path='%s': got %d, "
                   "want %d\n", i, r->is_book, r->genre ? r->genre : "(null)",
                   r->path ? r->path : "(null)", got, r->want);
            fails++;
        }
        h = (h ^ (uint32_t)got) * 16777619u;
    }
    /* every listed genre on its own, from an independent list: a missing
     * comma in the header's table would join two names and both fail */
    {
        static const char *const want[] = {
            "Audiobook", "Audiobooks", "Audio Book", "Audio Books", "Book",
            "Books", "Spoken Word", "Spoken & Audio", "Books & Spoken",
            "H\303\266rbuch", "H\303\266rb\303\274cher", "Hoerbuch",
            "Hoerbuecher", "Gesprochenes Wort", "Luisterboek",
            "Luisterboeken", "Gesproken woord",
        };
        static const char *const table[] = { BOOKCLASS_GENRES };
        unsigned nw = sizeof(want) / sizeof(want[0]);
        unsigned nt = sizeof(table) / sizeof(table[0]);

        for (i = 0; i < nw; i++)
            if (!bookclass_genre_is_book(want[i]))
            {
                printf("FAIL: genre '%s' is not a book\n", want[i]);
                fails++;
            }
        if (nt != nw || nt != BOOKCLASS_GENRE_COUNT)
        {
            printf("FAIL: genre table has %u entries, want %u (count %d): "
                   "a missing comma?\n", nt, nw, BOOKCLASS_GENRE_COUNT);
            fails++;
        }
    }
    /* the folder and genre helpers on their own */
    if (!bookclass_genre_is_book("Audiobook") || bookclass_genre_is_book(NULL)
        || !bookclass_path_is_book("/Audio Books/x")
        || bookclass_path_is_book(NULL))
    {
        printf("FAIL: helpers\n");
        fails++;
    }
    if (SORTKEY_VERSION != EXPECTED_VERSION || h != EXPECTED_HASH)
    {
        printf("FAIL: classification hash 0x%08x, SORTKEY_VERSION %d; "
               "recorded 0x%08x for version %d. The class flags are stored "
               "at scan time: if the rules changed on purpose, bump "
               "SORTKEY_VERSION in apps/ipodui/sortkey.h and update %s.\n",
               (unsigned)h, SORTKEY_VERSION, (unsigned)EXPECTED_HASH,
               EXPECTED_VERSION, __FILE__);
        fails++;
    }
    if (fails)
        return 1;
    printf("ok   bookclass: %u rows, hash 0x%08x, SORTKEY_VERSION %d\n",
           n, (unsigned)h, SORTKEY_VERSION);
    return 0;
}
