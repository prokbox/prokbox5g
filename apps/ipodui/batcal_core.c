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
 * iPod UI shell: automatic battery calibration, the pure part (tracker
 * #70; docs/batcal-brief.md sections 1-3). See batcal_core.h.
 *
 * Recording: one bc_tick() per second. A run opens at unplug after the
 * "full" latch (charger with >= 500 mA, charging done for 2 min at
 * >= 4100 mV), and closes as partial at any power input, unless its last
 * sample is already within BC_END_MV of disk-safe (then it has ended).
 * Each period's mean voltage and busy share make one 2-byte sample; when
 * the ring fills, pairs merge and the period doubles.
 *
 * Fit: each sample's charge weight is 1 + (BUSY_W - 1) x busy share (the
 * period is the same for all and cancels); Q, the cumulative charge at
 * the sample's midpoint, runs 0..1 over the run. Table point k is the
 * median voltage of the fit samples within a window around
 * Q = (1 - k/10)(1 - reserve): 4 % wide, 1.5 % past Q = 0.85 (the knee),
 * doubled while it holds fewer than 3 samples. Q is in 1/10000.
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
#include <string.h>
#include <stdio.h>
#include "crc32.h"
#include "ipodui/batcal_core.h"

#define Q_ONE       10000
#define Q_KNEE      8500
#define MED_MAX     64

void bc_init(struct bc_state *st, uint16_t disksafe)
{
    memset(st, 0, sizeof(*st));
    st->h.magic = BC_MAGIC;
    st->h.version = BC_VERSION;
    st->disksafe = disksafe;
    st->next_seq = 1;
}

uint16_t bc_merge(uint16_t a, uint16_t b)
{
    unsigned ma = BC_MV12(a), mb = BC_MV12(b);
    unsigned m = (ma == BC_SKIP || mb == BC_SKIP) ? BC_SKIP
                                                  : (ma + mb) / 2;
    return BC_PACK(m, (BC_BUSY(a) + BC_BUSY(b) + 1) / 2);
}

static void acc_clear(struct bc_state *st)
{
    st->acc_mv = 0;
    st->acc_n = st->acc_busy = 0;
    st->acc_disk = st->acc_skip = 0;
}

static bool near_end(const struct bc_hdr *h, uint16_t disksafe)
{
    return h->n && h->last_mv <= disksafe + BC_END_MV;
}

static enum bc_event push(struct bc_state *st, uint16_t *ring)
{
    struct bc_hdr *h = &st->h;
    unsigned mv = st->acc_mv / st->acc_n;
    unsigned busy = st->acc_disk ? 15
                  : (st->acc_busy * 16u + st->acc_n / 2) / st->acc_n;
    unsigned mv12 = mv <= 3000 ? 0 : mv - 3000;

    if (busy > 15)
        busy = 15;
    if (mv12 >= BC_SKIP)
        mv12 = BC_SKIP - 1;
    if (st->acc_skip)
        mv12 = BC_SKIP;
    ring[h->n++] = BC_PACK(mv12, busy);
    h->last_mv = mv;
    acc_clear(st);
    st->dirty = 1;
    if (h->n == BC_RING)
    {
        int i;
        if (h->period >= 0x8000)
        {
            h->flags = 0;           /* over a year on one charge: no */
            h->n = 0;
            return BC_EV_PARTIAL;
        }
        for (i = 0; i < BC_RING / 2; i++)
            ring[i] = bc_merge(ring[2 * i], ring[2 * i + 1]);
        h->n = BC_RING / 2;
        h->period *= 2;
    }
    return BC_EV_SAMPLE;
}

enum bc_event bc_tick(struct bc_state *st, uint16_t *ring,
                      const struct bc_in *in)
{
    struct bc_hdr *h = &st->h;

    st->boot_secs++;
    if (in->power)
    {
        st->was_power = 1;
        if (in->charge_ok && !in->charging && in->mv >= BC_FULL_MV)
        {
            if (st->full_secs < 0xffff)
                st->full_secs++;
        }
        else
            st->full_secs = 0;
        if ((h->flags & (BC_F_RUN | BC_F_ENDING)) != BC_F_RUN)
            return BC_EV_NONE;
        st->dirty = 1;
        if (near_end(h, st->disksafe))
        {
            h->flags |= BC_F_ENDING;
            return BC_EV_ENDED;
        }
        h->flags = 0;
        h->n = 0;
        return BC_EV_PARTIAL;
    }
    if (st->was_power)
    {
        bool full = st->full_secs >= BC_FULL_SECS;
        st->was_power = 0;
        st->full_secs = 0;
        if (full && !(h->flags & BC_F_ENDING))
        {
            h->flags = BC_F_RUN;
            h->seq = st->next_seq++;
            h->period = BC_PERIOD0;
            h->n = 0;
            h->boots = 0;
            h->last_mv = in->mv;
            st->run_secs = 0;
            acc_clear(st);
            st->dirty = 1;
            return BC_EV_OPEN;
        }
    }
    if ((h->flags & (BC_F_RUN | BC_F_ENDING)) != BC_F_RUN)
        return BC_EV_NONE;
    st->acc_mv += in->mv;
    st->acc_n++;
    if (in->busy)
        st->acc_busy++;
    if (in->disk)
        st->acc_disk = 1;
    if (st->run_secs < BC_SKIP_RUN || st->boot_secs <= BC_SKIP_BOOT)
        st->acc_skip = 1;
    st->run_secs++;
    if (st->acc_n < h->period)
        return BC_EV_NONE;
    return push(st, ring);
}

enum bc_write bc_write_plan(const struct bc_state *st, bool level_safe,
                            bool file_exists)
{
    if (!st->dirty || !level_safe)
        return BC_W_NONE;
    if (st->h.flags & BC_F_RUN)
        return BC_W_RUN;
    return file_exists ? BC_W_REMOVE : BC_W_NONE;
}

bool bc_write_due(const struct bc_state *st, bool urgent, bool spaced)
{
    return urgent || spaced
        || ((st->h.flags & BC_F_RUN) && near_end(&st->h, st->disksafe));
}

enum bc_boot bc_boot_decide(const struct bc_hdr *h, const uint16_t *ring,
                            uint16_t disksafe)
{
    uint32_t fit = 0, busy = 0;
    int i;

    if (!(h->flags & BC_F_RUN))
        return BC_BOOT_NONE;
    if (!(h->flags & BC_F_ENDING)
        && ((h->flags & BC_F_CLEAN) || !near_end(h, disksafe)))
        return (h->flags & BC_F_CLEAN) ? BC_BOOT_KEEP : BC_BOOT_DROP;
    for (i = 0; i < h->n; i++)
    {
        if (BC_MV12(ring[i]) != BC_SKIP)
            fit++;
        busy += BC_BUSY(ring[i]);
    }
    if ((uint32_t)h->n * h->period < BC_MIN_SECS || fit < BC_MIN_FIT
        || busy * 100 > (uint32_t)BC_MAX_BUSY_PC * 16 * h->n)
        return BC_BOOT_DROP;
    return BC_BOOT_FIT;
}

static unsigned weight(uint16_t s)
{
    return 256 + (BC_BUSY_W16 - 16) * BC_BUSY(s);
}

/* The median voltage of the fit samples within +-half of q (both 1/10000;
 * total is the run's summed weight), widening while under 3 samples */
static int curve_at(const struct bc_hdr *h, const uint16_t *ring,
                    uint32_t total, unsigned q)
{
    uint16_t v[MED_MAX];
    unsigned half = q > Q_KNEE ? 75 : 200;
    int n, i, j;

    for (;;)
    {
        uint32_t lo = (q > half ? q - half : 0) * total;
        uint32_t hi = (q + half) * total;
        uint32_t cum = 0;

        n = 0;
        for (i = 0; i < h->n; i++)
        {
            unsigned w = weight(ring[i]);
            uint32_t x = (2 * cum + w) * (Q_ONE / 2);   /* midpoint */
            unsigned m = BC_MV12(ring[i]);
            cum += w;
            if (m == BC_SKIP || x < lo || x > hi || n == MED_MAX)
                continue;
            for (j = n++; j > 0 && v[j - 1] > m; j--)
                v[j] = v[j - 1];    /* insertion: v stays sorted */
            v[j] = m;
        }
        if (n >= 3 || half >= 1600)
            break;
        half *= 2;
    }
    if (n == 0)
        return 0;
    return 3000 + (n & 1 ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2);
}

bool bc_sane(const uint16_t t[BC_POINTS])
{
    int k;

    if (t[10] < 3950 || t[10] > 4250 || t[0] < 3400 || t[0] > 3750)
        return false;
    for (k = 1; k < BC_POINTS; k++)
    {
        int d = t[k] - t[k - 1];
        if (d < 5 || d > 250)
            return false;
    }
    return true;
}

bool bc_fit_run(const struct bc_hdr *h, const uint16_t *ring,
                uint16_t disksafe, uint16_t out[BC_POINTS])
{
    uint32_t total = 0;
    int i, k;
    uint32_t first = 0;
    int qa, va, vb;

    for (i = 0; i < h->n; i++)
    {
        unsigned w = weight(ring[i]);
        if (!first && BC_MV12(ring[i]) != BC_SKIP)
            first = 2 * total + w;      /* the first fit sample, x2 */
        total += w;
    }
    if (!total)
        return false;
    /* before the first fit sample (the skipped surface charge) the curve
     * is extended along the slope of the 2 % after it */
    qa = first * (Q_ONE / 2) / total + 200;
    va = curve_at(h, ring, total, qa);
    vb = curve_at(h, ring, total, qa + 200);
    for (k = 0; k < BC_POINTS; k++)
    {
        int q = (10 - k) * (Q_ONE - BC_RESERVE_PM * 10) / 10;
        out[k] = q < qa - 200 ? va + (va - vb) * (qa - q) / 200
                              : curve_at(h, ring, total, q);
    }
    if (out[0] < disksafe + 20)
        out[0] = disksafe + 20;     /* 0 % stays "about to shut down" */
    return bc_sane(out);
}

int bc_fits_add(struct bc_fit *fits, int n, const struct bc_fit *f)
{
    int i;

    if (n >= BC_FITS)
    {
        memmove(&fits[0], &fits[1], (BC_FITS - 1) * sizeof(*fits));
        n = BC_FITS - 1;
    }
    for (i = n; i > 0 && fits[i - 1].seq > f->seq; i--)
        fits[i] = fits[i - 1];
    fits[i] = *f;
    return n + 1;
}

void bc_blend(const struct bc_fit *fits, int n, uint16_t out[BC_POINTS])
{
    int k;

    for (k = 0; k < BC_POINTS; k++)
    {
        unsigned a = fits[0].mv[k];
        unsigned b = n > 1 ? fits[1].mv[k] : a;
        if (n > 2)
        {
            unsigned c = fits[2].mv[k], t;
            if (a > b) { t = a; a = b; b = t; }
            if (b > c)
                b = a > c ? a : c;  /* the middle of three */
        }
        else
            b = (a + b) / 2;
        out[k] = b;
    }
}

uint32_t bc_run_crc(const struct bc_hdr *h, const uint16_t *ring)
{
    return crc_32(ring, h->n * 2u, crc_32(h, sizeof(*h), 0xffffffff));
}

bool bc_run_valid(const struct bc_hdr *h, const uint16_t *ring,
                  uint32_t crc, long size)
{
    return h->magic == BC_MAGIC && h->version == BC_VERSION
        && (h->flags & BC_F_RUN) && h->n < BC_RING
        && h->period >= BC_PERIOD0 && h->period % BC_PERIOD0 == 0
        && size == (long)(sizeof(*h) + h->n * 2u + 4)
        && bc_run_crc(h, ring) == crc;
}

/* ---- the learned-table file ------------------------------------------ */

static const char dat_head[] = "batcal 1\n";

size_t bc_dat_format(char *buf, size_t size, const struct bc_fit *fits,
                     int n)
{
    size_t len = sizeof(dat_head) - 1;
    int i, k, r;

    if (size <= len)
        return 0;
    memcpy(buf, dat_head, len);
    for (i = 0; i < n; i++)
    {
        const struct bc_fit *f = &fits[i];
        r = snprintf(buf + len, size - len, "%lu %s %u", (unsigned long)f->seq,
                     f->date[0] ? f->date : "-", (unsigned)f->hours);
        for (k = 0; r > 0 && k < BC_POINTS; k++)
        {
            len += r;
            r = len < size ? snprintf(buf + len, size - len, " %u%s",
                                      (unsigned)f->mv[k],
                                      k == BC_POINTS - 1 ? "\n" : "")
                           : -1;
        }
        if (r < 0 || (len += r) >= size)
            return 0;
    }
    r = snprintf(buf + len, size - len, "end %08lx\n",
                 (unsigned long)crc_32(buf, len, 0xffffffff));
    if (r < 0 || len + r >= size)
        return 0;
    return len + r;
}

/* An unsigned decimal or hex number after optional spaces; NULL if none */
static const char *num(const char *p, const char *end, unsigned long *v,
                       int base)
{
    const char *s;

    while (p < end && *p == ' ')
        p++;
    s = p;
    *v = 0;
    for (; p < end; p++)
    {
        int d = *p >= '0' && *p <= '9' ? *p - '0'
              : base == 16 && *p >= 'a' && *p <= 'f' ? *p - 'a' + 10 : -1;
        if (d < 0)
            break;
        *v = *v * base + d;
    }
    return p > s && p - s <= 10 ? p : NULL;
}

int bc_dat_parse(const char *buf, size_t len, struct bc_fit *fits)
{
    const char *p = buf + sizeof(dat_head) - 1, *end = buf + len, *e;
    unsigned long v, crc;
    int n = 0, k;

    /* the trailer: the last line, "end xxxxxxxx\n" */
    if (len < sizeof(dat_head) - 1 + 13
        || memcmp(buf, dat_head, sizeof(dat_head) - 1)
        || memcmp(end - 13, "end ", 4) || end[-1] != '\n')
        return 0;
    end -= 13;
    if (num(end + 4, end + 12, &crc, 16) != end + 12
        || crc != crc_32(buf, end - buf, 0xffffffff))
        return 0;
    while (p < end)
    {
        struct bc_fit *f = &fits[n];
        if (n == BC_FITS || !(p = num(p, end, &v, 10)))
            return 0;
        f->seq = v;
        while (p < end && *p == ' ')
            p++;
        for (e = p; e < end && *e != ' '; e++)
            ;
        if (e - p < 1 || e - p > 10)
            return 0;
        memcpy(f->date, p, e - p);
        f->date[e - p] = '\0';
        if (!(p = num(e, end, &v, 10)) || v > 0xffff)
            return 0;
        f->hours = v;
        for (k = 0; k < BC_POINTS; k++)
        {
            if (!(p = num(p, end, &v, 10)) || v > 0xffff)
                return 0;
            f->mv[k] = v;
        }
        if (p >= end || *p++ != '\n' || !bc_sane(f->mv))
            return 0;
        n++;
    }
    return n;
}
