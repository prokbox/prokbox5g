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
 * iPod UI shell: Now Playing lyrics wrapping (Phase 5)
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
/* Pure C on purpose: no Rockbox headers (host-tested by tests/npscreen/). */
#include <stdbool.h>
#include "np_lyrics.h"

static bool is_break(char c)
{
    return c == '\n' || c == '\r';
}

/* One UTF-8 code point at s[0..n): *len gets its byte count (>= 1; a bad
 * byte counts as one Latin-1 character). */
static unsigned utf8_cp(const unsigned char *s, int n, int *len)
{
    unsigned c = s[0];
    int k, need;

    if (c < 0x80)
    {
        *len = 1;
        return c;
    }
    if ((c & 0xe0) == 0xc0)
        need = 1, c &= 0x1f;
    else if ((c & 0xf0) == 0xe0)
        need = 2, c &= 0x0f;
    else if ((c & 0xf8) == 0xf0)
        need = 3, c &= 0x07;
    else
    {
        *len = 1;
        return c;
    }
    if (need >= n)
    {
        *len = 1;
        return s[0];
    }
    for (k = 1; k <= need; k++)
    {
        if ((s[k] & 0xc0) != 0x80)
        {
            *len = 1;
            return s[0];
        }
        c = (c << 6) | (s[k] & 0x3f);
    }
    *len = need + 1;
    return c;
}

/* Skips "[mm:ss.xx]" time tags (and spaces after them) at p */
static int skip_tags(const char *t, int p, int len)
{
    for (;;)
    {
        int q = p + 1, digits = 0;

        if (p >= len || t[p] != '[')
            return p;
        while (q < len && q - p < 14
               && ((t[q] >= '0' && t[q] <= '9') || t[q] == ':'
                   || t[q] == '.'))
        {
            if (t[q] >= '0' && t[q] <= '9')
                digits++;
            q++;
        }
        if (q >= len || t[q] != ']' || digits < 2)
            return p;
        p = q + 1;
        while (p < len && t[p] == ' ')
            p++;
    }
}

static bool para_start(const char *t, int p)
{
    return p == 0 || is_break(t[p - 1]);
}

int np_lyrics_wrap(const char *text, int len, const struct np_lyr_font *f,
                   uint16_t *starts, int max_lines)
{
    const unsigned char *u = (const unsigned char *)text;
    int pos = 0, n = 0;

    if (len > 0xffff)
        len = 0xffff;
    while (pos < len && n < max_lines)
    {
        int b = para_start(text, pos) ? skip_tags(text, pos, len) : pos;
        int i = b, w = 0, brk = -1;

        starts[n++] = (uint16_t)pos;
        for (;;)
        {
            int cl, cw;
            unsigned cp;

            if (i >= len)
            {
                pos = len;
                break;
            }
            if (is_break(text[i]))
            {
                /* CRLF, LF or a lone CR */
                pos = i + ((text[i] == '\r' && i + 1 < len
                            && text[i + 1] == '\n') ? 2 : 1);
                break;
            }
            cp = utf8_cp(u + i, len - i, &cl);
            cw = f->advance(f->ctx, cp);
            if (w + cw > f->width && i > b && text[i] != ' ')
            {
                pos = brk > b ? brk : i;
                while (pos < len && text[pos] == ' ')
                    pos++;
                break;
            }
            if (text[i] == ' ')
                brk = i + 1;
            w += cw;
            i += cl;
        }
    }
    return n;
}

void np_lyrics_line(const char *text, int len, const uint16_t *starts,
                    int n, int i, int *b, int *e)
{
    int s, t;

    if (i < 0 || i >= n)
    {
        *b = *e = 0;
        return;
    }
    s = starts[i];
    t = i + 1 < n ? starts[i + 1] : len;
    if (t > len)
        t = len;
    if (para_start(text, s))
        s = skip_tags(text, s, t);
    while (t > s && (is_break(text[t - 1]) || text[t - 1] == ' '))
        t--;
    *b = s;
    *e = t;
}
