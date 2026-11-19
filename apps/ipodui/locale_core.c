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

#include <stdio.h>
#include <string.h>
#include "locale_core.h"

size_t locale_utf8_cut(const char *s, size_t n)
{
    size_t l = strlen(s);

    if (l <= n)
        return l;
    while (n > 0 && ((unsigned char)s[n] & 0xc0) == 0x80)
        n--;                            /* s[n] continues a sequence */
    return n;
}

/* appends s (n bytes) at *o, cut on a character boundary at the end */
static void put(char *buf, size_t len, size_t *o, const char *s, size_t n)
{
    size_t room = len - 1 - *o;

    if (*o >= len - 1)
        return;
    if (n > room)
    {
        while (room > 0 && ((unsigned char)s[room] & 0xc0) == 0x80)
            room--;
        memcpy(buf + *o, s, room);
        buf[*o + room] = '\0';
        *o = len - 1;                   /* full: nothing after a cut */
        return;
    }
    memcpy(buf + *o, s, n);
    *o += n;
    buf[*o] = '\0';
}

static void put_num(char *buf, size_t len, size_t *o, int v, int width)
{
    char t[12];
    int n = snprintf(t, sizeof(t), width == 2 ? "%02d" : "%d", v);

    put(buf, len, o, t, (size_t)n);
}

/* a name, its first letter lower-cased if 'lower' (ASCII and Latin-1
 * capitals: the shell's Latin languages) */
static void put_name(char *buf, size_t len, size_t *o, const char *s,
                     bool lower)
{
    const unsigned char *u = (const unsigned char *)(s ? s : "");
    size_t at = *o;

    put(buf, len, o, (const char *)u, strlen((const char *)u));
    if (!lower || *o == at)
        return;
    if (u[0] >= 'A' && u[0] <= 'Z')
        buf[at] = (char)(u[0] + 32);
    else if (u[0] == 0xc3 && *o > at + 1 && u[1] >= 0x80 && u[1] <= 0x9e
             && u[1] != 0x97)           /* U+00C0..U+00DE, not U+00D7 */
        buf[at + 1] = (char)(u[1] + 32);
}

size_t locale_format(char *buf, size_t len, const char *pat,
                     const struct locale_when *w,
                     const struct locale_names *n)
{
    size_t o = 0;

    if (!len)
        return 0;
    buf[0] = '\0';
    if (!pat)
        return 0;
    while (*pat && o < len - 1)
    {
        bool lower = false;
        char c;

        if (*pat != '$')
        {
            const char *e = strchr(pat, '$');
            size_t k = e ? (size_t)(e - pat) : strlen(pat);
            put(buf, len, &o, pat, k);
            pat += k;
            continue;
        }
        pat++;
        if (*pat == '_')
        {
            lower = true;
            pat++;
        }
        c = *pat;
        if (!c)
            break;
        pat++;
        switch (c)
        {
            case 'j': put_num(buf, len, &o, w->mday, 1); break;
            case 'd': put_num(buf, len, &o, w->mday, 2); break;
            case 'n': put_num(buf, len, &o, w->mon, 1); break;
            case 'm': put_num(buf, len, &o, w->mon, 2); break;
            case 'y': put_num(buf, len, &o, w->year % 100, 2); break;
            case 'Y': put_num(buf, len, &o, w->year, 1); break;
            case 'G': put_num(buf, len, &o, w->hour, 1); break;
            case 'H': put_num(buf, len, &o, w->hour, 2); break;
            case 'g':
                put_num(buf, len, &o, w->hour % 12 ? w->hour % 12 : 12, 1);
                break;
            case 'i': put_num(buf, len, &o, w->min, 2); break;
            case 'A':
                put_name(buf, len, &o, w->hour < 12 ? n->am : n->pm, false);
                break;
            case 'M':
                put_name(buf, len, &o,
                         n->month ? n->month((w->mon - 1) % 12 + 1) : "",
                         lower);
                break;
            case 'D':
                put_name(buf, len, &o, n->wday ? n->wday(w->wday % 7) : "",
                         lower);
                break;
            case 'l':
                put_name(buf, len, &o,
                         n->wday_full ? n->wday_full(w->wday % 7) : "",
                         lower);
                break;
            case 't':
            {
                const char *tp = n->h12 ? n->time12 : n->time24;
                /* one level only: a time pattern holding $t prints none */
                if (tp && !strstr(tp, "$t") && o < len - 1)
                    o += locale_format(buf + o, len - o, tp, w, n);
                break;
            }
            case '$': put(buf, len, &o, "$", 1); break;
            default:                    /* unknown: shown, so it is seen */
                put(buf, len, &o, "$", 1);
                put(buf, len, &o, pat - 1, 1);
                break;
        }
    }
    return strlen(buf);
}

bool locale_date_order(const char *pat, char order[3])
{
    int k = 0;
    bool seen[3] = { false, false, false };

    while (pat && *pat && k < 3)
    {
        int f = -1;

        if (*pat++ != '$')
            continue;
        if (*pat == '_')
            pat++;
        switch (*pat)
        {
            case 'j': case 'd': f = 0; break;
            case 'n': case 'm': case 'M': f = 1; break;
            case 'y': case 'Y': f = 2; break;
            default: break;
        }
        if (*pat)
            pat++;
        if (f >= 0 && !seen[f])
        {
            seen[f] = true;
            order[k++] = "dmy"[f];
        }
    }
    if (k == 3)
        return true;
    order[0] = 'm';
    order[1] = 'd';
    order[2] = 'y';
    return false;
}

void locale_decimal(char *buf, size_t len, unsigned long whole,
                    unsigned long frac, int digits, const char *sep)
{
    if (!sep || !*sep)
        sep = ".";
    if (digits <= 0)
        snprintf(buf, len, "%lu", whole);
    else
        snprintf(buf, len, "%lu%s%0*lu", whole, sep, digits, frac);
}

void locale_value_row(char *buf, size_t len, const char *name, char sep,
                      const char *value)
{
    size_t v = strlen(value), room, k;

    if (!len)
        return;
    if (v + 2 > len)                    /* the value alone fills it */
        v = locale_utf8_cut(value, len > 2 ? len - 2 : 0);
    room = len - 2 - v;                 /* name bytes: sep and NUL left */
    k = locale_utf8_cut(name, room);
    memcpy(buf, name, k);
    buf[k] = sep;
    memcpy(buf + k + 1, value, v);
    buf[k + 1 + v] = '\0';
}

/* -- Line breaks for East Asian text (#266) ------------------------------
 * Japanese has no spaces between words: a line may break between any two
 * kana or kanji. The wrap loops ask ui_wrap_next() for the end of the next
 * unit instead of the next space. A unit is a run of other characters up
 * to a space or an East Asian character, or one East Asian character, with
 * the closing marks that may not start a line attached (kinsoku: 、。」）
 * ？！ー, small kana) and an opening bracket kept with what follows. */
static unsigned utf8_at(const char *s, int i, int *len)
{
    const unsigned char *u = (const unsigned char *)s + i;

    if (u[0] < 0xc0 || !u[1])
    {
        *len = 1;
        return u[0];
    }
    if (u[0] < 0xe0)
    {
        *len = 2;
        return ((u[0] & 0x1f) << 6) | (u[1] & 0x3f);
    }
    if (!u[2])
    {
        *len = 1;
        return u[0];
    }
    *len = 3;                           /* the BMP is all the shell draws */
    return ((u[0] & 0x0f) << 12) | ((u[1] & 0x3f) << 6) | (u[2] & 0x3f);
}

static bool is_cjk(unsigned c)
{
    return c >= 0x3000 && c < 0xffa0;
}

static bool no_line_start(unsigned c)
{
    static const unsigned short marks[] = {
        0x3001, 0x3002, 0x300d, 0x300f, 0x3011, 0x3009, 0x300b, 0x301c,
        0x3041, 0x3043, 0x3045, 0x3047, 0x3049, 0x3063, 0x3083, 0x3085,
        0x3087, 0x30a1, 0x30a3, 0x30a5, 0x30a7, 0x30a9, 0x30c3, 0x30e3,
        0x30e5, 0x30e7, 0x30fb, 0x30fc, 0xff01, 0xff09, 0xff0c, 0xff0e,
        0xff1a, 0xff1b, 0xff1f,
    };
    unsigned i;

    if (c == '.' || c == ',' || c == ')' || c == '!' || c == '?' || c == ':')
        return true;
    for (i = 0; i < sizeof(marks) / sizeof(marks[0]); i++)
        if (marks[i] == c)
            return true;
    return false;
}

static bool opens(unsigned c)
{
    return c == 0x300c || c == 0x300e || c == 0x3010 || c == 0x3008
        || c == 0x300a || c == 0xff08;
}

int locale_wrap_next(const char *s, int j)
{
    int l;
    unsigned c;

    while (s[j] == ' ')
        j++;
    if (!s[j])
        return j;
    c = utf8_at(s, j, &l);
    if (is_cjk(c))
    {
        j += l;
        /* 「 （ stay with the whole unit they open: a CJK character, or a
         * Latin word ("（Apple" never splits after its first letter) */
        if (opens(c) && s[j] && s[j] != ' ')
            return locale_wrap_next(s, j);
    }
    else
    {
        while (s[j] && s[j] != ' ')
        {
            c = utf8_at(s, j, &l);
            if (is_cjk(c))
                break;
            j += l;
        }
    }
    while (s[j] && no_line_start(utf8_at(s, j, &l)))
        j += l;
    return j;
}
