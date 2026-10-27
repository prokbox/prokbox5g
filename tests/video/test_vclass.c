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
 * Host test: apps/ipodui/vclass_core.c, the scan-time video classifier
 * (Phase 14a V2, docs/phase14a-brief.md 3.2, A3). Every 3.2 rule, the
 * empty-show sentinel, the buffer halves and their bounds.
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
#include <string.h>
#include "vclass.h"

#define V   VCLASS_VIDEO
#define TV  (VCLASS_VIDEO | VCLASS_TV)
#define POD (VCLASS_VIDEO | VCLASS_PODCAST)

struct tcase {
    const char *name;
    struct vclass_in in;
    int32_t flag;
    const char *show, *title;
    int season, episode;
};

/* in: path, album, title, genre, tvsh, is_video, pcst, stik, tvsn, tves */
static const struct tcase cases[] = {
    /* 1. not a video: the audio rules run unchanged */
    { "audio m4a", { "/Music/a.m4a", "Alb", "Song", NULL, NULL,
      0, 0, 0, 0, 0 }, 0, "", "", 0, 0 },
    { "audio in TV Shows", { "/TV Shows/Show/S01E01.mp4", NULL, NULL, NULL,
      "Show", 0, 0, 10, 1, 1 }, 0, "", "", 0, 0 },
    { "audio podcast", { "/Podcasts/P/ep.mp4", "P", NULL, "Podcast", NULL,
      0, 1, 0, 0, 0 }, 0, "", "", 0, 0 },

    /* 2. podcasts: pcst, genre Podcast(s), a Podcasts folder */
    { "pcst", { "/Videos/x.m4v", "Tech Talk", "Ep 5", NULL, NULL,
      1, 1, 0, 0, 0 }, POD, "Tech Talk", "Ep 5", 0, 0 },
    { "genre Podcast", { "/Videos/x.m4v", "Show P", NULL, "Podcast", NULL,
      1, 0, 0, 0, 0 }, POD, "Show P", "x", 0, 0 },
    { "genre podcasts", { "/Videos/y.m4v", NULL, NULL, "podcasts", NULL,
      1, 0, 0, 0, 0 }, POD, "", "y", 0, 0 },
    { "Podcasts folder", { "/Podcasts/Show Q/Episode_One.m4v", "Show Q",
      NULL, NULL, NULL, 1, 0, 0, 0, 0 }, POD, "Show Q", "Episode One", 0, 0 },
    { "podcasts folder lower", { "/media/podcasts/S/ep.m4v", NULL, "Ep",
      NULL, NULL, 1, 0, 0, 0, 0 }, POD, "", "Ep", 0, 0 },
    { "Podcasts as a file name", { "/Videos/Podcasts.m4v", NULL, NULL, NULL,
      NULL, 1, 0, 0, 0, 0 }, V, "", "Podcasts", 0, 0 },
    { "podcast beats stik 10", { "/TV Shows/S/S01E01.m4v", "Pod", "T",
      "Podcast", "S", 1, 0, 10, 1, 1 }, POD, "Pod", "T", 0, 0 },

    /* 3. TV: stik 10, tvsh, a TV Shows folder, SxxEyy / NxNN */
    { "stik 10 tagged", { "/Videos/tv.m4v", NULL, "Ep", NULL, "Show A",
      1, 0, 10, 2, 3 }, TV, "Show A", "Ep", 2, 3 },
    { "tvsh only", { "/Videos/tv.m4v", NULL, NULL, NULL, "Show B",
      1, 0, 0, 0, 0 }, TV, "Show B", "tv", 1, 0 },
    { "stik 10 bare", { "/Videos/clip.m4v", NULL, NULL, NULL, NULL,
      1, 0, 10, 0, 0 }, TV, "", "clip", 1, 0 },
    { "folder show, season, leading number", {
      "/TV Shows/Show B/Season 1/01 Pilot.m4v", NULL, NULL, NULL, NULL,
      1, 0, 0, 0, 0 }, TV, "Show B", "Pilot", 1, 1 },
    { "folder season 2 unnumbered", {
      "/TV Shows/Show B/Season 2/Something.m4v", NULL, NULL, NULL, NULL,
      1, 0, 0, 0, 0 }, TV, "Show B", "Something", 2, 0 },
    { "folder below TV Shows", { "/TV Shows/Show C/ep.m4v", NULL, NULL,
      NULL, NULL, 1, 0, 0, 0, 0 }, TV, "Show C", "ep", 1, 0 },
    { "leading number with dash", {
      "/TV Shows/Show D/Season 1/03 - Title.m4v", NULL, NULL, NULL, NULL,
      1, 0, 0, 0, 0 }, TV, "Show D", "Title", 1, 3 },
    { "Series folder", { "/TV Shows/Show E/Series 2/x.m4v", NULL, NULL,
      NULL, NULL, 1, 0, 0, 0, 0 }, TV, "Show E", "x", 2, 0 },
    { "S03 folder", { "/TV Shows/Show F/S03/x.m4v", NULL, NULL, NULL, NULL,
      1, 0, 0, 0, 0 }, TV, "Show F", "x", 3, 0 },
    { "season05 folder", { "/TV Shows/Show G/season05/x.m4v", NULL, NULL,
      NULL, NULL, 1, 0, 0, 0, 0 }, TV, "Show G", "x", 5, 0 },
    { "filename SxxEyy", { "/Downloads/Show.C.S01E02.m4v", NULL, NULL,
      NULL, NULL, 1, 0, 0, 0, 0 }, TV, "Show C", "Show C S01E02", 1, 2 },
    { "filename SxxEyy with title", {
      "/Downloads/Show.C.S01E02.The.Pilot.m4v", NULL, NULL, NULL, NULL,
      1, 0, 0, 0, 0 }, TV, "Show C", "The Pilot", 1, 2 },
    { "nameless SxxEyy in TV Shows", { "/TV Shows/S01E03.m4v", NULL, NULL,
      NULL, NULL, 1, 0, 0, 0, 0 }, TV, "", "S01E03", 1, 3 },
    { "nameless SxxEyy anywhere", { "/x/S01E03.m4v", NULL, NULL, NULL,
      NULL, 1, 0, 0, 0, 0 }, TV, "", "S01E03", 1, 3 },
    { "NxNN", { "/Video/Show 3x07.m4v", NULL, NULL, NULL, NULL,
      1, 0, 0, 0, 0 }, TV, "Show", "Show 3x07", 3, 7 },
    { "NxNNN", { "/Video/Show 1x100 End.m4v", NULL, NULL, NULL, NULL,
      1, 0, 0, 0, 0 }, TV, "Show", "End", 1, 100 },
    { "lower case, underscores", { "/d/show_name_s02e10.m4v", NULL, NULL,
      NULL, NULL, 1, 0, 0, 0, 0 }, TV, "show name", "show name s02e10",
      2, 10 },
    { "S1 E4 with dash", { "/d/Show - S1 E4.m4v", NULL, NULL, NULL, NULL,
      1, 0, 0, 0, 0 }, TV, "Show", "Show - S1 E4", 1, 4 },
    { "album as show", { "/TV Shows/Folder/x.m4v", "Album Show", "X", NULL,
      NULL, 1, 0, 0, 0, 0 }, TV, "Album Show", "X", 1, 0 },
    { "tvsh beats folder, season from folder", {
      "/TV Shows/Folder Show/Season 3/x.m4v", "Alb", NULL, NULL, "Tag Show",
      1, 0, 0, 0, 0 }, TV, "Tag Show", "x", 3, 0 },
    { "tvsn beats folder", { "/TV Shows/F/Season 3/x.m4v", NULL, NULL, NULL,
      NULL, 1, 0, 0, 4, 0 }, TV, "F", "x", 4, 0 },
    { "tves beats filename", { "/d/Show.S01E02.m4v", NULL, NULL, NULL, NULL,
      1, 0, 0, 0, 9 }, TV, "Show", "Show S01E02", 1, 9 },
    { "season from SxxEyy", { "/d/Show S03E01.m4v", NULL, "Tag Title", NULL,
      NULL, 1, 0, 0, 0, 0 }, TV, "Show", "Tag Title", 3, 1 },
    { "SxxEyy beats leading number", {
      "/TV Shows/Sh/Season 1/07 Show S01E02.m4v", NULL, NULL, NULL, NULL,
      1, 0, 0, 0, 0 }, TV, "Sh", "07 Show S01E02", 1, 2 },
    { "season folder under TV Shows only: empty show", {
      "/TV Shows/Season 1/x.m4v", NULL, NULL, NULL, NULL,
      1, 0, 0, 0, 0 }, TV, "", "x", 1, 0 },
    { "nearest category Movies over TV Shows", {
      "/TV Shows/Old/Movies/x.m4v", NULL, NULL, NULL, NULL,
      1, 0, 0, 0, 0 }, V, "", "x", 0, 0 },
    { "nearest category TV Shows over Movies", {
      "/Movies/TV Shows/Sh/x.m4v", NULL, NULL, NULL, NULL,
      1, 0, 0, 0, 0 }, TV, "Sh", "x", 1, 0 },
    { "no extension", { "/TV Shows/Sh/Season 2/Finale", NULL, NULL, NULL,
      NULL, 1, 0, 0, 0, 0 }, TV, "Sh", "Finale", 2, 0 },

    /* 4. movies: everything else, stik 6 and 9 beat the folder */
    { "stik 9 in TV Shows", { "/TV Shows/Sh/Season 1/S01E01.m4v", NULL,
      NULL, NULL, NULL, 1, 0, 9, 0, 0 }, V, "", "S01E01", 0, 0 },
    { "stik 6 is a movie", { "/TV Shows/Sh/clip.m4v", NULL, "Clip", NULL,
      NULL, 1, 0, 6, 0, 0 }, V, "", "Clip", 0, 0 },
    { "tagged movie", { "/Movies/Big Film.m4v", NULL, "Big Film", NULL,
      NULL, 1, 0, 0, 0, 0 }, V, "", "Big Film", 0, 0 },
    { "scene-named movie", { "/Movies/Big.Film.2019.m4v", NULL, NULL, NULL,
      NULL, 1, 0, 0, 0, 0 }, V, "", "Big Film 2019", 0, 0 },
    { "dots kept beside spaces", { "/Movies/Mr. Robot.m4v", NULL, NULL,
      NULL, NULL, 1, 0, 0, 0, 0 }, V, "", "Mr. Robot", 0, 0 },
    { "resolution is not NxNN", { "/Video/Movie 1920x1080.m4v", NULL, NULL,
      NULL, NULL, 1, 0, 0, 0, 0 }, V, "", "Movie 1920x1080", 0, 0 },
    { "letter before S is not SxxEyy", { "/Video/Class1e2.m4v", NULL, NULL,
      NULL, NULL, 1, 0, 0, 0, 0 }, V, "", "Class1e2", 0, 0 },
    { "season folder outside TV Shows", { "/Video/Show/Season 1/ep.m4v",
      NULL, NULL, NULL, NULL, 1, 0, 0, 0, 0 }, V, "", "ep", 0, 0 },
    { "album kept out of a movie", { "/Movies/m.m4v", "Some Album", NULL,
      NULL, NULL, 1, 0, 0, 0, 0 }, V, "", "m", 0, 0 },
    { "leading number kept in a movie", { "/Movies/2001 A Space Odyssey.m4v",
      NULL, NULL, NULL, NULL, 1, 0, 0, 0, 0 }, V, "",
      "2001 A Space Odyssey", 0, 0 },
};

static int fails;

static void expect_str(const char *name, const char *what, const char *got,
                       const char *want)
{
    if (!got || strcmp(got, want)) {
        printf("FAIL %s: %s '%s', want '%s'\n", name, what,
               got ? got : "(null)", want);
        fails++;
    }
}

static void expect_int(const char *name, const char *what, long got,
                       long want)
{
    if (got != want) {
        printf("FAIL %s: %s %ld, want %ld\n", name, what, got, want);
        fails++;
    }
}

/* out->show / out->title point into buf or into the input strings */
static bool inside(const char *p, const char *buf, size_t len,
                   const struct vclass_in *in)
{
    const char *s[] = { in->album, in->title, in->tvsh, in->genre, in->path };
    size_t i;

    if (!p)
        return false;
    if (p >= buf && p < buf + len)
        return true;
    for (i = 0; i < sizeof(s) / sizeof(s[0]); i++)
        if (s[i] && p >= s[i] && p <= s[i] + strlen(s[i]))
            return true;
    return *p == '\0';      /* a string literal "" */
}

static void run_case(const struct tcase *c)
{
    char buf[260];
    struct vclass_out out;
    int32_t r;
    int fails0 = fails;

    memset(buf, 0x7e, sizeof(buf));
    r = vclass_classify(&c->in, &out, buf, sizeof(buf));
    expect_int(c->name, "return", r, c->flag);
    expect_int(c->name, "flag", out.flag, c->flag);
    expect_str(c->name, "show", out.show, c->show);
    expect_str(c->name, "title", out.title, c->title);
    expect_int(c->name, "season", out.season, c->season);
    expect_int(c->name, "episode", out.episode, c->episode);
    if (!inside(out.show, buf, sizeof(buf), &c->in)
        || !inside(out.title, buf, sizeof(buf), &c->in)) {
        printf("FAIL %s: a result points outside buf and the input\n",
               c->name);
        fails++;
    }
    if (fails == fails0)
        printf("ok   %s\n", c->name);
}

/* the halves: show and title each stay inside their own half, are
 * terminated, and are cut on a UTF-8 boundary */
static void bounds(void)
{
    static const char *path =
        "/TV Shows/A Very Long Show Name That Goes On/Season 1/"
        "\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9 An Episode Title.m4v";
    struct vclass_in in = { path, NULL, NULL, NULL, NULL,
                            true, false, 0, 0, 0 };
    struct vclass_out out;
    char buf[24];
    int fails0 = fails;
    size_t i;

    memset(buf, 0x7e, sizeof(buf));
    vclass_classify(&in, &out, buf, 16);
    for (i = 16; i < sizeof(buf); i++)
        if (buf[i] != 0x7e) {
            printf("FAIL bounds: wrote past len at %zu\n", i);
            fails++;
            break;
        }
    expect_str("bounds", "show", out.show, "A Very");
    if (out.show != buf || out.title != buf + 8) {
        printf("FAIL bounds: show/title not in the two halves\n");
        fails++;
    }
    /* 7 bytes for the title: three 2-byte characters, never half of one */
    expect_str("bounds", "title", out.title, "\xc3\xa9\xc3\xa9\xc3\xa9");

    /* no room at all: empty strings, nothing written */
    memset(buf, 0x7e, sizeof(buf));
    vclass_classify(&in, &out, buf, 1);
    expect_str("bounds len 1", "show", out.show, "");
    expect_str("bounds len 1", "title", out.title, "");
    expect_int("bounds len 1", "flag", out.flag, TV);
    if (buf[1] != 0x7e) {
        printf("FAIL bounds len 1: wrote past len\n");
        fails++;
    }
    if (fails == fails0)
        printf("ok   bounds (halves, UTF-8 cut, tiny buffer)\n");
}

int main(void)
{
    size_t i, n = sizeof(cases) / sizeof(cases[0]);

    for (i = 0; i < n; i++)
        run_case(&cases[i]);
    bounds();
    printf("test_vclass: %zu cases + bounds, %d failure(s)\n", n, fails);
    return fails ? 1 : 0;
}
