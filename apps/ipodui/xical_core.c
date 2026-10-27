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

/* iPod UI shell: the Calendars index builder's pure core (P12 W3-C): the
 * iCalendar parser, the recurrence expander, the instance cap and the
 * index layout (xical_core.h). Host-tested in tests/xical */
#include <stdint.h>
#include <string.h>
#include <stddef.h>
#include "crc32.h"
#include "ipodui/tzpick.h"
#include "ipodui/xical_core.h"

#define DAY_S   86400LL

enum { C_NONE = 0, C_EVENT, C_TODO, C_TZ };
enum { S_NONE = 0, S_STD, S_DST };
enum { F_NONE = 0, F_DAILY, F_WEEKLY, F_MONTHLY, F_YEARLY, F_OTHER };

/* -- civil time ------------------------------------------------------------ */

static int64_t fdiv(int64_t a, int64_t b)       /* floor division, b > 0 */
{
    int64_t q = a / b;

    return (a % b < 0) ? q - 1 : q;
}

/* -- zones ----------------------------------------------------------------- */

static const struct xcal_zone z_utc = { 0, TZ_DST_NONE, XCAL_Z_UTC };

static int dst_at(int rule, int64_t utc, int std)
{
    if (rule == TZ_DST_NONE || utc < 0 || utc > 0xffffffffLL)
        return 0;
    return tz_dst_minutes(rule, (uint32_t)utc, std);
}

/* A wall time in zone z to the home zone's local time */
static int64_t to_home(const struct xcal_build *b, const struct xcal_zone *z,
                       int64_t wall)
{
    int64_t utc;

    if (z->kind == XCAL_Z_FLOAT)
        return wall;
    if (z->kind == XCAL_Z_TZ)
    {
        if (z->std == b->home_std && z->rule == b->home_rule)
            return wall;                /* the home zone: as written */
        utc = wall - z->std * 60LL;
        utc -= 60LL * dst_at(z->rule, utc, z->std);
    }
    else
        utc = wall;
    return utc + 60LL * (b->home_std + dst_at(b->home_rule, utc, b->home_std));
}

/* Common zone names for a TZID with no VTIMEZONE in its file (matched as
 * the TZID's tail, so "/mozilla.org/.../Europe/Paris" works too) */
static const struct { const char *id; int16_t std; uint8_t rule; } known[] = {
    { "America/New_York", -300, TZ_DST_US },
    { "America/Toronto", -300, TZ_DST_US },
    { "America/Detroit", -300, TZ_DST_US },
    { "America/Chicago", -360, TZ_DST_US },
    { "America/Denver", -420, TZ_DST_US },
    { "America/Phoenix", -420, TZ_DST_NONE },
    { "America/Los_Angeles", -480, TZ_DST_US },
    { "America/Vancouver", -480, TZ_DST_US },
    { "America/Anchorage", -540, TZ_DST_US },
    { "Pacific/Honolulu", -600, TZ_DST_NONE },
    { "America/Mexico_City", -360, TZ_DST_NONE },
    { "America/Sao_Paulo", -180, TZ_DST_NONE },
    { "Europe/London", 0, TZ_DST_EU },
    { "Europe/Dublin", 0, TZ_DST_EU },
    { "Europe/Lisbon", 0, TZ_DST_EU },
    { "Europe/Paris", 60, TZ_DST_EU },
    { "Europe/Berlin", 60, TZ_DST_EU },
    { "Europe/Madrid", 60, TZ_DST_EU },
    { "Europe/Rome", 60, TZ_DST_EU },
    { "Europe/Amsterdam", 60, TZ_DST_EU },
    { "Europe/Brussels", 60, TZ_DST_EU },
    { "Europe/Zurich", 60, TZ_DST_EU },
    { "Europe/Vienna", 60, TZ_DST_EU },
    { "Europe/Stockholm", 60, TZ_DST_EU },
    { "Europe/Oslo", 60, TZ_DST_EU },
    { "Europe/Copenhagen", 60, TZ_DST_EU },
    { "Europe/Prague", 60, TZ_DST_EU },
    { "Europe/Warsaw", 60, TZ_DST_EU },
    { "Europe/Athens", 120, TZ_DST_EU },
    { "Europe/Helsinki", 120, TZ_DST_EU },
    { "Europe/Kiev", 120, TZ_DST_EU },
    { "Europe/Kyiv", 120, TZ_DST_EU },
    { "Europe/Istanbul", 180, TZ_DST_NONE },
    { "Europe/Moscow", 180, TZ_DST_NONE },
    { "Asia/Dubai", 240, TZ_DST_NONE },
    { "Asia/Kolkata", 330, TZ_DST_NONE },
    { "Asia/Bangkok", 420, TZ_DST_NONE },
    { "Asia/Singapore", 480, TZ_DST_NONE },
    { "Asia/Hong_Kong", 480, TZ_DST_NONE },
    { "Asia/Shanghai", 480, TZ_DST_NONE },
    { "Asia/Seoul", 540, TZ_DST_NONE },
    { "Asia/Tokyo", 540, TZ_DST_NONE },
    { "Australia/Perth", 480, TZ_DST_NONE },
    { "Australia/Brisbane", 600, TZ_DST_NONE },
    { "Australia/Sydney", 600, TZ_DST_SOUTH },
    { "Australia/Melbourne", 600, TZ_DST_SOUTH },
    { "Australia/Hobart", 600, TZ_DST_SOUTH },
    { "Australia/Canberra", 600, TZ_DST_SOUTH },
    { "UTC", 0, TZ_DST_NONE },
    { "GMT", 0, TZ_DST_NONE },
    { "Etc/UTC", 0, TZ_DST_NONE },
};

static int tail_eq(const char *s, const char *t)
{
    size_t ls = strlen(s), lt = strlen(t);

    return ls >= lt && !strcmp(s + ls - lt, t)
           && (ls == lt || s[ls - lt - 1] == '/');
}

/* A VTIMEZONE's two parts to one of tzpick's rules (a 60 min shift) */
static uint8_t tz_rule(const struct xcal_tzdef *t)
{
    if (!t->has_dst || t->dst == t->std)
        return TZ_DST_NONE;
    if (t->dmon == 3 && t->dord == 2 && t->smon == 11 && t->sord == 1)
        return TZ_DST_US;
    if (t->dmon == 3 && t->dord == -1 && t->smon == 10 && t->sord == -1)
        return TZ_DST_EU;
    if (t->dmon == 10 && t->dord == 1 && t->smon == 4 && t->sord == 1)
        return TZ_DST_SOUTH;
    return TZ_DST__N;                   /* not one of ours */
}

/* The zone of a date property: UTC, its TZID's, or floating */
static void zone_of(struct xcal_build *b, const struct xcal_dt *dt,
                    const char *tzid, struct xcal_zone *z)
{
    unsigned i;

    z->std = 0;
    z->rule = TZ_DST_NONE;
    z->kind = XCAL_Z_FLOAT;
    if (dt->utc)
    {
        z->kind = XCAL_Z_UTC;
        return;
    }
    if (dt->date_only || !tzid[0])
        return;
    for (i = 0; i < b->ntz; i++)
        if (!strcmp(b->tz[i].id, tzid))
        {
            uint8_t r = tz_rule(&b->tz[i]);

            z->kind = XCAL_Z_TZ;
            z->std = b->tz[i].std;
            if (r == TZ_DST__N)
            {
                b->tz_unknown++;        /* its standard time only */
                r = TZ_DST_NONE;
            }
            z->rule = r;
            return;
        }
    for (i = 0; i < sizeof(known) / sizeof(known[0]); i++)
        if (tail_eq(tzid, known[i].id))
        {
            z->kind = XCAL_Z_TZ;
            z->std = known[i].std;
            z->rule = known[i].rule;
            return;
        }
    b->tz_unknown++;                    /* read as floating */
}

/* -- small parsers --------------------------------------------------------- */

static int upc(int c)
{
    return c >= 'a' && c <= 'z' ? c - 32 : c;
}

/* s (len n) equals the upper-case word w, any case */
static int word_eq(const char *s, int n, const char *w)
{
    int i;

    for (i = 0; i < n; i++)
        if (!w[i] || upc((unsigned char)s[i]) != w[i])
            return 0;
    return !w[i];
}

static int digits(const char *s, int n, int *out)
{
    int i, v = 0;

    for (i = 0; i < n; i++)
    {
        if (s[i] < '0' || s[i] > '9')
            return 0;
        v = v * 10 + s[i] - '0';
    }
    *out = v;
    return 1;
}

/* "YYYYMMDD" or "YYYYMMDDTHHMMSS[Z]" */
static int parse_dt(const char *v, int date_param, struct xcal_dt *dt)
{
    int y, mo, d, h = 0, mi = 0, s = 0;

    memset(dt, 0, sizeof(*dt));
    dt->tz = 0xff;
    if (!digits(v, 4, &y) || !digits(v + 4, 2, &mo) || !digits(v + 6, 2, &d)
        || mo < 1 || mo > 12 || d < 1 || d > 31)
        return 0;
    if (v[8] == 'T' || v[8] == 't')
    {
        if (!digits(v + 9, 2, &h) || !digits(v + 11, 2, &mi))
            return 0;
        if (!digits(v + 13, 2, &s))
            s = 0;
        else
            dt->utc = (v[15] == 'Z' || v[15] == 'z');
        if (h > 23 || mi > 59 || s > 60)
            return 0;
    }
    else
        dt->date_only = 1;
    if (date_param)
    {
        dt->date_only = 1;
        h = mi = s = 0;
        dt->utc = 0;
    }
    dt->wall = xcal_days(y, mo, d) * DAY_S + h * 3600LL + mi * 60LL + s;
    dt->set = 1;
    return 1;
}

/* "[+-]P[nW][nD][T[nH][nM][nS]]" in seconds; a negative one is 0 */
static int64_t parse_dur(const char *v)
{
    int64_t total = 0, n = 0;
    int neg = 0, t = 0;

    if (*v == '+' || *v == '-')
        neg = *v++ == '-';
    if (upc((unsigned char)*v++) != 'P')
        return -1;
    for (; *v; v++)
    {
        int c = upc((unsigned char)*v);

        if (c >= '0' && c <= '9')
        {
            n = n * 10 + (c - '0');
            continue;
        }
        switch (c)
        {
            case 'T': t = 1; break;
            case 'W': total += n * 7 * DAY_S; break;
            case 'D': total += n * DAY_S; break;
            case 'H': if (t) total += n * 3600; break;
            case 'M': if (t) total += n * 60; break;
            case 'S': if (t) total += n; break;
            default: return -1;
        }
        n = 0;
    }
    return neg ? 0 : total;
}

/* A TEXT value, unescaped into out (cap bytes with the NUL), cut at a
 * UTF-8 boundary; newlines become spaces unless keep_nl */
static void text(char *out, int cap, const char *v, int keep_nl)
{
    int o = 0;

    while (*v && o < cap - 1)
    {
        char c = *v++;

        if (c == '\\' && *v)
        {
            c = *v++;
            if (c == 'n' || c == 'N')
                c = keep_nl ? '\n' : ' ';
        }
        else if (c == '\r' || c == '\n' || c == '\t')
            c = keep_nl && c == '\n' ? '\n' : ' ';
        out[o++] = c;
    }
    if (((unsigned char)*v & 0xc0) == 0x80)
    {
        /* cut inside a character: never half of one */
        while (o > 0 && ((unsigned char)out[o - 1] & 0xc0) == 0x80)
            o--;
        if (o > 0)
            o--;
    }
    out[o] = '\0';
}

static int wd_code(const char *s)        /* "SU".."SA" -> 0..6, else -1 */
{
    static const char codes[] = "SUMOTUWETHFRSA";
    int i;

    for (i = 0; i < 7; i++)
        if (upc((unsigned char)s[0]) == codes[2 * i]
            && upc((unsigned char)s[1]) == codes[2 * i + 1])
            return i;
    return -1;
}

/* One BYDAY item "[+-][n]XX": its ordinal (0 = every) and weekday */
static int byday_item(const char *s, int n, int8_t *ord, uint8_t *wd)
{
    int sign = 1, v = 0, i = 0, w;

    if (n < 2)
        return 0;
    if (s[0] == '+' || s[0] == '-')
        sign = s[i++] == '-' ? -1 : 1;
    while (i < n - 2 && s[i] >= '0' && s[i] <= '9')
        v = v * 10 + s[i++] - '0';
    if (i != n - 2 || (w = wd_code(s + i)) < 0 || v > 53)
        return 0;
    *ord = (int8_t)(sign * v);
    *wd = (uint8_t)w;
    return 1;
}

static int atoi_s(const char *s, int n)
{
    int sign = 1, v = 0, i = 0;

    if (n > 0 && (s[0] == '+' || s[0] == '-'))
        sign = s[i++] == '-' ? -1 : 1;
    for (; i < n && s[i] >= '0' && s[i] <= '9'; i++)
        v = v * 10 + s[i] - '0';
    return sign * v;
}

/* RRULE "FREQ=..;KEY=a,b;.." into the event (or a VTIMEZONE part's
 * month and ordinal when part != NULL) */
static void parse_rrule(struct xcal_ev *e, const char *v, int8_t *pmon,
                        int8_t *pord)
{
    while (*v)
    {
        const char *k = v, *val, *end;
        int kl;

        while (*v && *v != '=' && *v != ';')
            v++;
        kl = (int)(v - k);
        if (*v != '=')
        {
            if (*v)
                v++;
            continue;
        }
        val = ++v;
        while (*v && *v != ';')
            v++;
        end = v;
        if (*v)
            v++;
        if (pmon)                       /* a VTIMEZONE part */
        {
            if (word_eq(k, kl, "BYMONTH"))
                *pmon = (int8_t)atoi_s(val, (int)(end - val));
            else if (word_eq(k, kl, "BYDAY"))
            {
                int8_t o;
                uint8_t w;

                if (byday_item(val, (int)(end - val), &o, &w))
                    *pord = o;
            }
            continue;
        }
        if (word_eq(k, kl, "FREQ"))
        {
            int n = (int)(end - val);

            e->freq = word_eq(val, n, "DAILY") ? F_DAILY
                    : word_eq(val, n, "WEEKLY") ? F_WEEKLY
                    : word_eq(val, n, "MONTHLY") ? F_MONTHLY
                    : word_eq(val, n, "YEARLY") ? F_YEARLY : F_OTHER;
        }
        else if (word_eq(k, kl, "INTERVAL"))
        {
            int n = atoi_s(val, (int)(end - val));

            e->interval = (uint16_t)(n >= 1 && n <= 1000 ? n : 1);
        }
        else if (word_eq(k, kl, "COUNT"))
        {
            int n = atoi_s(val, (int)(end - val));

            e->count = n > 0 ? (uint32_t)n : 1;
        }
        else if (word_eq(k, kl, "UNTIL"))
        {
            char tmp[20];
            int n = (int)(end - val);

            n = n < (int)sizeof(tmp) - 1 ? n : (int)sizeof(tmp) - 1;
            memcpy(tmp, val, n);
            tmp[n] = '\0';
            parse_dt(tmp, 0, &e->until);
        }
        else if (word_eq(k, kl, "BYDAY"))
        {
            while (val < end && e->nbd < XCAL_BYDAY_MAX)
            {
                const char *it = val;

                while (val < end && *val != ',')
                    val++;
                if (byday_item(it, (int)(val - it), &e->bd_ord[e->nbd],
                               &e->bd_wd[e->nbd]))
                    e->nbd++;
                if (val < end)
                    val++;
            }
        }
        else if (word_eq(k, kl, "BYMONTHDAY"))
        {
            while (val < end && e->nbymd < XCAL_BYMD_MAX)
            {
                const char *it = val;
                int n;

                while (val < end && *val != ',')
                    val++;
                n = atoi_s(it, (int)(val - it));
                if (n != 0 && n >= -31 && n <= 31)
                    e->bymd[e->nbymd++] = (int8_t)n;
                if (val < end)
                    val++;
            }
        }
        else if (word_eq(k, kl, "BYMONTH"))
        {
            while (val < end)
            {
                const char *it = val;
                int n;

                while (val < end && *val != ',')
                    val++;
                n = atoi_s(it, (int)(val - it));
                if (n >= 1 && n <= 12)
                    e->bymon |= (uint16_t)(1u << n);
                if (val < end)
                    val++;
            }
        }
        else if (word_eq(k, kl, "WKST"))
        {
            int w = (end - val) >= 2 ? wd_code(val) : -1;

            if (w >= 0)
                e->wkst = (uint8_t)w;
        }
        else if (word_eq(k, kl, "BYSETPOS") || word_eq(k, kl, "BYWEEKNO")
                 || word_eq(k, kl, "BYYEARDAY"))
            e->unsup = 1;
    }
}

static uint16_t uid_hash(const char *s)
{
    uint32_t h = 0x811c9dc5u;

    while (*s)
        h = (h ^ (uint8_t)*s++) * 0x01000193u;
    return (uint16_t)((h ^ (h >> 15) ^ (h >> 30)) & 0x7fff);
}

/* -- the collection: a max-heap on the start ------------------------------- */

static int rec_cmp(const struct xcal_rec *a, const struct xcal_rec *b)
{
    if (a->start_rtc != b->start_rtc)
        return a->start_rtc < b->start_rtc ? -1 : 1;
    if (a->end_rtc != b->end_rtc)
        return a->end_rtc < b->end_rtc ? -1 : 1;
    if (a->title_off != b->title_off)
        return a->title_off < b->title_off ? -1 : 1;
    return 0;
}

static void swap_at(struct xcal_build *b, uint32_t i, uint32_t j)
{
    struct xcal_rec t = b->rec[i];
    uint16_t u = b->uid[i];

    b->rec[i] = b->rec[j];
    b->rec[j] = t;
    b->uid[i] = b->uid[j];
    b->uid[j] = u;
}

static void sift_down(struct xcal_build *b, uint32_t i, uint32_t n)
{
    for (;;)
    {
        uint32_t l = 2 * i + 1, m = i;

        if (l < n && rec_cmp(&b->rec[l], &b->rec[m]) > 0)
            m = l;
        if (l + 1 < n && rec_cmp(&b->rec[l + 1], &b->rec[m]) > 0)
            m = l + 1;
        if (m == i)
            return;
        swap_at(b, i, m);
        i = m;
    }
}

static void sift_up(struct xcal_build *b, uint32_t i)
{
    while (i > 0)
    {
        uint32_t p = (i - 1) / 2;

        if (rec_cmp(&b->rec[i], &b->rec[p]) <= 0)
            return;
        swap_at(b, i, p);
        i = p;
    }
}

static void heap_add(struct xcal_build *b, const struct xcal_rec *r,
                     uint16_t uid)
{
    b->instances++;
    if (b->n < XCAL_MAX_INSTANCES)
    {
        b->rec[b->n] = *r;
        b->uid[b->n] = uid;
        sift_up(b, b->n++);
        return;
    }
    b->cut++;                           /* full: the latest goes */
    if (rec_cmp(r, &b->rec[0]) >= 0)
        return;
    b->rec[0] = *r;
    b->uid[0] = uid;
    sift_down(b, 0, b->n);
}

/* -- strings --------------------------------------------------------------- */

static uint32_t pool_put(struct xcal_build *b, const char *s, uint32_t keep)
{
    uint32_t len = (uint32_t)strlen(s) + 1, at = b->pool_used;

    if (at + len + keep > XCAL_POOL)
        return 0;                       /* offset 0: "" */
    memcpy(b->pool + at, s, len);
    b->pool_used += len;
    return at;
}

/* -- the expander ---------------------------------------------------------- */

struct exp {
    struct xcal_build *b;
    const struct xcal_ev *e;
    struct xcal_zone z;
    int64_t start, dur;
    int64_t until_home, until_day;
    uint32_t n;                         /* occurrences counted */
    uint32_t toff, loff;
    uint16_t flags;
    uint8_t allday, done, strings;
};

static void put_strings(struct exp *x)
{
    struct xcal_build *b = x->b;
    const struct xcal_ev *e = x->e;
    uint32_t free_after;

    x->strings = 1;
    x->toff = e->title[0] ? pool_put(b, e->title, 2) : 0;
    if (e->title[0] && !x->toff)
        b->pool_cut++;
    x->loff = 0;
    free_after = XCAL_POOL - b->pool_used;
    if (!e->loc[0] && !e->desc[0])
        return;
    if (free_after > XCAL_POOL / 10)
    {
        uint32_t at = pool_put(b, e->loc, 1);

        if (at)
        {
            x->loff = at;
            if (!e->desc[0] || free_after <= XCAL_POOL / 4
                || !pool_put(b, e->desc, 0))
            {
                b->pool[b->pool_used++] = '\0';     /* no description */
                if (e->desc[0])
                    b->pool_cut++;
            }
            return;
        }
    }
    b->pool_cut++;
}

static int excluded(const struct exp *x, int64_t wall, int64_t home)
{
    const struct xcal_build *b = x->b;
    const struct xcal_ev *e = x->e;
    int i;

    for (i = 0; i < e->nex; i++)
        if ((e->ex_date[i] || x->allday) ? fdiv(wall, DAY_S) == e->ex[i]
                                         : home == e->ex[i])
            return 1;
    for (i = 0; i < b->novr; i++)
        if (b->ovr[i].uid == e->uid && b->ovr[i].at == home
            && b->ovr[i].cal == b->cal)
            return 1;
    return 0;
}

static void occur(struct exp *x, int64_t wall)
{
    struct xcal_build *b = x->b;
    int64_t home, end;
    struct xcal_rec r;

    if (wall < x->start)
        return;                         /* before DTSTART: not counted */
    home = x->allday ? wall : to_home(b, &x->z, wall);
    if (x->e->until.set
        && (x->e->until.date_only ? fdiv(wall, DAY_S) > x->until_day
                                  : home > x->until_home))
    {
        x->done = 1;
        return;
    }
    if (x->e->count && ++x->n > x->e->count)
    {
        x->done = 1;
        return;
    }
    if (home >= b->win1)
    {
        x->done = 1;
        return;
    }
    end = home + x->dur;
    if ((x->dur ? end : home + 1) <= b->win0 || excluded(x, wall, home))
        return;
    if (!x->strings)
        put_strings(x);
    r.start_rtc = (uint32_t)(home < 0 ? 0 : home);
    r.end_rtc = (uint32_t)(end < 0 ? 0 : end > 0xffffffffLL ? 0xffffffffLL
                                                            : end);
    r.title_off = x->toff;
    r.loc_off = x->loff;
    r.cal = b->cal;
    r.flags = x->flags;
    heap_add(b, &r, x->e->uid);
}

/* BYMONTHDAY's days of month (y, m): bit d (1..31) */
static uint32_t md_mask(const struct xcal_ev *e, int y, int m)
{
    int dim = xcal_mdays(y, m), i, d;
    uint32_t md = 0;

    for (i = 0; i < e->nbymd; i++)
    {
        d = e->bymd[i] > 0 ? e->bymd[i] : dim + 1 + e->bymd[i];
        if (d >= 1 && d <= dim)
            md |= 1u << d;
    }
    return md;
}

/* The days of month (y, m) the rule picks: bit d (1..31) */
static uint32_t month_mask(const struct xcal_ev *e, int y, int m, int mday)
{
    int dim = xcal_mdays(y, m), i, d;
    uint32_t md = md_mask(e, y, m), bd = 0;

    if (e->nbd)
    {
        int first = xcal_wday(xcal_days(y, m, 1));

        for (i = 0; i < e->nbd; i++)
        {
            int d1 = 1 + (e->bd_wd[i] - first + 7) % 7;
            int o = e->bd_ord[i];

            if (!o)
                for (d = d1; d <= dim; d += 7)
                    bd |= 1u << d;
            else if (o > 0)
            {
                d = d1 + 7 * (o - 1);
                if (d <= dim)
                    bd |= 1u << d;
            }
            else
            {
                d = d1 + 7 * ((dim - d1) / 7) + 7 * (o + 1);
                if (d >= 1)
                    bd |= 1u << d;
            }
        }
    }
    if (e->nbymd && e->nbd)
        return md & bd;
    if (e->nbymd)
        return md;
    if (e->nbd)
        return bd;
    return mday <= dim ? 1u << mday : 0;
}

static uint8_t wd_mask(const struct xcal_ev *e)
{
    uint8_t m = 0;
    int i;

    for (i = 0; i < e->nbd; i++)
        m |= (uint8_t)(1u << e->bd_wd[i]);
    return m;
}

static void expand(struct exp *x)
{
    struct xcal_build *b = x->b;
    const struct xcal_ev *e = x->e;
    int64_t sday = fdiv(x->start, DAY_S), tod = x->start - sday * DAY_S;
    int64_t k, k0 = 0, stop = b->win1 + 2 * DAY_S, day, p0 = 0;
    int sy, sm, sd, swd = xcal_wday(sday), iv = e->interval ? e->interval : 1;
    uint32_t it = 0;
    uint8_t wdm = wd_mask(e);

    xcal_civil(sday, &sy, &sm, &sd);
    if (!e->freq || e->freq == F_OTHER || e->unsup)
    {
        if (e->freq)
            b->rr_unsup++;
        occur(x, x->start);
        return;
    }
    if (!e->count)
    {
        /* no COUNT: start a period or so before the window */
        int64_t target = b->win0 - x->dur - 2 * DAY_S;

        if (target > x->start)
        {
            int ty, tm, td;

            xcal_civil(fdiv(target, DAY_S), &ty, &tm, &td);
            switch (e->freq)
            {
                case F_DAILY:
                    k0 = (target - x->start) / (iv * DAY_S) - 1;
                    break;
                case F_WEEKLY:
                    k0 = (target - x->start) / (iv * 7 * DAY_S) - 1;
                    break;
                case F_MONTHLY:
                    k0 = ((ty * 12LL + tm) - (sy * 12LL + sm)) / iv - 1;
                    break;
                default:
                    k0 = (ty - sy) / iv - 1;
                    break;
            }
            if (k0 < 0)
                k0 = 0;
        }
    }
    for (k = k0; !x->done; k++)
    {
        if (++it > XCAL_ITER_MAX)
        {
            b->iter_cut++;
            break;
        }
        switch (e->freq)
        {
            case F_DAILY:
            {
                int y, m, d;

                day = p0 = sday + k * iv;
                xcal_civil(day, &y, &m, &d);
                if ((wdm && !(wdm & (1u << xcal_wday(day))))
                    || (e->bymon && !(e->bymon & (1u << m)))
                    || (e->nbymd && !(md_mask(e, y, m) & (1u << d))))
                    break;                  /* BYDAY etc. filter a day */
                occur(x, day * DAY_S + tod);
                break;
            }
            case F_WEEKLY:
            {
                int64_t ws = sday - (swd - e->wkst + 7) % 7 + 7 * iv * k;
                int j;

                p0 = ws;
                for (j = 0; j < 7 && !x->done; j++)
                {
                    int wd = (e->wkst + j) % 7, y, m, d;

                    if (!((wdm ? wdm : (1u << swd)) & (1u << wd)))
                        continue;
                    day = ws + j;
                    if (e->bymon)
                    {
                        xcal_civil(day, &y, &m, &d);
                        if (!(e->bymon & (1u << m)))
                            continue;
                    }
                    occur(x, day * DAY_S + tod);
                }
                break;
            }
            case F_MONTHLY:
            {
                int64_t mi = (sy * 12LL + sm - 1) + k * iv;
                int y = (int)fdiv(mi, 12), m = (int)(mi - y * 12LL) + 1, d;
                uint32_t mask;

                p0 = xcal_days(y, m, 1);
                if (e->bymon && !(e->bymon & (1u << m)))
                    break;
                mask = month_mask(e, y, m, sd);
                for (d = 1; d <= 31 && !x->done; d++)
                    if (mask & (1u << d))
                        occur(x, (p0 + d - 1) * DAY_S + tod);
                break;
            }
            default:                    /* F_YEARLY */
            {
                int y = sy + (int)(k * iv), m, d;
                uint16_t months = e->bymon ? e->bymon : (uint16_t)(1u << sm);

                p0 = xcal_days(y, 1, 1);
                for (m = 1; m <= 12 && !x->done; m++)
                {
                    uint32_t mask;
                    int64_t m0;

                    if (!(months & (1u << m)))
                        continue;
                    mask = month_mask(e, y, m, sd);
                    m0 = xcal_days(y, m, 1);
                    for (d = 1; d <= 31 && !x->done; d++)
                        if (mask & (1u << d))
                            occur(x, (m0 + d - 1) * DAY_S + tod);
                }
                break;
            }
        }
        if (p0 * DAY_S > stop)
            break;                      /* past the window */
    }
}

/* -- components ------------------------------------------------------------ */

static int64_t span(struct xcal_build *b, const struct xcal_zone *zs,
                    int allday)
{
    const struct xcal_ev *e = &b->ev;
    struct xcal_zone ze;
    int64_t d;

    if (e->end.set)
    {
        if (allday || e->end.date_only)
            d = fdiv(e->end.wall, DAY_S) * DAY_S
                - fdiv(e->start.wall, DAY_S) * DAY_S;
        else
        {
            zone_of(b, &e->end, e->tzid_e[0] ? e->tzid_e : e->tzid_s, &ze);
            d = to_home(b, &ze, e->end.wall) - to_home(b, zs, e->start.wall);
        }
    }
    else if (e->has_dur)
        d = e->dur;
    else
        d = allday ? DAY_S : 0;
    return d < 0 ? 0 : d;
}

static void event_end(struct xcal_build *b)
{
    struct xcal_ev *e = &b->ev;
    struct exp x;
    int i;

    if (!e->start.set)
    {
        b->bad++;
        return;
    }
    memset(&x, 0, sizeof(x));
    x.b = b;
    x.e = e;
    x.allday = e->start.date_only;
    zone_of(b, &e->start, e->tzid_s, &x.z);
    x.start = e->start.wall;
    x.dur = span(b, &x.z, x.allday);
    x.flags = (uint16_t)((x.allday ? XCAL_ALL_DAY : 0)
                         | (e->alarm ? XCAL_ALARM : 0));
    if (e->rid.set)
    {
        /* one instance of a series, moved or cancelled */
        struct xcal_zone zr;

        zone_of(b, &e->rid, e->tzid_r[0] ? e->tzid_r : e->tzid_s, &zr);
        if (b->novr < XCAL_OVR_MAX)
        {
            b->ovr[b->novr].at = e->rid.date_only ? e->rid.wall
                                                  : to_home(b, &zr, e->rid.wall);
            b->ovr[b->novr].uid = e->uid;
            b->ovr[b->novr].cal = b->cal;
            b->novr++;
        }
        if (e->cancelled)
            return;
        b->events++;
        e->freq = F_NONE;               /* the override is that one only */
        e->nex = 0;
        e->uid |= XCAL_UID_OVR;         /* never removed by its own rid */
        expand(&x);
        return;
    }
    if (e->cancelled)
        return;
    b->events++;
    if (e->until.set && !e->until.date_only)
    {
        struct xcal_zone zu;

        zone_of(b, &e->until, e->until.utc ? "" : e->tzid_s, &zu);
        x.until_home = to_home(b, &zu, e->until.wall);
        if (x.allday)
        {
            x.until_day = fdiv(e->until.wall, DAY_S);
            e->until.date_only = 1;     /* compared by day */
        }
    }
    else if (e->until.set)
        x.until_day = fdiv(e->until.wall, DAY_S);
    /* EXDATEs in place: a day, or the home local time */
    for (i = 0; i < e->nex; i++)
        e->ex[i] = (e->ex_date[i] || x.allday)
                   ? fdiv(e->ex[i], DAY_S)
                   : to_home(b, e->ex_tz[i] == 'z' ? &z_utc : &x.z, e->ex[i]);
    expand(&x);
}

static int todo_cmp(const struct xcal_rec *a, const struct xcal_rec *b)
{
    /* undated last */
    if ((a->start_rtc == 0) != (b->start_rtc == 0))
        return a->start_rtc == 0 ? 1 : -1;
    return rec_cmp(a, b);
}

static void todo_end(struct xcal_build *b)
{
    struct xcal_ev *e = &b->ev;
    struct xcal_rec r;
    struct xcal_zone z;
    int64_t due = 0;

    if (e->done || e->cancelled)
        return;
    if (b->ntodo >= XCAL_TODO_MAX)
    {
        b->todo_cut++;
        return;
    }
    memset(&r, 0, sizeof(r));
    if (e->end.set)
    {
        zone_of(b, &e->end, e->tzid_e, &z);
        due = e->end.date_only ? e->end.wall : to_home(b, &z, e->end.wall);
        if (due <= 0 || due > 0xffffffffLL)
            due = 0;
        if (e->end.date_only)
            r.flags |= XCAL_ALL_DAY;
    }
    r.start_rtc = r.end_rtc = (uint32_t)due;
    r.title_off = e->title[0] ? pool_put(b, e->title, 2) : 0;
    r.cal = b->cal;
    r.flags |= XCAL_TODO;
    b->todo[b->ntodo++] = r;
}

static void part_end(struct xcal_build *b, int dst)
{
    struct xcal_tzdef *t;

    if (b->ntz >= XCAL_TZ_MAX)
        return;
    t = &b->tz[b->ntz];
    if (dst)
    {
        if (t->has_dst && b->p_start < t->dstart)
            return;                     /* an older rule */
        t->has_dst = 1;
        t->dst = b->p_off;
        t->dmon = b->p_mon;
        t->dord = b->p_ord;
        t->dstart = b->p_start;
    }
    else
    {
        if (t->sstart && b->p_start < t->sstart)
            return;
        t->std = b->p_off;
        t->smon = b->p_mon;
        t->sord = b->p_ord;
        t->sstart = b->p_start ? b->p_start : 1;
    }
}

/* "+HHMM" / "-HHMM" in minutes */
static int16_t utc_offset(const char *v)
{
    int sign = *v == '-' ? -1 : 1, h, m;

    if (*v == '+' || *v == '-')
        v++;
    if (!digits(v, 2, &h) || !digits(v + 2, 2, &m))
        return 0;
    return (int16_t)(sign * (h * 60 + m));
}

/* -- lines ----------------------------------------------------------------- */

static void date_prop(struct xcal_dt *dt, char *tzid, const char *ptz,
                      int pdate, const char *v)
{
    if (parse_dt(v, pdate, dt))
        text(tzid, XCAL_TZID_MAX, ptz, 0);
}

static void line_done(struct xcal_build *b)
{
    char *s = b->line, *v, *p, *name_end;
    char ptz[XCAL_TZID_MAX];
    int nlen, pdate = 0, q = 0;
    struct xcal_ev *e = &b->ev;

    s[b->len] = '\0';
    b->len = 0;
    b->trunc = 0;
    /* the name, the parameters, the value */
    for (p = s; *p && *p != ';' && *p != ':'; p++)
        ;
    name_end = p;
    nlen = (int)(name_end - s);
    ptz[0] = '\0';
    while (*p == ';')
    {
        char *k = ++p, *val;
        int kl;

        while (*p && *p != '=' && *p != ';' && *p != ':')
            p++;
        kl = (int)(p - k);
        if (*p != '=')
            continue;
        val = ++p;
        for (; *p && (q || (*p != ';' && *p != ':')); p++)
            if (*p == '"')
                q = !q;
        if (word_eq(k, kl, "TZID"))
        {
            int n = (int)(p - val);

            if (n && *val == '"')
            {
                val++;
                n -= 2;
            }
            if (n < 0)
                n = 0;
            if (n > XCAL_TZID_MAX - 1)
                n = XCAL_TZID_MAX - 1;
            memcpy(ptz, val, n);
            ptz[n] = '\0';
        }
        else if (word_eq(k, kl, "VALUE"))
            pdate = word_eq(val, (int)(p - val), "DATE");
    }
    if (*p != ':')
        return;                         /* not a property line */
    v = p + 1;

    if (b->depth)                       /* inside a component we skip */
    {
        if (word_eq(s, nlen, "BEGIN"))
            b->depth++;
        else if (word_eq(s, nlen, "END"))
            b->depth--;
        return;
    }
    if (word_eq(s, nlen, "BEGIN"))
    {
        int n = (int)strlen(v);

        if (b->comp == C_NONE && word_eq(v, n, "VEVENT"))
            b->comp = C_EVENT;
        else if (b->comp == C_NONE && word_eq(v, n, "VTODO"))
            b->comp = C_TODO;
        else if (b->comp == C_NONE && word_eq(v, n, "VTIMEZONE"))
        {
            b->comp = C_TZ;
            if (b->ntz < XCAL_TZ_MAX)
                memset(&b->tz[b->ntz], 0, sizeof(b->tz[0]));
            return;
        }
        else if (b->comp == C_TZ && (word_eq(v, n, "STANDARD")
                                     || word_eq(v, n, "DAYLIGHT")))
        {
            b->sub = word_eq(v, n, "DAYLIGHT") ? S_DST : S_STD;
            b->p_start = 0;
            b->p_off = 0;
            b->p_mon = b->p_ord = 0;
            return;
        }
        else if (b->comp == C_EVENT && word_eq(v, n, "VALARM"))
        {
            b->in_alarm = 1;
            e->alarm = 1;
            return;
        }
        else if (b->comp == C_NONE && word_eq(v, n, "VCALENDAR"))
            return;
        else
        {
            b->depth = 1;               /* anything else: skipped whole */
            return;
        }
        memset(e, 0, sizeof(*e));
        e->interval = 1;
        e->wkst = 1;                    /* Monday */
        e->dur = -1;
        return;
    }
    if (word_eq(s, nlen, "END"))
    {
        int n = (int)strlen(v);

        if (b->in_alarm && word_eq(v, n, "VALARM"))
            b->in_alarm = 0;
        else if (b->comp == C_EVENT && word_eq(v, n, "VEVENT"))
        {
            event_end(b);
            b->comp = C_NONE;
        }
        else if (b->comp == C_TODO && word_eq(v, n, "VTODO"))
        {
            todo_end(b);
            b->comp = C_NONE;
        }
        else if (b->comp == C_TZ && b->sub && (word_eq(v, n, "STANDARD")
                                               || word_eq(v, n, "DAYLIGHT")))
        {
            part_end(b, b->sub == S_DST);
            b->sub = S_NONE;
        }
        else if (b->comp == C_TZ && word_eq(v, n, "VTIMEZONE"))
        {
            if (b->ntz < XCAL_TZ_MAX && b->tz[b->ntz].id[0])
                b->ntz++;
            b->comp = C_NONE;
        }
        return;
    }
    switch (b->comp)
    {
        case C_NONE:
            if (word_eq(s, nlen, "X-WR-CALNAME") && !b->cal_named && *v
                && b->cal < XCAL_MAX_CALS)
            {
                text(b->meta.name[b->cal], XCAL_NAME_MAX, v, 0);
                b->cal_named = 1;
            }
            return;
        case C_TZ:
            if (b->ntz >= XCAL_TZ_MAX)
                return;
            if (!b->sub)
            {
                if (word_eq(s, nlen, "TZID"))
                    text(b->tz[b->ntz].id, XCAL_TZID_MAX, v, 0);
            }
            else if (word_eq(s, nlen, "TZOFFSETTO"))
                b->p_off = utc_offset(v);
            else if (word_eq(s, nlen, "DTSTART"))
            {
                struct xcal_dt dt;

                if (parse_dt(v, 0, &dt))
                    b->p_start = dt.wall;
            }
            else if (word_eq(s, nlen, "RRULE"))
                parse_rrule(e, v, &b->p_mon, &b->p_ord);
            return;
        default:
            break;
    }
    if (b->in_alarm)
        return;
    if (word_eq(s, nlen, "SUMMARY"))
        text(e->title, XCAL_TITLE_MAX, v, 0);
    else if (word_eq(s, nlen, "LOCATION"))
        text(e->loc, XCAL_LOC_MAX, v, 0);
    else if (word_eq(s, nlen, "DESCRIPTION"))
        text(e->desc, XCAL_DESC_MAX, v, 1);
    else if (word_eq(s, nlen, "DTSTART"))
        date_prop(&e->start, e->tzid_s, ptz, pdate, v);
    else if (word_eq(s, nlen, "DTEND") || word_eq(s, nlen, "DUE"))
        date_prop(&e->end, e->tzid_e, ptz, pdate, v);
    else if (word_eq(s, nlen, "RECURRENCE-ID"))
        date_prop(&e->rid, e->tzid_r, ptz, pdate, v);
    else if (word_eq(s, nlen, "DURATION"))
    {
        e->dur = parse_dur(v);
        e->has_dur = e->dur >= 0;
    }
    else if (word_eq(s, nlen, "RRULE") && !e->freq)
        parse_rrule(e, v, NULL, NULL);
    else if (word_eq(s, nlen, "EXDATE"))
    {
        /* a list: "a,b,c" */
        while (*v && e->nex < XCAL_EXDATE_MAX)
        {
            struct xcal_dt dt;
            char *c = v;

            while (*c && *c != ',')
                c++;
            if (*c)
                *c++ = '\0';
            if (parse_dt(v, pdate, &dt))
            {
                e->ex[e->nex] = dt.wall;
                e->ex_date[e->nex] = dt.date_only;
                e->ex_tz[e->nex] = dt.utc ? 'z' : 's';
                e->nex++;
            }
            v = c;
        }
    }
    else if (word_eq(s, nlen, "STATUS"))
    {
        int n = (int)strlen(v);

        if (word_eq(v, n, "CANCELLED"))
            e->cancelled = 1;
        else if (word_eq(v, n, "COMPLETED"))
            e->done = 1;
    }
    else if (word_eq(s, nlen, "COMPLETED"))
        e->done = 1;
    else if (word_eq(s, nlen, "UID"))
        e->uid = uid_hash(v);
}

/* -- the file -------------------------------------------------------------- */

void xcal_begin(struct xcal_build *b)
{
    int y, m, d, y1, m1;
    int64_t today = fdiv((int64_t)b->now, DAY_S);

    xcal_civil(today, &y, &m, &d);
    /* [the first of last month, the first of the month 13 on) */
    m -= XCAL_WINDOW_BACK_MON;
    while (m < 1)
    {
        m += 12;
        y--;
    }
    y1 = y;
    m1 = m + XCAL_WINDOW_BACK_MON + XCAL_WINDOW_AHEAD_MON + 1;
    while (m1 > 12)
    {
        m1 -= 12;
        y1++;
    }
    b->win0 = xcal_days(y, m, 1) * DAY_S;
    b->win1 = xcal_days(y1, m1, 1) * DAY_S;
    b->ndays = (uint16_t)((b->win1 - b->win0) / DAY_S);
    b->rec = (struct xcal_rec *)(b->out + sizeof(struct xidx_hdr));
    b->pool = (char *)b->out + sizeof(struct xidx_hdr)
              + XCAL_MAX_INSTANCES * sizeof(struct xcal_rec) + XCAL_TAB_MAX;
    b->pool[0] = b->pool[1] = '\0';     /* "" and its empty description */
    b->pool_used = 2;
    b->n = b->ntodo = 0;
    memset(&b->meta, 0, sizeof(b->meta));
    b->cut = b->todo_cut = b->pool_cut = b->iter_cut = 0;
    b->tz_unknown = b->rr_unsup = b->events = b->instances = 0;
    b->files = b->cal_cut = b->bad = 0;
}

int xcal_file_begin(struct xcal_build *b, const char *name)
{
    int n = 0;

    if (b->meta.ncal >= XCAL_MAX_CALS)
    {
        b->cal_cut++;
        return -1;
    }
    b->cal = b->meta.ncal++;
    b->files++;
    /* the file's name without its extension, cut on a character */
    {
        const char *dot = strrchr(name, '.');
        int len = dot && dot != name ? (int)(dot - name) : (int)strlen(name);

        while (n < len && n < XCAL_NAME_MAX - 1)
            n++;
        if (n < len)
            while (n > 0 && ((unsigned char)name[n] & 0xc0) == 0x80)
                n--;
        memcpy(b->meta.name[b->cal], name, n);
        b->meta.name[b->cal][n] = '\0';
    }
    b->comp = b->sub = b->depth = b->in_alarm = 0;
    b->trunc = b->pending = b->ntz = b->cal_named = 0;
    b->novr = 0;
    b->len = 0;
    return 0;
}

void xcal_feed(struct xcal_build *b, const char *buf, uint32_t len)
{
    uint32_t i;

    for (i = 0; i < len; i++)
    {
        char c = buf[i];

        if (b->pending)
        {
            b->pending = 0;
            if (c == ' ' || c == '\t')
                continue;               /* folded: the line goes on */
            line_done(b);
        }
        if (c == '\n')
        {
            if (b->len && b->line[b->len - 1] == '\r')
                b->len--;
            b->pending = 1;
            continue;
        }
        if (b->len < XCAL_LINE_MAX - 1)
            b->line[b->len++] = c;
        else
            b->trunc = 1;
    }
}

void xcal_file_end(struct xcal_build *b)
{
    uint32_t i, j, n;

    if (b->pending || b->len)
    {
        if (b->len && b->line[b->len - 1] == '\r')
            b->len--;
        b->pending = 0;
        line_done(b);
    }
    if (!b->novr)
        return;
    /* the series instances that an override of this file replaced */
    for (i = j = 0, n = b->n; i < n; i++)
    {
        int gone = 0;
        uint32_t k;

        if (!(b->uid[i] & XCAL_UID_OVR) && b->rec[i].cal == b->cal)
            for (k = 0; k < b->novr && !gone; k++)
                gone = b->ovr[k].uid == b->uid[i]
                       && b->ovr[k].at == (int64_t)b->rec[i].start_rtc;
        if (gone)
            continue;
        b->rec[j] = b->rec[i];
        b->uid[j++] = b->uid[i];
    }
    b->n = j;
    for (i = j / 2; i-- > 0;)           /* a heap again */
        sift_down(b, i, j);
}

/* -- the index ------------------------------------------------------------- */

int32_t xcal_emit(struct xcal_build *b, uint32_t sig)
{
    struct xidx_hdr *h = (struct xidx_hdr *)b->out;
    struct xcal_meta *m;
    uint16_t *first, *ovl;
    uint8_t *bits;
    uint32_t i, j, total, tab, rec_off, str_off, bm, tb;
    int64_t d;
    struct xcal_rec *rec;

    /* the cap over events and To Do's: the latest events go */
    while (b->n && b->n + b->ntodo > XCAL_MAX_INSTANCES)
    {
        swap_at(b, 0, --b->n);
        sift_down(b, 0, b->n);
        b->cut++;
    }
    for (i = b->n; i > 1; i--)          /* heapsort: ascending */
    {
        swap_at(b, 0, i - 1);
        sift_down(b, 0, i - 1);
    }
    for (i = 1; i < b->ntodo; i++)      /* insertion sort, <= 128 */
    {
        struct xcal_rec t = b->todo[i];

        for (j = i; j > 0 && todo_cmp(&t, &b->todo[j - 1]) < 0; j--)
            b->todo[j] = b->todo[j - 1];
        b->todo[j] = t;
    }
    total = b->n + b->ntodo;
    /* the records down to their place, after the tables */
    tab = sizeof(*h);
    tb = XCAL_TAB_BYTES(b->ndays, b->meta.ncal);
    rec_off = (tab + tb + 3u) & ~3u;
    rec = (struct xcal_rec *)(b->out + rec_off);
    memmove(rec, b->rec, b->n * sizeof(struct xcal_rec));
    memcpy(rec + b->n, b->todo, b->ntodo * sizeof(struct xcal_rec));
    b->rec = rec;

    /* the tables */
    bm = (b->ndays + 7u) / 8u;
    m = (struct xcal_meta *)(b->out + tab);
    *m = b->meta;
    m->day0 = (uint32_t)b->win0;
    m->built = b->now;
    m->ndays = b->ndays;
    m->nevents = (uint16_t)b->n;
    m->bm_bytes = (uint16_t)bm;
    m->cut = (uint16_t)(b->cut > 0xffff ? 0xffff : b->cut);
    m->home_std = b->home_std;
    m->home_rule = b->home_rule;
    first = (uint16_t *)(b->out + tab + xcal_first_off());
    ovl = (uint16_t *)(b->out + tab + xcal_ovl_off(m));
    bits = b->out + tab + xcal_bits_off(m);
    memset(bits, 0, (m->ncal + 1u) * bm);
    for (i = 0, d = 0; d <= b->ndays; d++)
    {
        int64_t t = b->win0 + d * DAY_S;

        while (i < b->n && (int64_t)b->rec[i].start_rtc < t)
            i++;
        first[d] = (uint16_t)i;
        if (d < b->ndays)
            ovl[d] = (uint16_t)i;
    }
    for (i = 0; i < b->n; i++)
    {
        const struct xcal_rec *r = &b->rec[i];
        int64_t s = r->start_rtc, e = r->end_rtc > r->start_rtc
                                      ? (int64_t)r->end_rtc : s + 1;
        int64_t d0 = fdiv(s - b->win0, DAY_S), d1 = fdiv(e - 1 - b->win0,
                                                         DAY_S);

        if (d0 < 0)
            d0 = 0;
        if (d1 >= b->ndays)
            d1 = b->ndays - 1;
        if (d1 - d0 > XCAL_SPAN_DAYS)
            d1 = d0 + XCAL_SPAN_DAYS;
        for (d = d0; d <= d1; d++)
        {
            if (i < ovl[d])
                ovl[d] = (uint16_t)i;
            bits[d >> 3] |= (uint8_t)(1u << (d & 7));
            if (r->cal < m->ncal)
                bits[(r->cal + 1u) * bm + (d >> 3)] |= (uint8_t)(1u << (d & 7));
        }
    }

    memset(b->out + tab + tb, 0, rec_off - tab - tb);
    m->tab_crc = 0;
    m->tab_crc = crc_32(m, tb, 0xffffffffu);

    /* the strings, down to their place */
    str_off = rec_off + total * (uint32_t)sizeof(struct xcal_rec);
    memmove(b->out + str_off, b->pool, b->pool_used);
    b->pool = (char *)b->out + str_off;

    memset(h, 0, sizeof(*h));
    h->kind = XIDX_CAL;
    h->sig = sig;
    h->count = total;
    h->rec_off = rec_off;
    h->str_off = str_off;
    h->str_bytes = b->pool_used;
    xidx_seal(b->out, str_off + b->pool_used);
    return (int32_t)(str_off + b->pool_used);
}
