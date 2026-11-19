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

/* #352: the EQ preset tables per output-rate family, the pure part (host
 * test: tests/eqtable). See eqtable_core.h for the file format. */
#include <string.h>
#include "eqtable_core.h"
#include "eqlab_core.h"

static const char *const fam_names[EQTABLE_FAMILIES] = { "base", "hi" };

int eqtable_family_of(unsigned long rate_hz)
{
    return rate_hz > EQTABLE_BASE_MAX_HZ ? EQTABLE_HI : EQTABLE_BASE;
}

const char *eqtable_family_name(int family)
{
    return family >= 0 && family < EQTABLE_FAMILIES ? fam_names[family]
                                                    : "?";
}

const char *eqtable_err_name(int err)
{
    static const char *const names[] = {
        "ok", "size", "header", "version", "family", "count", "bad line",
        "short", "long" };

    return err >= 0 && err < (int)(sizeof(names) / sizeof(names[0]))
           ? names[err] : "?";
}

/* the literal lit at *s (within e): taken */
static bool take(const char **s, const char *e, const char *lit)
{
    size_t n = strlen(lit);

    if ((size_t)(e - *s) < n || memcmp(*s, lit, n))
        return false;
    *s += n;
    return true;
}

/* 1..4 decimal digits at *s (within e) */
static bool take_uint(const char **s, const char *e, int *v)
{
    int n = 0, digits = 0;

    while (*s < e && **s >= '0' && **s <= '9' && digits < 4)
    {
        n = n * 10 + (*(*s)++ - '0');
        digits++;
    }
    if (!digits || (*s < e && **s >= '0' && **s <= '9'))
        return false;
    *v = n;
    return true;
}

/* "eqtable <v> family=<name> n=<count>", the whole line [s, e) */
static int parse_header(const char *s, const char *e, int family, int *n)
{
    int v, f;

    if (!take(&s, e, "eqtable ") || !take_uint(&s, e, &v))
        return EQT_HEADER;
    if (v != EQTABLE_VERSION)
        return EQT_VERSION;     /* before the rest: v2 may lay it out
                                 * differently */
    if (!take(&s, e, " family="))
        return EQT_HEADER;
    for (f = 0; f < EQTABLE_FAMILIES; f++)
    {
        const char *t = s;

        if (take(&t, e, fam_names[f]) && t < e && *t == ' ')
        {
            s = t;
            break;
        }
    }
    if (f == EQTABLE_FAMILIES)
        return EQT_HEADER;
    if (f != family)
        return EQT_FAMILY;
    if (!take(&s, e, " n=") || !take_uint(&s, e, n) || s != e)
        return EQT_HEADER;
    return *n == EQTABLE_PRESETS ? EQT_OK : EQT_COUNT;
}

int eqtable_parse(const char *buf, int len, int family,
                  struct dac_eq_band (*out)[DAC_EQ_BANDS], int *line)
{
    const char *p = buf, *end = buf + (len > 0 ? len : 0);
    int ln = 0, n = 0, k = 0, err;
    bool header = false;

    if (line)
        *line = 0;
    if (len <= 0 || len > EQTABLE_FILE_MAX)
        return EQT_SIZE;
    while (p < end)
    {
        const char *nl = memchr(p, '\n', end - p);
        const char *e = nl ? nl : end;
        const char *next = nl ? nl + 1 : end;

        ln++;
        if (e > p && e[-1] == '\r')
            e--;
        if (line)
            *line = ln;
        if (e == p || *p == '#')
        {
            p = next;
            continue;           /* blank or comment */
        }
        if (!header)
        {
            err = parse_header(p, e, family, &n);
            if (err != EQT_OK)
                return err;
            header = true;
        }
        else
        {
            char tmp[EQLAB_TEXT_LEN];
            int l = (int)(e - p);

            if (k == n)
                return EQT_LONG;
            /* a NUL inside would hide the rest of the line from the
             * parser */
            if (l >= EQLAB_TEXT_LEN || memchr(p, '\0', l))
                return EQT_LINE;
            memcpy(tmp, p, l);
            tmp[l] = '\0';
            if (!eqlab_parse(tmp, out[k]))
                return EQT_LINE;
            k++;
        }
        p = next;
    }
    if (line)
        *line = 0;
    if (!header)
        return EQT_HEADER;
    return k < n ? EQT_SHORT : EQT_OK;
}
