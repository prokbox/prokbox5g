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

/* iPod UI shell: audiobook and podcast classification rules (#146) */
#ifndef _IPODUI_BOOKCLASS_H_
#define _IPODUI_BOOKCLASS_H_

/* Pure C, header only: no Rockbox headers, no statics beyond read-only
 * tables, no allocation, no I/O. Included by apps/tagcache.c (ui_classify(),
 * the only decision point; scanner on the tagcache thread, and
 * tagcache_ui_resumes() on the audio thread) and by the host test
 * tests/classify/test_bookclass.c. Header only so that every build of
 * tagcache.c (firmware, sim, the db_commit plugin) gets it without a new
 * source file. Brief: docs/design/audiobook-classify-brief.md.
 *
 * Rules, applied after the video check in ui_classify():
 *   1. Podcast: genre Podcast/Podcasts (case-insensitive, exact) or a path
 *      containing "/Podcasts/" (unchanged from before #146).
 *   2. Book, any one of:
 *      - the parser's type flag (m4b extension, MP4 book flag);
 *      - a book genre from bookclass_genres[], trimmed, case-insensitive
 *        (ASCII fold only), exact: "Books" matches, "Bookshelf" does not;
 *      - a path containing a folder named "Audiobooks" or "Audio Books"
 *        (case-insensitive, any depth). Fixed names, no setting.
 * A podcast wins over a book. Music in an Audiobooks folder is a book, and
 * so is a book-genre file anywhere: both are explicit owner acts. */

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

enum bookclass {
    BOOKCLASS_MUSIC = 0,
    BOOKCLASS_BOOK,
    BOOKCLASS_PODCAST,
};

/* 's' equals 'name' ignoring ASCII case and leading/trailing blanks */
static inline bool bookclass_eq(const char *s, const char *name)
{
    size_t n = strlen(name);

    if (!s)
        return false;
    while (*s == ' ' || *s == '\t')
        s++;
    if (strncasecmp(s, name, n))
        return false;
    for (s += n; *s; s++)
        if (*s != ' ' && *s != '\t' && *s != '\r' && *s != '\n')
            return false;
    return true;
}

/* Book genres: English (iTunes/Audible/ID3 names) plus Dutch and German
 * (1.0 languages). Radio plays (Hoerspiel) are left out on purpose: they
 * are often music-like. Non-ASCII names are listed literally. ID3v1
 * numeric genre 101 is "Speech" (not a book); 183 (Audiobook) is beyond
 * the parser's table and arrives as no genre, so it needs the folder. */
#define BOOKCLASS_GENRES                                                   \
    "Audiobook", "Audiobooks", "Audio Book", "Audio Books",                \
    "Book", "Books", "Spoken Word", "Spoken & Audio", "Books & Spoken",    \
    /* de; UTF-8 in octal (a hex escape would swallow the next letter) */  \
    "H\303\266rbuch", "H\303\266rb\303\274cher", "Hoerbuch", "Hoerbuecher",\
    "Gesprochenes Wort",                                                   \
    /* nl */                                                               \
    "Luisterboek", "Luisterboeken", "Gesproken woord"
#define BOOKCLASS_GENRE_COUNT 17    /* tests/classify checks the table size */

static inline bool bookclass_genre_is_book(const char *genre)
{
    static const char *const genres[] = { BOOKCLASS_GENRES };
    size_t i;

    if (!genre)
        return false;
    for (i = 0; i < sizeof(genres) / sizeof(genres[0]); i++)
        if (bookclass_eq(genre, genres[i]))
            return true;
    return false;
}

/* 'path' has a folder component equal (ASCII case-insensitive) to 'dir' */
static inline bool bookclass_path_has_dir(const char *path, const char *dir)
{
    size_t n = strlen(dir);

    if (!path)
        return false;
    for (; (path = strchr(path, '/')) != NULL; path++)
        if (!strncasecmp(path + 1, dir, n) && path[1 + n] == '/')
            return true;
    return false;
}

static inline bool bookclass_path_is_book(const char *path)
{
    return bookclass_path_has_dir(path, "Audiobooks")
        || bookclass_path_has_dir(path, "Audio Books");
}

/* The class of a non-video file. 'is_audiobook' is mp3entry.is_audiobook;
 * 'genre' and 'path' may be NULL. */
static inline enum bookclass bookclass_of(bool is_audiobook,
                                          const char *genre, const char *path)
{
    if ((genre && (!strcasecmp(genre, "Podcast")
                   || !strcasecmp(genre, "Podcasts")))
        || bookclass_path_has_dir(path, "Podcasts"))
        return BOOKCLASS_PODCAST;
    if (is_audiobook || bookclass_genre_is_book(genre)
        || bookclass_path_is_book(path))
        return BOOKCLASS_BOOK;
    return BOOKCLASS_MUSIC;
}

#endif /* _IPODUI_BOOKCLASS_H_ */
