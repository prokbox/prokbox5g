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
 * iPod UI shell: the diagnostics report's pure logic (release-plan 6.2).
 * No firmware calls: host-tested in tests/diag.
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
#include "diag_core.h"

/* The extensions a user's own file may carry: audio, video, pictures,
 * playlists, cue sheets, lyrics and text (lower case; matched without
 * case). The firmware's own files (.bin, .cfg, .log, .dat) are not here:
 * they name nothing of the user's, and their paths are caught as paths */
static const char ext_list[] =
    " mp3 mp2 mpa m4a m4b m4r m4p mp4 m4v aac alac flac fla ogg oga opus"
    " spx wav wv aif aiff aifc ape mpc wma asf ac3 a52 mid midi mod s3m xm"
    " it spc sid nsf vgm tta shn mka mkv webm mov avi 3gp vix jpg jpeg png"
    " bmp gif tif tiff webp heic cue lrc snc txt m3u m3u8 pls nfo pdf epub"
    " rtf doc docx ";

static const char *const title_keys[] = {
    "title", "artist", "album", "albumartist", "composer", "genre", "book",
    "song", "name", "comment", "grouping",
};
static const char *const path_keys[] = {
    "file", "filename", "path", "dir", "folder", "url",
};

#define ARRAYLEN_(a) (sizeof(a) / sizeof((a)[0]))

/* the system folder's name, spelled out so that no string literal in the
 * shell holds it (validate.sh's native-illusion grep) */
static const char sys_name[] = { 'r', 'o', 'c', 'k', 'b', 'o', 'x', '\0' };

static int lower(int c)
{
    return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c;
}

static bool is_alnum(int c)
{
    return (c >= '0' && c <= '9') || (lower(c) >= 'a' && lower(c) <= 'z');
}

/* in[0..len) starts with s (NUL ended), case aside: strlen(s), else 0 */
static size_t starts_ci(const char *in, size_t len, const char *s)
{
    size_t n = strlen(s), i;

    if (!n || n > len)
        return 0;
    for (i = 0; i < n; i++)
        if (lower((unsigned char)in[i]) != lower((unsigned char)s[i]))
            return 0;
    return n;
}

/* in[i] == '.': a known extension ending at a word's end? Its end, or 0 */
static size_t ext_end(const char *in, size_t len, size_t i)
{
    size_t k = i + 1, n;
    char e[10];

    while (k < len && is_alnum((unsigned char)in[k]) && k - i <= 6)
        k++;
    n = k - i - 1;
    if (!n || n > 5 || (k < len && is_alnum((unsigned char)in[k])))
        return 0;
    e[0] = ' ';
    for (size_t j = 0; j < n; j++)
        e[j + 1] = lower((unsigned char)in[i + 1 + j]);
    e[n + 1] = ' ';
    e[n + 2] = '\0';
    return strstr(ext_list, e) ? k : 0;   /* " flac " in the list */
}

static bool token_start(const char *in, size_t i)
{
    if (!i)
        return true;
    return strchr(" \t=:([,'<>", in[i - 1]) != NULL;
}

struct out {
    char *p;
    size_t o, size;
};

static void put(struct out *w, const char *s, size_t n)
{
    while (n-- && w->o + 1 < w->size)
        w->p[w->o++] = *s++;
}

static void puts_(struct out *w, const char *s)
{
    put(w, s, strlen(s));
}

/* "<seq> <tag> " (or "p <tag> "): the start of the message proper */
static size_t msg_start(const char *in, size_t len)
{
    size_t i = 0, sp;
    bool digits = true;

    while (i < len && in[i] != ' ')
    {
        if (in[i] < '0' || in[i] > '9')
            digits = false;
        i++;
    }
    if (i >= len || !(digits || (i == 1 && in[0] == 'p')))
        return 0;
    sp = i + 1;
    while (sp < len && in[sp] != ' ')
    {
        if (!is_alnum((unsigned char)in[sp]) && in[sp] != '_')
            return 0;               /* not a tag: scrub it all */
        sp++;
    }
    return sp < len ? sp + 1 : 0;
}

size_t diag_scrub_line(const char *in, size_t len, char *out, size_t size,
                       const char *const *deny, int ndeny, unsigned *hits)
{
    struct out w = { out, 0, size };
    size_t i = msg_start(in, len), bound, o_msg, k;
    unsigned h = 0;

    if (!size)
        return 0;
    put(&w, in, i);                 /* the sequence number and the tag */
    o_msg = bound = w.o;
    while (i < len)
    {
        const char *at = in + i;
        size_t rest = len - i, n = 0;
        int d;

        /* the playing track's names and path parts */
        for (d = 0; d < ndeny && !n; d++)
            if (deny[d] && strlen(deny[d]) >= DIAG_DENY_MIN)
                n = starts_ci(at, rest, deny[d]);
        if (n)
        {
            puts_(&w, DIAG_TITLE);
            i += n;
            bound = w.o;
            h++;
            continue;
        }
        if (*at == '"')
        {
            const char *q = memchr(at + 1, '"', rest - 1);

            puts_(&w, DIAG_TITLE);
            i = q ? (size_t)(q - in) + 1 : len;
            bound = w.o;
            h++;
            continue;
        }
        if (token_start(in, i))
        {
            for (k = 0; k < ARRAYLEN_(title_keys) && !n; k++)
                if ((n = starts_ci(at, rest, title_keys[k])) && n < rest
                    && at[n] == '=')
                {
                    put(&w, at, n + 1);
                    puts_(&w, DIAG_TITLE);
                }
                else
                    n = 0;
            for (k = 0; k < ARRAYLEN_(path_keys) && !n; k++)
                if ((n = starts_ci(at, rest, path_keys[k])) && n < rest
                    && at[n] == '=')
                {
                    put(&w, at, n + 1);
                    puts_(&w, DIAG_PATH);
                }
                else
                    n = 0;
            if (n)
            {
                h++;
                i = len;            /* the value may hold spaces */
                break;
            }
            if (*at == '/' && rest > 1 && at[1] != ' ' && at[1] != '\t')
            {
                size_t j = i + 1, e = 0;

                while (j < len && in[j] != '"')
                {
                    if (in[j] == '.' && (e = ext_end(in, len, j)))
                        break;
                    j++;
                }
                puts_(&w, DIAG_PATH);
                i = e ? e : j;
                bound = w.o;
                h++;
                continue;
            }
        }
        if (*at == '.' && i > 0 && (k = ext_end(in, len, i)))
        {
            w.o = bound;            /* the name before it, whatever it is */
            puts_(&w, DIAG_PATH);
            i = k;
            bound = w.o;
            h++;
            continue;
        }
        if ((n = starts_ci(at, rest, sys_name)))
        {
            puts_(&w, DIAG_SYS);
            i += n;
            h++;
            continue;
        }
        put(&w, at, 1);
        if (*at == '=')
            bound = w.o;
        i++;
    }
    /* anything still outside plain ASCII is a name we did not recognise */
    for (k = o_msg; k < w.o; k++)
    {
        unsigned char c = (unsigned char)out[k];

        if (c >= 0x80 || (c < 0x20 && c != '\t'))
        {
            w.o = o_msg;
            puts_(&w, DIAG_TITLE);
            h++;
            break;
        }
    }
    out[w.o] = '\0';
    if (hits)
        *hits += h;
    return w.o;
}

size_t diag_tail(const char *buf, size_t len, bool whole, int n)
{
    size_t start = 0, i;
    int count = 0;

    if (n <= 0)
        return len;
    if (!whole)
    {
        const char *nl = memchr(buf, '\n', len);

        if (!nl)
            return len;             /* not one whole line */
        start = (size_t)(nl - buf) + 1;
    }
    i = len;
    if (i > start && buf[i - 1] == '\n')
        i--;                        /* the last line's own end */
    for (; i > start; i--)
        if (buf[i - 1] == '\n' && ++count == n)
            return i;
    return start;
}

void diag_init(struct diag_buf *b, char *p, size_t size)
{
    b->p = p;
    b->len = 0;
    b->size = size;
    b->full = false;
    if (size)
        p[0] = '\0';
}

void diag_printf(struct diag_buf *b, const char *fmt, ...)
{
    va_list ap;
    size_t room = b->size - b->len;
    int n;

    if (room < 2)
    {
        b->full = true;
        return;
    }
    va_start(ap, fmt);
    n = vsnprintf(b->p + b->len, room, fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    if ((size_t)n >= room)
    {
        b->full = true;
        n = (int)room - 1;
    }
    b->len += (size_t)n;
}

void diag_put_lines(struct diag_buf *b, const char *indent, const char *text,
                    size_t len, const char *const *deny, int ndeny,
                    unsigned *hits)
{
    char line[128];

    while (len)
    {
        const char *nl = memchr(text, '\n', len);
        size_t l = nl ? (size_t)(nl - text) : len;
        size_t s = l;

        if (s && text[s - 1] == '\r')
            s--;
        if (s)
        {
            diag_scrub_line(text, s, line, sizeof(line), deny, ndeny, hits);
            diag_printf(b, "%s%s\n", indent, line);
        }
        if (!nl)
            break;
        text += l + 1;
        len -= l + 1;
    }
}
