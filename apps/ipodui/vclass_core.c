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
 * iPod UI shell: video classification core (Phase 14a)
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

/* Contract: vclass.h; rules: docs/phase14a-brief.md 3.2. Pure C (host-
 * tested by tests/video/test_vclass.c): no Rockbox headers, no statics, no
 * allocation, a few hundred bytes of stack. Runs once per video file on the
 * tagcache thread while the scanner has it open; never at browse time.
 *
 * Choices the brief leaves open (all tested):
 *  - a stik of 6 or 9 makes a Movie whatever the tags, folders or name say
 *    (a stik beats the folder); other stik values are ignored;
 *  - the patterns need a non-alphanumeric character (or the start) before
 *    S / the season digits and a non-digit after the episode digits, so
 *    "Class1e2" and "1920x1080" are not episodes;
 *  - cleaning a file or folder name: '_' becomes a space, '.' too when the
 *    name has no space (scene names), runs of spaces collapse, and spaces,
 *    '-' and '_' are trimmed from both ends;
 *  - a TV title from the file name is the text after SxxEyy / NxNN when
 *    there is any, else the leading episode number is dropped when it gave
 *    the episode, else the whole cleaned name. */
#include <string.h>
#include "vclass.h"

struct span {
    const char *s;
    size_t n;
};

static char lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? c + ('a' - 'A') : c;
}

static bool is_digit(char c)
{
    return c >= '0' && c <= '9';
}

static bool is_alnum(char c)
{
    c = lower(c);
    return is_digit(c) || (c >= 'a' && c <= 'z');
}

static bool span_is(struct span sp, const char *word)
{
    size_t i;

    for (i = 0; i < sp.n; i++)
        if (!word[i] || lower(sp.s[i]) != lower(word[i]))
            return false;
    return word[i] == '\0';
}

static bool str_is(const char *s, const char *word)
{
    return s && span_is((struct span){ s, strlen(s) }, word);
}

static bool nonempty(const char *s)
{
    return s && *s;
}

/* up to 'max' digits at s[*i]; at least one */
static int digits(const char *s, size_t n, size_t *i, int max)
{
    int v = 0, k = 0;

    while (*i < n && k < max && is_digit(s[*i])) {
        v = v * 10 + (s[*i] - '0');
        (*i)++;
        k++;
    }
    return k ? v : -1;
}

/* ^(Season|Series|S)\s*(\d{1,2})$, case-insensitive; 0 when no match */
static int season_folder(struct span sp)
{
    static const char *const words[] = { "season", "series", "s" };
    size_t w;

    for (w = 0; w < sizeof(words) / sizeof(words[0]); w++) {
        size_t k = strlen(words[w]), i;
        int v;

        if (sp.n <= k || !span_is((struct span){ sp.s, k }, words[w]))
            continue;
        i = k;
        while (i < sp.n && sp.s[i] == ' ')
            i++;
        v = digits(sp.s, sp.n, &i, 2);
        if (v >= 0 && i == sp.n)
            return v;
    }
    return 0;
}

/* S(\d{1,2})[ ._-]?E(\d{1,3}) then (\d{1,2})x(\d{2,3}) in the file stem;
 * the match is [*at, *end) */
static bool episode_pattern(struct span st, int *season, int *episode,
                            size_t *at, size_t *end)
{
    size_t p;

    for (p = 0; p < st.n; p++) {
        size_t i = p + 1;
        int s, e;

        if (lower(st.s[p]) != 's' || (p && is_alnum(st.s[p - 1])))
            continue;
        if ((s = digits(st.s, st.n, &i, 2)) < 0)
            continue;
        if (i < st.n && (st.s[i] == ' ' || st.s[i] == '.' || st.s[i] == '_'
                         || st.s[i] == '-'))
            i++;
        if (i >= st.n || lower(st.s[i]) != 'e')
            continue;
        i++;
        if ((e = digits(st.s, st.n, &i, 3)) < 0
            || (i < st.n && is_digit(st.s[i])))
            continue;
        *season = s;
        *episode = e;
        *at = p;
        *end = i;
        return true;
    }
    for (p = 0; p < st.n; p++) {
        size_t i = p;
        int s, e;

        if (!is_digit(st.s[p]) || (p && is_alnum(st.s[p - 1])))
            continue;
        if ((s = digits(st.s, st.n, &i, 2)) < 0 || i >= st.n
            || lower(st.s[i]) != 'x')
            continue;
        i++;
        if (i >= st.n || !is_digit(st.s[i]))
            continue;
        {
            size_t d = i;
            e = digits(st.s, st.n, &i, 3);
            if (i - d < 2 || (i < st.n && is_digit(st.s[i])))
                continue;
        }
        *season = s;
        *episode = e;
        *at = p;
        *end = i;
        return true;
    }
    return false;
}

/* Copies a cleaned 'src' into dst[size], cut on a UTF-8 boundary;
 * returns dst ("" when size is 0: a literal). */
static const char *clean(char *dst, size_t size, struct span src)
{
    bool dots = memchr(src.s, ' ', src.n) == NULL;
    size_t i, o = 0;

    if (size == 0)
        return "";
    for (i = 0; i < src.n; i++) {
        char c = src.s[i];

        if (c == '_' || (dots && c == '.'))
            c = ' ';
        if (c == ' ' && (o == 0 || dst[o - 1] == ' '))
            continue;                   /* leading or repeated space */
        if (o + 1 >= size)
            break;
        dst[o++] = c;
    }
    /* never end inside a UTF-8 sequence */
    if (i < src.n) {
        size_t k = o;
        while (k > 0 && ((unsigned char)dst[k - 1] & 0xc0) == 0x80)
            k--;
        if (k > 0 && ((unsigned char)dst[k - 1] & 0x80)) {
            unsigned char lead = dst[k - 1];
            size_t need = lead >= 0xf0 ? 4 : lead >= 0xe0 ? 3
                        : lead >= 0xc0 ? 2 : 1;
            if (o - (k - 1) < need)
                o = k - 1;
        }
    }
    while (o > 0 && (dst[o - 1] == ' ' || dst[o - 1] == '-'
                     || dst[o - 1] == '_'))
        o--;
    dst[o] = '\0';
    {
        size_t lead = 0;
        while (dst[lead] == '-' || dst[lead] == ' ')
            lead++;
        if (lead)
            memmove(dst, dst + lead, o - lead + 1);
    }
    return dst;
}

int32_t vclass_classify(const struct vclass_in *in, struct vclass_out *out,
                        char *buf, size_t len)
{
    /* directory components of the path, nearest first */
    enum { MAXDIRS = 16 };
    struct span dir[MAXDIRS];
    int ndirs = 0;
    struct span stem;
    const char *path = in->path ? in->path : "";
    const char *base, *p, *q;
    size_t half = len / 2;
    char *show_buf = buf, *title_buf = buf + half;
    size_t show_size = half, title_size = len - half;
    bool podcast_dir = false;
    int tv_dir = -1, movie_dir = -1, season_dir = -1, season_n = 0;
    int pat_s = 0, pat_e = 0;
    size_t pat_at = 0, pat_end = 0;
    bool pat;

    memset(out, 0, sizeof(*out));
    out->show = "";
    out->title = "";
    if (!in->is_video)
        return 0;

    /* the file stem: base name without its extension */
    base = strrchr(path, '/');
    base = base ? base + 1 : path;
    q = strrchr(base, '.');
    stem.s = base;
    stem.n = (q && q != base) ? (size_t)(q - base) : strlen(base);

    /* directories, walking back from the file */
    p = base;
    while (p > path && ndirs < MAXDIRS) {
        const char *e = p - 1;          /* the '/' before p */
        const char *s = e;
        while (s > path && s[-1] != '/')
            s--;
        if (e > s)
            dir[ndirs++] = (struct span){ s, (size_t)(e - s) };
        p = s;
    }
    for (int i = 0; i < ndirs; i++) {
        if (span_is(dir[i], "podcasts"))
            podcast_dir = true;
        if (tv_dir < 0 && movie_dir < 0) {
            if (span_is(dir[i], "tv shows"))
                tv_dir = i;
            else if (span_is(dir[i], "movies"))
                movie_dir = i;
        }
        if (season_dir < 0 && (season_n = season_folder(dir[i])) > 0)
            season_dir = i;
    }
    pat = episode_pattern(stem, &pat_s, &pat_e, &pat_at, &pat_end);

    /* 2. podcast: pcst, genre Podcast(s), a Podcasts folder */
    if (in->pcst || str_is(in->genre, "podcast") || str_is(in->genre, "podcasts")
        || podcast_dir) {
        out->flag = VCLASS_VIDEO | VCLASS_PODCAST;
        if (nonempty(in->album))
            out->show = in->album;
        out->title = nonempty(in->title) ? in->title
                   : clean(title_buf, title_size, stem);
        return out->flag;
    }

    /* 3. TV: stik 10, tvsh, the nearest category folder, SxxEyy / NxNN;
     * a stik of 6 or 9 says Movie and beats all of them */
    if (in->stik != 6 && in->stik != 9
        && (in->stik == 10 || nonempty(in->tvsh) || tv_dir >= 0 || pat)) {
        int lead = -1;
        size_t lead_end = 0;

        out->flag = VCLASS_VIDEO | VCLASS_TV;

        /* show */
        if (nonempty(in->tvsh))
            out->show = in->tvsh;
        else if (nonempty(in->album))
            out->show = in->album;
        else if (season_dir >= 0 && season_dir + 1 < ndirs
                 && season_dir + 1 != tv_dir
                 && !span_is(dir[season_dir + 1], "movies")
                 && !span_is(dir[season_dir + 1], "podcasts"))
            out->show = clean(show_buf, show_size, dir[season_dir + 1]);
        else if (tv_dir > 0 && season_folder(dir[tv_dir - 1]) == 0)
            out->show = clean(show_buf, show_size, dir[tv_dir - 1]);
        else if (pat && pat_at > 0)
            out->show = clean(show_buf, show_size,
                              (struct span){ stem.s, pat_at });

        /* season */
        out->season = in->tvsn > 0 ? in->tvsn
                    : season_dir >= 0 ? season_n
                    : pat ? pat_s : 1;
        if (out->season <= 0)
            out->season = 1;

        /* episode: a leading number only when nothing else numbers it */
        if (!pat && in->tves <= 0) {
            size_t i = 0;
            lead = digits(stem.s, stem.n, &i, 3);
            if (lead >= 0 && i < stem.n && stem.s[i] != ' '
                && stem.s[i] != '.' && stem.s[i] != '-' && stem.s[i] != '_')
                lead = -1;
            lead_end = i;
        }
        out->episode = in->tves > 0 ? in->tves
                     : pat ? pat_e
                     : lead >= 0 ? lead : 0;

        /* title */
        if (nonempty(in->title)) {
            out->title = in->title;
        } else {
            struct span t = stem;
            if (pat && pat_end < stem.n) {
                struct span after = { stem.s + pat_end, stem.n - pat_end };
                out->title = clean(title_buf, title_size, after);
                if (*out->title)
                    return out->flag;
            } else if (lead >= 0 && lead_end < stem.n) {
                t = (struct span){ stem.s + lead_end, stem.n - lead_end };
            }
            out->title = clean(title_buf, title_size, t);
            if (!*out->title)
                out->title = clean(title_buf, title_size, stem);
        }
        return out->flag;
    }

    /* 4. movie: everything else */
    out->flag = VCLASS_VIDEO;
    out->title = nonempty(in->title) ? in->title
               : clean(title_buf, title_size, stem);
    return out->flag;
}
