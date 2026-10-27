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
 * Host test of automatic battery calibration (apps/ipodui/batcal_core.c,
 * tracker #70; docs/batcal-brief.md "Tests and acceptance"): the fit on
 * synthetic discharges, the sanity and reserve rules, decimation over a
 * 60 h run, the blend and its order, the recorder's state machine, both
 * file formats (round trip, every truncation, every corrupted byte) and
 * the write policy below disk-safe.
 *
 *   test_batcal                  the unit tests
 *   test_batcal --replay <csv>   a power.csv-style trace through the
 *                                recorder; "expect=<event>" marks must
 *                                each see that event within their row
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
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "crc32.h"
#include "ipodui/batcal_core.h"

static int fails, checks;

#define CHECK(c) do { checks++; if (!(c)) { \
    fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #c); \
    fails++; } } while (0)

/* The firmware's crc_32 (firmware/common/crc32.c): MSB first, polynomial
 * 0x04C11DB7, no final xor; bitwise here */
uint32_t crc_32(const void *src, uint32_t len, uint32_t crc)
{
    const unsigned char *p = src;
    while (len--)
    {
        int b;
        crc ^= (uint32_t)*p++ << 24;
        for (b = 0; b < 8; b++)
            crc = crc & 0x80000000u ? (crc << 1) ^ 0x04c11db7u : crc << 1;
    }
    return crc;
}

#define DISKSAFE 3500

static uint16_t ring[BC_RING];
static struct bc_state st;

static uint32_t rng = 12345;
static int noise(int amp)           /* -amp..amp, deterministic */
{
    rng = rng * 1103515245u + 12345u;
    return (int)((rng >> 16) % (2 * amp + 1)) - amp;
}

/* A lithium-ion-like discharge curve: V at charge used q (0..1); its
 * slope changes gradually, as a real cell's does */
static double curve(double q)
{
    static const double pts[][2] = {
        {0.00, 4170}, {0.025, 4120}, {0.05, 4080}, {0.10, 4030},
        {0.15, 3990}, {0.30, 3910},
        {0.50, 3830}, {0.70, 3770}, {0.85, 3720}, {0.93, 3660},
        {0.97, 3580}, {1.00, 3490}, {1.10, 3300},
    };
    int i;
    for (i = 1; i < (int)(sizeof(pts) / sizeof(pts[0])); i++)
        if (q <= pts[i][0])
            return pts[i - 1][1] + (pts[i][1] - pts[i - 1][1])
                 * (q - pts[i - 1][0]) / (pts[i][0] - pts[i - 1][0]);
    return 3300;
}

static struct bc_in in_at(int mv)
{
    struct bc_in in;
    memset(&in, 0, sizeof(in));
    in.mv = mv;
    return in;
}

static enum bc_event tick_n(struct bc_in in, long n, enum bc_event want,
                            long *seen)
{
    enum bc_event last = BC_EV_NONE;
    long i;
    if (seen)
        *seen = 0;
    for (i = 0; i < n; i++)
    {
        enum bc_event e = bc_tick(&st, ring, &in);
        if (e != BC_EV_NONE && e != BC_EV_SAMPLE)
            last = e;
        if (seen && e == want)
            (*seen)++;
    }
    return last;
}

/* Charged to full on a 500 mA charger, then unplugged: a run opens */
static void open_run(void)
{
    struct bc_in in = in_at(4180);
    in.power = in.charge_ok = true;
    tick_n(in, 150, BC_EV_NONE, NULL);
    in.power = in.charge_ok = false;
    CHECK(bc_tick(&st, ring, &in) == BC_EV_OPEN);
}

/* ---- the fit on synthetic discharges ---------------------------------- */

/* Simulates a run second by second: idle current 1, busy current 1.5
 * (matches BC_BUSY_W16), a busy stretch of busy_len s every busy_every s
 * (sagging sag mV), noise, and a first-order lag of tau s on the voltage
 * (powermgmt's filter). Ends when the reading reaches end_mv. Returns the
 * fit's worst error against the true curve at table points 0..9. Point 10
 * (100 %) must lie between the curve at the first fit sample (after the
 * surface-charge skip, the earliest the record can see) and the curve at
 * Q = 0, within 15 mV: *full_err is its distance outside that band. */
static int fit_case(const char *name, double hours, int busy_every,
                    int busy_len, int sag, int amp, double tau, int end_mv,
                    bool *sane, int *full_err)
{
    double q = 0, total, lagv = 4170, step;
    long s, nsec;
    uint16_t t[BC_POINTS];
    int k, worst = 0;
    double qend, qs[32] = { 0 }, top;

    bc_init(&st, DISKSAFE);
    st.boot_secs = 100000;          /* no boot skip in this test */
    open_run();
    /* charge per second so the cell reaches q = 1 at ~hours with the
     * busy mix */
    nsec = (long)(hours * 3600);
    step = 1.0 / nsec;
    for (s = 0; ; s++)
    {
        bool busy = busy_every && (s % busy_every) < busy_len;
        /* surface charge: +40 mV fading over the first minutes, which
         * the recorder's 10-min skip keeps out of the fit */
        double v = curve(q) - (busy ? sag : 0) + 40 * exp(-s / 180.0);
        struct bc_in in;
        lagv += (v - lagv) / (tau > 1 ? tau : 1);
        in = in_at((int)lround(lagv) + noise(amp));
        in.busy = busy;
        bc_tick(&st, ring, &in);
        q += step * (busy ? 1.5 : 1.0);
        if (s % 60 == 59 && s / 60 < 31)
            qs[s / 60 + 1] = q;     /* charge used after each minute */
        if (st.acc_n == 0 && st.h.last_mv <= end_mv)
            break;                  /* the last write, near disk-safe */
        if (s > nsec * 3)
            break;
    }
    qend = q;
    total = qend;
    *sane = bc_fit_run(&st.h, ring, DISKSAFE, t);
    for (k = 0; k < BC_POINTS; k++)
    {
        double qk = (1.0 - k / 10.0) * (1.0 - BC_RESERVE_PM / 1000.0) * total;
        int want = (int)lround(curve(qk));
        int err;
        if (k == 0 && want < DISKSAFE + 20)
            want = DISKSAFE + 20;
        err = abs((int)t[k] - want);
        if (k == 10)
        {
            unsigned p = st.h.period, first = (BC_SKIP_RUN + p - 1) / p * p;
            top = curve(qs[first / 60] / total);
            *full_err = t[k] > want ? (int)t[k] - want
                      : t[k] < top ? (int)lround(top) - t[k] : 0;
        }
        else if (err > worst)
            worst = err;
        if (err > 15 && k < 10)
            fprintf(stderr, "  %s: point %d got %d want %d\n", name, k,
                    t[k], want);
    }
    printf("     fit %-18s n=%3u period=%4us worst=%2d mV 100%%=%+d mV "
           "sane=%d\n", name, st.h.n, st.h.period, worst,
           (int)t[10] - (int)lround(curve(0)), (int)*sane);
    return worst;
}

static void test_fit(void)
{
    static const struct {
        const char *name;
        double hours;
        int busy_every, busy_len, sag, amp;
        double tau;
        int full_tol;               /* 100 %: within this of the band */
    } c[] = {
        { "clean 20 h",       20,    0,   0,  0,  0,  1, 15 },
        { "noise +-12 mV",    20,    0,   0,  0, 12,  1, 15 },
        { "busy stretches",   20, 1800, 120, 25,  8,  1, 15 },
        { "filter lag 30 s",  20,    0,   0,  0,  8, 30, 15 },
        { "stock cell 8 h",    8,    0,   0,  0,  8,  1, 15 },
        { "60 h, lag, busy",  60, 3600, 300, 20, 10, 20, 15 },
        { "short 3 h",         3,    0,   0,  0,  8,  1, 15 },
    };
    unsigned i;
    for (i = 0; i < sizeof(c) / sizeof(c[0]); i++)
    {
        bool sane;
        int full_err = 0;
        CHECK(fit_case(c[i].name, c[i].hours, c[i].busy_every,
                       c[i].busy_len, c[i].sag, c[i].amp, c[i].tau, 3540,
                       &sane, &full_err) <= 15);
        CHECK(full_err <= c[i].full_tol);
        CHECK(sane);
    }
}

/* ---- sanity and the reserve ------------------------------------------- */

static const uint16_t good[BC_POINTS] =
    { 3600, 3700, 3740, 3770, 3800, 3830, 3870, 3930, 4000, 4080, 4170 };

static void test_sanity(void)
{
    uint16_t t[BC_POINTS];

    CHECK(bc_sane(good));
    memcpy(t, good, sizeof(t)); t[5] = t[4];            /* flat step */
    CHECK(!bc_sane(t));
    memcpy(t, good, sizeof(t)); t[5] = t[4] - 10;       /* rising */
    CHECK(!bc_sane(t));
    memcpy(t, good, sizeof(t)); t[10] = 4260;           /* full too high */
    CHECK(!bc_sane(t));
    memcpy(t, good, sizeof(t)); t[10] = 3940; t[9] = 3900;  /* too low */
    CHECK(!bc_sane(t));
    memcpy(t, good, sizeof(t)); t[0] = 3440;            /* 0 % too low */
    CHECK(!bc_sane(t));
    memcpy(t, good, sizeof(t)); t[0] = 3760; t[1] = 3770; /* 0 % high */
    CHECK(!bc_sane(t));
    memcpy(t, good, sizeof(t)); t[1] = 3851;            /* step > 250 */
    CHECK(!bc_sane(t));

    /* the reserve: a curve that falls under disk-safe + 20 at Q = 0.97
     * keeps its 0 % at disk-safe + 20 */
    {
        int i;
        bc_init(&st, DISKSAFE);
        st.h.flags = BC_F_RUN | BC_F_ENDING;
        st.h.period = 60;
        for (i = 0; i < 300; i++)
        {
            int mv = 4170 - i * 670 / 299;     /* linear to 3500 */
            ring[i] = BC_PACK(mv - 3000, 0);
        }
        st.h.n = 300;
        bc_fit_run(&st.h, ring, DISKSAFE, t);
        CHECK(t[0] == DISKSAFE + 20);
        CHECK(t[10] >= 4150);
        /* a flat (broken) record fails the sanity check */
        for (i = 0; i < 300; i++)
            ring[i] = BC_PACK(800, 0);
        CHECK(!bc_fit_run(&st.h, ring, DISKSAFE, t));
    }
}

/* ---- decimation ------------------------------------------------------- */

static void test_decimation(void)
{
    long s, secs = 60L * 3600;
    int i, skips = 0;
    struct bc_in in = in_at(3900);

    bc_init(&st, DISKSAFE);
    st.boot_secs = 100000;
    open_run();
    for (s = 0; s < secs; s++)
    {
        in.mv = 4100 - (int)(s / 400);  /* a slow ramp: order shows */
        bc_tick(&st, ring, &in);
    }
    /* 3600 one-minute samples: halved at 512 each time */
    CHECK(st.h.period == 60 * 8);
    CHECK((long)st.h.n * st.h.period + st.acc_n == secs);
    for (i = 0; i < st.h.n; i++)
    {
        if (BC_MV12(ring[i]) == BC_SKIP)
            skips++;
        else if (i > 0 && BC_MV12(ring[i - 1]) != BC_SKIP)
            CHECK(BC_MV12(ring[i]) <= BC_MV12(ring[i - 1]));
    }
    /* the first 10 min, merged into the first 480 s sample (and the one
     * after it holds minute 8-16, which includes minutes 8-10) */
    CHECK(skips == 2);
    CHECK(BC_MV12(ring[0]) == BC_SKIP && BC_MV12(ring[1]) == BC_SKIP);
    /* a sample at index i covers [i*period, (i+1)*period): its mean */
    {
        int mid = 100;
        long t0 = (long)mid * st.h.period + st.h.period / 2;
        int want = 4100 - (int)(t0 / 400);
        CHECK(abs((int)BC_MV12(ring[mid]) + 3000 - want) <= 2);
    }
    /* merge rules */
    CHECK(bc_merge(BC_PACK(700, 4), BC_PACK(701, 7)) == BC_PACK(700, 6));
    CHECK(BC_MV12(bc_merge(BC_PACK(BC_SKIP, 0), BC_PACK(700, 0)))
          == BC_SKIP);
}

/* ---- blend and order -------------------------------------------------- */

static struct bc_fit mkfit(uint32_t seq, int shift)
{
    struct bc_fit f;
    int k;
    memset(&f, 0, sizeof(f));
    f.seq = seq;
    strcpy(f.date, "-");
    f.hours = 20;
    for (k = 0; k < BC_POINTS; k++)
        f.mv[k] = good[k] + shift;
    return f;
}

static void test_blend(void)
{
    struct bc_fit fits[BC_FITS], f;
    uint16_t t[BC_POINTS];
    int n = 0;

    f = mkfit(5, 0);  n = bc_fits_add(fits, n, &f);
    bc_blend(fits, n, t);
    CHECK(t[3] == good[3]);
    f = mkfit(3, 20); n = bc_fits_add(fits, n, &f);   /* older, by seq */
    CHECK(n == 2 && fits[0].seq == 3 && fits[1].seq == 5);
    bc_blend(fits, n, t);
    CHECK(t[3] == good[3] + 10);
    f = mkfit(7, -10); n = bc_fits_add(fits, n, &f);
    bc_blend(fits, n, t);
    CHECK(t[3] == good[3]);                 /* median of +20, 0, -10 */
    f = mkfit(9, 40); n = bc_fits_add(fits, n, &f);   /* drops seq 3 */
    CHECK(n == 3 && fits[0].seq == 5 && fits[1].seq == 7
          && fits[2].seq == 9);
    bc_blend(fits, n, t);
    CHECK(t[3] == good[3]);                 /* median of 0, -10, +40 */
    CHECK(bc_sane(t));
}

/* ---- the state machine ------------------------------------------------ */

static void test_states(void)
{
    struct bc_in in;
    long seen;

    /* full latch: 2 min at >= 4100 mV, charging done, 500 mA */
    bc_init(&st, DISKSAFE);
    open_run();
    CHECK(st.h.flags == BC_F_RUN && st.h.seq == 1 && st.h.period == 60);

    /* misses */
    {
        static const struct { bool ok, chg; int mv; long secs; } miss[] = {
            { false, false, 4180, 300 },    /* USB charging off */
            { false, false, 4150, 300 },    /* a 100 mA host */
            { true,  true,  4150, 300 },    /* unplugged while charging */
            { true,  false, 4090, 300 },    /* under BC_FULL_MV */
            { true,  false, 4180, 119 },    /* not 2 min yet */
        };
        unsigned i;
        for (i = 0; i < sizeof(miss) / sizeof(miss[0]); i++)
        {
            bc_init(&st, DISKSAFE);
            in = in_at(miss[i].mv);
            in.power = true;
            in.charge_ok = miss[i].ok;
            in.charging = miss[i].chg;
            tick_n(in, miss[i].secs, BC_EV_NONE, NULL);
            in.power = false;
            CHECK(tick_n(in, 5, BC_EV_NONE, NULL) == BC_EV_NONE);
            CHECK(!(st.h.flags & BC_F_RUN));
        }
        /* the latch must be continuous: a charging blip resets it */
        bc_init(&st, DISKSAFE);
        in = in_at(4180);
        in.power = in.charge_ok = true;
        tick_n(in, 100, BC_EV_NONE, NULL);
        in.charging = true;
        tick_n(in, 1, BC_EV_NONE, NULL);
        in.charging = false;
        tick_n(in, 100, BC_EV_NONE, NULL);
        in.power = false;
        CHECK(tick_n(in, 1, BC_EV_NONE, NULL) == BC_EV_NONE);
    }

    /* USB (power, no charging allowed) mid-run: partial, discarded */
    bc_init(&st, DISKSAFE);
    open_run();
    tick_n(in_at(3900), 3600, BC_EV_NONE, NULL);
    in = in_at(3900);
    in.power = true;
    CHECK(tick_n(in, 1, BC_EV_NONE, NULL) == BC_EV_PARTIAL);
    CHECK(st.h.flags == 0 && st.h.n == 0 && st.dirty);

    /* a charger plug-in mid-run: partial */
    bc_init(&st, DISKSAFE);
    open_run();
    tick_n(in_at(3800), 3600, BC_EV_NONE, NULL);
    in = in_at(3850);
    in.power = in.charge_ok = in.charging = true;
    CHECK(tick_n(in, 1, BC_EV_NONE, NULL) == BC_EV_PARTIAL);
    /* ... and the next unplug, not full, opens nothing */
    in.power = false;
    CHECK(tick_n(in, 1, BC_EV_NONE, NULL) == BC_EV_NONE);

    /* plugged in at the run's end: kept as ending, fitted */
    bc_init(&st, DISKSAFE);
    open_run();
    tick_n(in_at(3530), 3 * 3600, BC_EV_NONE, NULL);
    in = in_at(3530);
    in.power = in.charge_ok = in.charging = true;
    CHECK(tick_n(in, 1, BC_EV_NONE, NULL) == BC_EV_ENDED);
    CHECK(st.h.flags == (BC_F_RUN | BC_F_ENDING));
    CHECK(bc_boot_decide(&st.h, ring, DISKSAFE) == BC_BOOT_FIT);
    /* charged full and unplugged before the fit: the ended run stays */
    in = in_at(4180);
    in.power = in.charge_ok = true;
    tick_n(in, 150, BC_EV_NONE, NULL);
    in.power = false;
    CHECK(tick_n(in, 1, BC_EV_NONE, NULL) == BC_EV_NONE);
    CHECK(st.h.flags == (BC_F_RUN | BC_F_ENDING));

    /* a clean reboot carries on; its first 5 min are skip samples */
    bc_init(&st, DISKSAFE);
    st.boot_secs = 100000;
    open_run();
    tick_n(in_at(3900), 3600, BC_EV_NONE, NULL);
    st.h.flags |= BC_F_CLEAN;
    CHECK(bc_boot_decide(&st.h, ring, DISKSAFE) == BC_BOOT_KEEP);
    {
        struct bc_hdr h = st.h;
        uint16_t n0 = h.n;
        bc_init(&st, DISKSAFE);         /* the next boot */
        st.h = h;
        st.h.flags &= ~BC_F_CLEAN;
        st.h.boots++;
        st.run_secs = (uint32_t)h.n * h.period;
        tick_n(in_at(3890), 600, BC_EV_NONE, NULL);
        CHECK(st.h.n == n0 + 10);
        CHECK(BC_MV12(ring[n0 + 4]) == BC_SKIP);   /* minute 5 */
        CHECK(BC_MV12(ring[n0 + 5]) == 890);       /* minute 6 */
    }

    /* a hard reset or crash mid-run (open, not clean, mid voltage): drop */
    bc_init(&st, DISKSAFE);
    open_run();
    tick_n(in_at(3800), 4 * 3600, BC_EV_NONE, NULL);
    CHECK(bc_boot_decide(&st.h, ring, DISKSAFE) == BC_BOOT_DROP);

    /* the normal end: open, last write near disk-safe, long enough */
    tick_n(in_at(3540), 3600, BC_EV_NONE, NULL);
    CHECK(bc_boot_decide(&st.h, ring, DISKSAFE) == BC_BOOT_FIT);
    /* a brown-out under load: the last write below disk-safe */
    tick_n(in_at(3460), 60, BC_EV_NONE, NULL);
    CHECK(bc_boot_decide(&st.h, ring, DISKSAFE) == BC_BOOT_FIT);
    /* a clean shutdown near the end carries on instead */
    st.h.flags |= BC_F_CLEAN;
    CHECK(bc_boot_decide(&st.h, ring, DISKSAFE) == BC_BOOT_KEEP);
    st.h.flags &= ~BC_F_CLEAN;

    /* too short (under 2 h) */
    bc_init(&st, DISKSAFE);
    open_run();
    tick_n(in_at(3540), 7000, BC_EV_NONE, NULL);
    CHECK(bc_boot_decide(&st.h, ring, DISKSAFE) == BC_BOOT_DROP);
    /* 2 h of on-time but under 120 fit samples (10 min skipped) */
    tick_n(in_at(3540), 200, BC_EV_NONE, NULL);
    CHECK(st.h.n == 120);
    CHECK(bc_boot_decide(&st.h, ring, DISKSAFE) == BC_BOOT_DROP);
    tick_n(in_at(3540), 600, BC_EV_NONE, NULL);
    CHECK(bc_boot_decide(&st.h, ring, DISKSAFE) == BC_BOOT_FIT);

    /* too busy: over 30 % */
    bc_init(&st, DISKSAFE);
    open_run();
    in = in_at(3540);
    in.busy = true;
    tick_n(in, 3 * 3600, BC_EV_NONE, &seen);
    CHECK(bc_boot_decide(&st.h, ring, DISKSAFE) == BC_BOOT_DROP);
    /* a disk-active period counts as wholly busy */
    bc_init(&st, DISKSAFE);
    open_run();
    in = in_at(3900);
    in.disk = true;
    tick_n(in, 1, BC_EV_NONE, NULL);
    in.disk = false;
    tick_n(in, 59, BC_EV_NONE, NULL);
    CHECK(st.h.n == 1 && BC_BUSY(ring[0]) == 15);

    /* no run, no file */
    bc_init(&st, DISKSAFE);
    CHECK(bc_boot_decide(&st.h, ring, DISKSAFE) == BC_BOOT_NONE);
}

/* ---- writes and files ------------------------------------------------- */

static void test_write_plan(void)
{
    bc_init(&st, DISKSAFE);
    open_run();
    CHECK(bc_write_plan(&st, true, false) == BC_W_RUN);
    CHECK(bc_write_plan(&st, false, true) == BC_W_NONE);  /* below safe */
    st.dirty = 0;
    CHECK(bc_write_plan(&st, true, true) == BC_W_NONE);   /* unchanged */
    st.h.flags = 0;
    st.dirty = 1;
    CHECK(bc_write_plan(&st, true, true) == BC_W_REMOVE);
    CHECK(bc_write_plan(&st, false, true) == BC_W_NONE);
    CHECK(bc_write_plan(&st, true, false) == BC_W_NONE);

    /* the 5-min spacing holds mid-run writes back, but not near the end:
     * the next chance may be the last above disk-safe (review M1) */
    bc_init(&st, DISKSAFE);
    open_run();
    tick_n(in_at(3800), 600, BC_EV_NONE, NULL);
    CHECK(!bc_write_due(&st, false, false));
    CHECK(bc_write_due(&st, false, true));
    CHECK(bc_write_due(&st, true, false));
    tick_n(in_at(3545), 60, BC_EV_NONE, NULL);
    CHECK(bc_write_due(&st, false, false));
    st.h.flags = 0;                 /* no run: spacing again */
    CHECK(!bc_write_due(&st, false, false));
}

/* The glue's read: header, samples (at most BC_RING), trailer, size */
static bool load(const unsigned char *buf, long len, struct bc_hdr *h,
                 uint16_t *r)
{
    uint32_t crc = 0;
    long off = sizeof(*h);
    memset(h, 0, sizeof(*h));
    if (len < off)
        return false;
    memcpy(h, buf, sizeof(*h));
    if (h->n >= BC_RING || off + h->n * 2 + 4 > len)
        return false;
    memcpy(r, buf + off, h->n * 2);
    memcpy(&crc, buf + off + h->n * 2, 4);
    return bc_run_valid(h, r, crc, len);
}

static void test_files(void)
{
    static unsigned char buf[sizeof(struct bc_hdr) + BC_RING * 2 + 4];
    static uint16_t r2[BC_RING];
    struct bc_hdr h2;
    uint32_t crc;
    long len, i;
    int bad = 0;

    bc_init(&st, DISKSAFE);
    open_run();
    tick_n(in_at(3900), 5 * 3600, BC_EV_NONE, NULL);
    st.h.flags |= BC_F_CLEAN;
    len = sizeof(st.h);
    memcpy(buf, &st.h, len);
    memcpy(buf + len, ring, st.h.n * 2);
    len += st.h.n * 2;
    crc = bc_run_crc(&st.h, ring);
    memcpy(buf + len, &crc, 4);
    len += 4;
    CHECK(load(buf, len, &h2, r2));
    CHECK(!memcmp(&h2, &st.h, sizeof(h2)) && !memcmp(r2, ring, h2.n * 2));
    for (i = 0; i < len; i++)           /* every truncation */
        if (load(buf, i, &h2, r2))
            bad++;
    CHECK(bad == 0);
    for (i = 0; i < len; i++)           /* every corrupted byte */
    {
        buf[i] ^= 0x5a;
        if (load(buf, len, &h2, r2))
            bad++;
        buf[i] ^= 0x5a;
    }
    CHECK(bad == 0);
    CHECK(!load(buf, len + 1, &h2, r2));    /* trailing junk */

    /* the learned-table file, with an unset clock in one fit */
    {
        struct bc_fit fits[BC_FITS], back[BC_FITS];
        char dat[400];
        size_t dl;
        int n;
        fits[0] = mkfit(4, 0);
        fits[1] = mkfit(6, 10);
        strcpy(fits[1].date, "2026-10-11");
        fits[2] = mkfit(4000000000u, -5);
        dl = bc_dat_format(dat, sizeof(dat), fits, 3);
        CHECK(dl > 0 && dl < sizeof(dat));
        CHECK(bc_dat_parse(dat, dl, back) == 3);
        CHECK(!memcmp(back[1].mv, fits[1].mv, sizeof(back[1].mv)));
        CHECK(!strcmp(back[0].date, "-") && back[2].seq == 4000000000u);
        CHECK(!strcmp(back[1].date, "2026-10-11") && back[1].hours == 20);
        bad = 0;
        for (i = 0; i < (long)dl; i++)
            if (bc_dat_parse(dat, i, back))
                bad++;
        CHECK(bad == 0);
        for (i = 0; i < (long)dl; i++)
        {
            dat[i] ^= 0x01;
            if (bc_dat_parse(dat, dl, back))
                bad++;
            dat[i] ^= 0x01;
        }
        CHECK(bad == 0);
        CHECK(bc_dat_format(dat, 100, fits, 3) == 0);   /* too small */
        n = bc_dat_parse(dat, bc_dat_format(dat, sizeof(dat), fits, 0),
                         back);
        CHECK(n == 0);
    }
}

/* ---- the replay -------------------------------------------------------- */

static const char *ev_name[] = { "none", "sample", "open", "partial",
                                 "ended" };

static int col(char **f, int nf, const char *name, char **hdr, int nh)
{
    int i;
    for (i = 0; i < nh && i < nf; i++)
        if (!strcmp(hdr[i], name))
            return atoi(f[i]);
    return 0;
}

static int split(char *line, char **f, int max)
{
    int n = 0;
    char *p = line;
    while (n < max)
    {
        f[n++] = p;
        p = strchr(p, ',');
        if (!p)
            break;
        *p++ = '\0';
    }
    p = f[n - 1] + strlen(f[n - 1]);
    while (p > f[n - 1] && (p[-1] == '\n' || p[-1] == '\r'))
        *--p = '\0';
    return n;
}

static int replay(const char *path)
{
    static char hline[512], line[512];
    char *hdr[32], *f[32];
    int nh, nf, mark = -1, i, expects = 0, missed = 0;
    long prev_s = -1, rows = 0;
    int counts[5] = { 0 };
    FILE *fp = fopen(path, "r");

    if (!fp || !fgets(hline, sizeof(hline), fp))
    {
        fprintf(stderr, "replay: can't read %s\n", path);
        return 1;
    }
    nh = split(hline, hdr, 32);
    for (i = 0; i < nh; i++)
        if (!strcmp(hdr[i], "mark"))
            mark = i;
    bc_init(&st, DISKSAFE);
    while (fgets(line, sizeof(line), fp))
    {
        struct bc_in in;
        long s, dt, k;
        int bl, boost, disk, busy_s, disk_s, seen[5] = { 0 };
        const char *m;

        if (line[0] == '#')
            continue;
        nf = split(line, f, 32);
        s = col(f, nf, "s", hdr, nh);
        m = mark >= 0 && mark < nf ? f[mark] : "";
        if (!strncmp(m, "boot", 4))
        {
            st.boot_secs = 0;           /* a new boot: RAM state kept as
                                         * if the run file carried it */
            st.h.boots++;
            prev_s = s;
            continue;
        }
        dt = prev_s < 0 ? 1 : s - prev_s;
        prev_s = s;
        if (dt <= 0)
            continue;               /* a mark row between periodic rows */
        rows++;
        memset(&in, 0, sizeof(in));
        in.mv = col(f, nf, "mv", hdr, nh);
        in.power = col(f, nf, "charger", hdr, nh) != 0;
        in.charge_ok = in.power;    /* the owner's own charger */
        in.charging = col(f, nf, "charging", hdr, nh) != 0;
        bl = col(f, nf, "bl_pct", hdr, nh);
        boost = col(f, nf, "boost_pct", hdr, nh);
        disk = col(f, nf, "disk_pct", hdr, nh);
        busy_s = (int)(dt * (bl > boost ? (bl > disk ? bl : disk)
                                        : (boost > disk ? boost : disk))
                       / 100);
        disk_s = (int)(dt * disk / 100);
        for (k = 0; k < dt; k++)
        {
            enum bc_event e;
            in.busy = k < busy_s;
            in.disk = k < disk_s;
            e = bc_tick(&st, ring, &in);
            seen[e]++;
            if (e != BC_EV_NONE && e != BC_EV_SAMPLE)
            {
                counts[e]++;
                printf("     replay s=%ld mv=%d %s (n=%u)\n", s, in.mv,
                       ev_name[e], st.h.n);
            }
        }
        if (!strncmp(m, "expect=", 7))
        {
            const char *want = m + 7;
            bool ok = false;
            expects++;
            if (!strcmp(want, "fit"))
                ok = bc_boot_decide(&st.h, ring, DISKSAFE) == BC_BOOT_FIT;
            else if (!strcmp(want, "noopen"))
                ok = seen[BC_EV_OPEN] == 0;
            else if (!strcmp(want, "drop"))
                ok = bc_boot_decide(&st.h, ring, DISKSAFE) == BC_BOOT_DROP;
            else
                for (i = 0; i < 5; i++)
                    if (!strcmp(want, ev_name[i]))
                        ok = seen[i] > 0;
            if (!ok)
            {
                missed++;
                fprintf(stderr, "replay: s=%ld expected %s\n", s, want);
            }
            if (ok && !strcmp(want, "fit"))
            {
                uint16_t t[BC_POINTS];
                bool sane = bc_fit_run(&st.h, ring, DISKSAFE, t);
                printf("     replay fit sane=%d:", (int)sane);
                for (i = 0; i < BC_POINTS; i++)
                    printf(" %u", t[i]);
                printf("\n");
                if (!sane)
                    missed++;
            }
        }
    }
    fclose(fp);
    printf("     replay rows=%ld open=%d partial=%d ended=%d expects=%d\n",
           rows, counts[BC_EV_OPEN], counts[BC_EV_PARTIAL],
           counts[BC_EV_ENDED], expects);
    return missed ? 1 : 0;
}

int main(int argc, char **argv)
{
    if (argc == 3 && !strcmp(argv[1], "--replay"))
        return replay(argv[2]);
    test_fit();
    test_sanity();
    test_decimation();
    test_blend();
    test_states();
    test_write_plan();
    test_files();
    printf("     %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
