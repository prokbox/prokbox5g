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
 * The PP5022 band governor's simulator (docs/design/pp-governor.md
 * section 5): the shipped policy (firmware/pgov_core.c) against today's
 * rules over load traces, on an energy proxy with bracketed [estimate]
 * parameters, misses and transitions.
 *
 *   sim             the regression table (fixed parameters)
 *   sim --grid      the policy sweep and the model grid: the A/B verdict
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
#include <stdint.h>
#include <stdbool.h>
#include "pgov.h"

#define DT_US       10000.0
#define PCM_CAP_US  3000000.0       /* pcmbuf: 3 s (MIN_BUFFER_SIZE) */
#define PCM_WM_US   2000000.0       /* the boost watermark: 2 s */
#define UI_SLOT_US  33000.0         /* a frame's slot */
/* a PLL start or retune: Apple's fixed 255 us wait [apple, mac-and-dvs.md
 * 3.1], the envelope; our polled lock measured 16-29 us typical, 95-217
 * worst [silicon 18-5] */
#define LOCK_US     255.0
#define MAXF        64

/* ---- Traces ------------------------------------------------------------
 * All synthetic [estimate], built from the silicon figures named beside
 * each; work is given in microseconds at 24 MHz (the clock most figures
 * were measured at) and scaled by the memory-bound model per run. */

struct sample {
    double cpu_us24;                /* fluid CPU work (kernel, storage, feed) */
    int nframes;                    /* UI frames arriving (COP, 19-9 split) */
    double frame_us24;
    bool boost;                     /* a trace cpu_boost() (glide, track load) */
    bool load_floor;                /* clockpol: this track needs > 24 MHz */
    bool ata;                       /* an ATA DMA transfer this sample */
    bool lit;
    bool cap80;                     /* a host-port push (band 3 cap) */
    bool playing;
};

struct scen {
    const char *name;
    int secs, weight;               /* weight: share of the day mix, % */
    double dec_share24;             /* decode: busy share at 24 MHz per audio */
    double frame_ms;                /* codec frame */
    double backlog_s80;             /* fixed work at start (s at 80 MHz) */
    void (*gen)(int k, struct sample *s);
    const char *src;
};

static uint32_t rng = 1;
static uint32_t rnd(void)
{
    rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
    return rng;
}
static double frnd(void) { return (rnd() & 0xffff) / 65536.0; }

static void g_background(int k, struct sample *s)
{
    s->cpu_us24 = 10.0;             /* tick, power thread: ~0.1 % */
    (void)k;
}

/* the flash refill: every 120 s, 2 s of transfers about 33 a second
 * [estimate; dark play disk share 7.4 %, darkplay-1614 power.csv] */
static void g_refill(int k, struct sample *s, int period_s)
{
    int t = k % (period_s * 100);
    if (t >= 500 && t < 700 && (k % 3) == 0)
    {
        s->ata = true;
        s->cpu_us24 += 200.0;
    }
}

/* a track load every 240 s boosts 0.5 s (codec_thread) [estimate] */
static void g_track(int k, struct sample *s)
{
    if (k % 24000 >= 1000 && k % 24000 < 1050)
        s->boost = true;
}

static void g_dark_flac(int k, struct sample *s)
{
    g_background(k, s); g_refill(k, s, 120); g_track(k, s);
    s->playing = true;
}

static void g_dark_heavy(int k, struct sample *s)
{
    g_dark_flac(k, s);
    s->load_floor = true;           /* not a light format, or not 1x */
}

static void g_dark_paused(int k, struct sample *s)
{
    g_background(k, s);
}

static void g_lit_np(int k, struct sample *s)
{
    g_background(k, s); g_refill(k, s, 120); g_track(k, s);
    s->playing = true;
    s->lit = true;
    if (k % 100 == 0)
    {
        /* the 1 Hz progress redraw, 4 ms at 80 [estimate]; the marquee is
         * a BCM2722 pan (19-5), no PP work */
        s->nframes = 1;
        s->frame_us24 = 13300.0;
        s->cpu_us24 += 1000.0;      /* the E2 push's setup */
    }
}

static void g_lit_browse(int k, struct sample *s)
{
    int t = k % 400;                /* a 4 s cycle */

    g_background(k, s); g_refill(k, s, 120);
    s->playing = true;
    s->lit = true;
    /* a wheel burst, 8 steps a second for 1 s, 6 ms at 80 a frame
     * [estimate from f3: 304 pushes, 2,496 ms in 60 s lit, session 38] */
    if (t < 100 && t % 12 == 0 && frnd() < 0.9)
    {
        s->nframes = 1;
        s->frame_us24 = 20000.0;
        s->cpu_us24 += 1000.0;
    }
    /* a menu slide at 2 s: boosted 330 ms, a frame every 33 ms of 10 ms
     * at 80 (anim.c boosts) */
    if (t >= 200 && t < 233)
    {
        s->boost = true;
        if ((t - 200) % 3 == 0)
        {
            s->nframes = 1;
            s->frame_us24 = 33000.0;
            s->cpu_us24 += 1000.0;
        }
    }
    /* a cached art read every 8 s: one transfer, 15 ms at 24 */
    if (k % 800 == 300)
    {
        s->ata = true;
        s->cpu_us24 += 15000.0;
    }
}

static void g_scroll_fast(int k, struct sample *s)
{
    g_background(k, s);
    s->playing = true;
    s->lit = true;
    s->boost = true;                /* the list's glide boosts while it moves */
    if (k % 5 == 0)                 /* 20 steps a second, 8 ms at 80 */
    {
        s->nframes = 1;
        s->frame_us24 = 26700.0;
        s->cpu_us24 += 1000.0;
    }
}

static void g_library(int k, struct sample *s)
{
    g_background(k, s);
    if (k % 3 == 0)
        s->ata = true;              /* about 30 transfers a second (18-5) */
}

static void g_video(int k, struct sample *s)
{
    g_background(k, s); g_refill(k, s, 120);
    s->playing = true;
    s->lit = true;
    s->load_floor = true;
    if (k % 10 < 3)                 /* the FEED: 3 ms at 80 every 33 ms */
    {
        s->cpu_us24 += 3300.0;
        s->cap80 = true;
    }
}

static const struct scen scens[] = {
    /* name, s, weight, decode share at 24, frame ms, backlog, gen, source */
    { "dark_flac",   300, 40, 0.55,  93, 0, g_dark_flac,
      "FLAC 55% at 24 [silicon s36]" },
    { "dark_aac",    300, 10, 1.125, 23, 0, g_dark_heavy,
      "AAC ~27 MHz [estimate]" },
    { "dark_vorbis", 300,  5, 1.19,  46, 0, g_dark_heavy,
      "Vorbis ~28.6 MHz [estimate]" },
    { "dark_mp3x15", 300,  5, 1.06,  26, 0, g_dark_heavy,
      "MP3 1.5x + stretch ~25.5 MHz [estimate]" },
    { "dark_paused", 120, 10, 0,      0, 0, g_dark_paused, "idle" },
    { "lit_np",      120, 10, 0.50,  26, 0, g_lit_np,
      "MP3 lit, 1 Hz bar" },
    { "lit_browse",  120, 12, 0.55,  93, 0, g_lit_browse,
      "f3 pushes [silicon s38]" },
    { "scroll_fast",  30,  3, 0.55,  93, 0, g_scroll_fast, "boosted spin" },
    { "library",      60,  3, 0,      0, 40, g_library,
      "40 s at 80 of scan, boosted" },
    { "video",        60,  2, 1.125, 23, 0, g_video, "AAC + FEED" },
};
#define NSCEN ((int)(sizeof(scens)/sizeof(scens[0])))

/* ---- Arms --------------------------------------------------------------- */

enum { ARM_R, ARM_T1, ARM_P1, ARM_T, ARM_P, ARM_T3, ARM_P3, NARMS };
static const char *arm_name[NARMS] =
    { "R", "T-1", "P-1", "T", "P", "T+3", "P+3" };
static unsigned int arm_live(int a)
{
    switch (a)
    {
    case ARM_T: case ARM_P:   return PGOV_LIVE_B1;
    case ARM_T3: case ARM_P3: return PGOV_LIVE_B1 | PGOV_LIVE_B3;
    default:                  return 0;
    }
}
static int arm_inband(int a)
{
    return (a == ARM_P1 || a == ARM_P || a == ARM_P3)
        ? PGOV_INBAND_PROP : PGOV_INBAND_TOP;
}

/* ---- One run ------------------------------------------------------------ */

struct result {
    /* energy accumulators, per voltage code (0: 0xEC, 1: 0xEB) */
    double core[2], clk[2], clk_idle[2], tv[2];
    double t_pll, starts;
    /* misses and transitions */
    int und, resc, uimiss, unfinished;
    int starts_out;                 /* PLL starts outside ATA bursts */
    int changes, vchanges;
    double band_us[4], secs;
    double boost_us;
};

static double tf(double f, double m)    /* time per unit of 80 MHz work */
{
    return (1.0 - m) * 80.0 / f + m;
}

static void run(const struct scen *sc, int arm, const struct pgov_params *pp,
                double m, struct result *r)
{
    struct pgov_core c;
    struct sample s;
    double fr_rem[MAXF], fr_dl[MAXF];
    int fh = 0, fn = 0;
    double level = PCM_CAP_US, dec_rem = 0, misc = 0;
    double scale24 = 1.0 / tf(24, m);   /* 24 MHz us -> 80 MHz work */
    bool dec_on = false, below_wm = false, resc_boost = false;
    bool prev_boost = false, prev_ata = false, under = false;
    unsigned int mhz = 24, vcode = PGOV_V_NORM, prev_floor = 0;
    int n = sc->secs * 100;

    memset(r, 0, sizeof(*r));
    rng = 0x9e3779b9u ^ (uint32_t)(sc - scens);
    misc = sc->backlog_s80 * 1e6;
    if (arm != ARM_R)
    {
        pgov_core_init(&c, pp, arm_live(arm), 24);
        pgov_core_arm(&c, true);
    }
    for (int k = 0; k < n; k++)
    {
        unsigned int want_floor, new_mhz;
        bool boost;
        double budget = DT_US, busy_cpu, busy_cop, t, f;

        memset(&s, 0, sizeof(s));
        sc->gen(k, &s);
        if (sc->backlog_s80 > 0 && misc > 1.0)
            s.boost = true;         /* maint.c boosts while it works */
        boost = s.boost || resc_boost;
        want_floor = (s.load_floor && s.playing) || s.ata ? 30 : 0;

        /* the clock for this sample */
        if (arm == ARM_R)
            new_mhz = boost ? 80 : (want_floor || s.lit) ? 30 : 24;
        else
        {
            if (boost && !prev_boost)
                pgov_core_boost(&c);
            if (want_floor > prev_floor)
                pgov_core_floor(&c, want_floor);
            new_mhz = c.mhz;
            if (c.vcode != vcode)
                r->vchanges++;
            vcode = c.vcode;
        }
        prev_boost = boost;
        prev_floor = want_floor;
        f = new_mhz;
        if (new_mhz != mhz)
        {
            r->changes++;
            if (new_mhz > 24)
            {
                budget -= LOCK_US;  /* the lock, polled at 24 MHz */
                if (mhz == 24)
                {
                    r->starts++;
                    if (!s.ata && !prev_ata)
                        r->starts_out++;
                }
            }
            mhz = new_mhz;
        }
        prev_ata = s.ata;

        /* work: the CPU decodes first, then the rest */
        misc += s.cpu_us24 * scale24;
        for (int i = 0; i < s.nframes && fn < MAXF; i++)
        {
            int j = (fh + fn++) % MAXF;
            fr_rem[j] = s.frame_us24 * scale24;
            fr_dl[j] = UI_SLOT_US;  /* from the sample's start */
        }
        t = budget;
        while (t > 0)
        {
            if (!dec_on && s.playing && sc->dec_share24 > 0
                && PCM_CAP_US - level >= sc->frame_ms * 1000)
            {
                dec_on = true;
                dec_rem = sc->frame_ms * 1000 * sc->dec_share24 * scale24;
            }
            if (dec_on)
            {
                double need = dec_rem * tf(f, m);
                if (need <= t)
                {
                    t -= need;
                    dec_on = false;
                    level += sc->frame_ms * 1000;
                    continue;
                }
                dec_rem -= t / tf(f, m);
                t = 0;
            }
            else if (misc > 0)
            {
                double need = misc * tf(f, m);
                if (need <= t) { t -= need; misc = 0; }
                else { misc -= t / tf(f, m); t = 0; }
            }
            else
                break;
        }
        busy_cpu = DT_US - t;
        t = budget;                 /* the COP: the UI's frames, in order */
        while (t > 0 && fn > 0)
        {
            double need = fr_rem[fh] * tf(f, m);
            if (need <= t)
            {
                t -= need;
                if (DT_US - t > fr_dl[fh])
                    r->uimiss++;
                fh = (fh + 1) % MAXF; fn--;
            }
            else
            {
                fr_rem[fh] -= t / tf(f, m);
                t = 0;
            }
        }
        for (int i = 0; i < fn; i++)
            fr_dl[(fh + i) % MAXF] -= DT_US;
        busy_cop = DT_US - t;

        /* the PCM drains */
        if (s.playing)
        {
            level -= DT_US;
            if (level <= 0)
            {
                if (!under)
                    r->und++;
                under = true;
                level = 0;
            }
            else
                under = false;
            if (level < PCM_WM_US)
            {
                if (!below_wm)
                    r->resc++;
                below_wm = resc_boost = true;
            }
            else
                below_wm = false;
            if (level >= PCM_CAP_US - sc->frame_ms * 1000)
                resc_boost = false;
        }

        /* the energy proxy's accumulators */
        {
            int v = vcode == PGOV_V_LOW;
            double idle = DT_US - (busy_cpu > busy_cop ? busy_cpu : busy_cop);
            r->core[v] += (busy_cpu + busy_cop) * f;
            r->clk[v] += DT_US * f;
            r->clk_idle[v] += idle * f;
            r->tv[v] += DT_US;
            if (f > 24)
                r->t_pll += DT_US;
            r->band_us[pgov_band_of(arm_live(arm), mhz)] += DT_US;
            if (boost)
                r->boost_us += DT_US;
        }

        /* the tick: the governor's sample for the next one */
        if (arm != ARM_R)
        {
            struct pgov_in in;
            in.busy_pm[0] = (uint16_t)(busy_cpu * 1000 / DT_US);
            in.busy_pm[1] = (uint16_t)(busy_cop * 1000 / DT_US);
            in.cur_mhz = (uint8_t)mhz;
            in.floor_mhz = (uint8_t)want_floor;
            in.cap_mhz = s.cap80 || s.ata ? 80 : 0;
            in.boost = boost;
            in.vwrite = 0;
            in.dt_ms = 10;
            pgov_core_step(&c, &in);
            if (!pgov_core_valid(&c, c.mhz, c.vcode))
            {
                fprintf(stderr, "invariant: %u MHz at 0x%02x\n",
                        c.mhz, c.vcode);
                exit(2);
            }
        }
    }
    r->unfinished = misc > 1000.0 || fn > 0;
    r->secs = sc->secs;
}

/* ---- The energy proxy (section 5.1), all [estimate] --------------------- */

struct model {
    double g;                       /* the share of clock power sleep gates */
    double k_clk;                   /* clock tree + bus + SDRAM ctl at 80 MHz, mW */
    double p_pll;                   /* PLL, mW while powered */
    double e_start;                 /* a PLL start, uJ */
    int vform;                      /* 0: A (0.6 + 0.05 x), 1: B (0.9 + 0.025 x) */
};
#define K_CORE80    40.0            /* a fully busy core at 80 MHz, 1.2 V, mW */
#define K_LEAK      2.0             /* leakage at 1.2 V, mW */

static double volts(int low, int form)
{
    int code = low ? 0x0b : 0x0c;
    return form ? 0.9 + 0.025 * code : 0.6 + 0.05 * code;
}

/* mJ over the run */
static double energy(const struct result *r, const struct model *md)
{
    double e = 0;               /* nJ = mW x us */
    for (int v = 0; v < 2; v++)
    {
        double V = volts(v, md->vform), v2 = V * V / 1.44;
        e += v2 * (K_CORE80 / 80.0) * r->core[v];
        e += v2 * (md->k_clk / 80.0)
             * (r->clk[v] - md->g * r->clk_idle[v]);
        e += K_LEAK * V / 1.2 * r->tv[v];
    }
    e += md->p_pll * r->t_pll + md->e_start * 1000.0 * r->starts;
    return e / 1e6;
}

static const struct model mid = { 0.4, 12.0, 1.5, 10.0, 0 };

/* ---- Reports ------------------------------------------------------------ */

static void table(const struct pgov_params *pp, double m)
{
    printf("# m=%.1f g=%.1f k_clk=%.0f p_pll=%.1f e_start=%.0f V=A\n",
           m, mid.g, mid.k_clk, mid.p_pll, mid.e_start);
    printf("%-12s %-4s %8s %6s %4s %4s %4s %3s %5s %5s %5s %5s %5s %5s\n",
           "scenario", "arm", "mJ", "vsR", "und", "resc", "ui", "unf",
           "st/mn", "chg/s", "b0%", "b1%", "b2%", "b3%");
    for (int i = 0; i < NSCEN; i++)
    {
        double er = 0;
        for (int a = 0; a < NARMS; a++)
        {
            struct result r;
            struct pgov_params p = *pp;
            double e;
            p.inband = arm_inband(a);
            run(&scens[i], a, &p, m, &r);
            e = energy(&r, &mid);
            if (a == ARM_R)
                er = e;
            printf("%-12s %-4s %8.1f %6.3f %4d %4d %4d %3d %5.1f %5.2f"
                   " %5.1f %5.1f %5.1f %5.1f\n",
                   scens[i].name, arm_name[a], e, er > 0 ? e / er : 0,
                   r.und, r.resc, r.uimiss, r.unfinished,
                   r.starts_out * 60.0 / r.secs, r.changes / r.secs,
                   r.band_us[0] / r.secs / 1e4, r.band_us[1] / r.secs / 1e4,
                   r.band_us[2] / r.secs / 1e4, r.band_us[3] / r.secs / 1e4);
        }
    }
}

/* The grid: per arm, the policy configurations with no more misses than
 * R in any scenario and <= 20 PLL starts a minute outside ATA; the best
 * by the day mix's energy at the mid model; then T against P at every
 * point of the model grid */
#define NM 3
static const double ms[NM] = { 0.0, 0.2, 0.4 };

struct cfg { int dwell, sat, hr, ew, dn; };
static const int dwells[] = { 20, 50, 100, 200, 500, 1000 };
static const int sats[] = { 2, 4, 8, 16 };
static const int hrs[] = { 70, 80, 90, 95 };
static const int ews[] = { 5, 4, 3, 2 };
static const int dns[] = { 0, 100, 110 };
#define NCFG (6 * 4 * 4 * 4 * 3)

static void cfg_params(int ci, struct pgov_params *p, struct cfg *cf)
{
    cf->dwell = dwells[ci % 6]; ci /= 6;
    cf->sat = sats[ci % 4]; ci /= 4;
    cf->hr = hrs[ci % 4]; ci /= 4;
    cf->ew = ews[ci % 4]; ci /= 4;
    cf->dn = dns[ci % 3];
    pgov_params_default(p);
    /* the dwell scales the brief's ratios: b2 = d, b1 = 2.5 d, b3 = d / 2 */
    p->dwell_ms[PGOV_B2] = cf->dwell;
    p->dwell_ms[PGOV_B1] = cf->dwell * 5 / 2;
    p->dwell_ms[PGOV_B3] = cf->dwell / 2;
    p->sat_samples = cf->sat;
    p->headroom_pct = cf->hr;
    p->ewma_shift = cf->ew;
    p->down_pct = cf->dn;
}

static struct result rs[NARMS][NM][NSCEN];
static struct result rr[NM][NSCEN];     /* R */

static double mix(struct result res[NSCEN], const struct model *md)
{
    double e = 0;
    for (int i = 0; i < NSCEN; i++)
        e += scens[i].weight * energy(&res[i], md) / scens[i].secs;
    return e / 100.0;               /* mW, the day mix */
}

static bool feasible(struct result res[NM][NSCEN])
{
    for (int mi = 0; mi < NM; mi++)
        for (int i = 0; i < NSCEN; i++)
        {
            struct result *r = &res[mi][i], *b = &rr[mi][i];
            if (r->und > b->und || r->uimiss > b->uimiss
                || r->unfinished > b->unfinished
                || r->starts_out * 60.0 / r->secs > 20.0)
                return false;
        }
    return true;
}

static void grid(void)
{
    static struct result cur[NM][NSCEN];
    int best[NARMS];
    double beste[NARMS];

    for (int mi = 0; mi < NM; mi++)
        for (int i = 0; i < NSCEN; i++)
        {
            struct pgov_params p;
            pgov_params_default(&p);
            run(&scens[i], ARM_R, &p, ms[mi], &rr[mi][i]);
        }
    printf("# grid: %d policy configurations x %d m x %d scenarios\n",
           NCFG, NM, NSCEN);
    /* per arm: every feasible configuration's day-mix energy and its
     * changes a minute; the pick is the fewest changes within 0.5 % of
     * the lowest energy (the optimum sits at the shortest dwell, where
     * only the transition count separates the configurations) */
    for (int a = 1; a < NARMS; a++)
    {
        static double ecfg[NCFG], chcfg[NCFG];
        int nfeas = 0, lo = -1;
        best[a] = -1;
        beste[a] = 1e30;
        for (int ci = 0; ci < NCFG; ci++)
        {
            struct pgov_params p;
            struct cfg cf;
            double e = 0, ch = 0;
            ecfg[ci] = -1;
            cfg_params(ci, &p, &cf);
            p.inband = arm_inband(a);
            for (int mi = 0; mi < NM; mi++)
                for (int i = 0; i < NSCEN; i++)
                    run(&scens[i], a, &p, ms[mi], &cur[mi][i]);
            if (!feasible(cur))
                continue;
            nfeas++;
            for (int mi = 0; mi < NM; mi++)
            {
                e += mix(cur[mi], &mid);
                for (int i = 0; i < NSCEN; i++)
                    ch += scens[i].weight * cur[mi][i].changes * 60.0
                          / cur[mi][i].secs / 100.0;
            }
            ecfg[ci] = e;
            chcfg[ci] = ch / NM;
            if (lo < 0 || e < ecfg[lo])
                lo = ci;
        }
        for (int ci = 0; lo >= 0 && ci < NCFG; ci++)
            if (ecfg[ci] >= 0 && ecfg[ci] <= ecfg[lo] * 1.005
                && (best[a] < 0 || chcfg[ci] < chcfg[best[a]]))
                best[a] = ci;
        if (best[a] >= 0)
        {
            struct pgov_params p;
            struct cfg cf;
            double er = 0;
            cfg_params(best[a], &p, &cf);
            p.inband = arm_inband(a);
            beste[a] = ecfg[best[a]];
            for (int mi = 0; mi < NM; mi++)
                for (int i = 0; i < NSCEN; i++)
                    run(&scens[i], a, &p, ms[mi], &rs[a][mi][i]);
            for (int mi = 0; mi < NM; mi++)
                er += mix(rr[mi], &mid);
            printf("arm %-4s feasible %4d/%d pick: dwell(b2)=%d ms sat=%d"
                   " H=%d dn=%d ewma=1/%d  %.2f mW (R %.2f, %.3f;"
                   " lowest %.3f) %.0f chg/min\n",
                   arm_name[a], nfeas, NCFG, cf.dwell, cf.sat, cf.hr, cf.dn,
                   1 << cf.ew, beste[a] / NM, er / NM, beste[a] / er,
                   ecfg[lo] / er, chcfg[best[a]]);
        }
        else
            printf("arm %-4s feasible 0/%d\n", arm_name[a], NCFG);
    }

    /* T against P, each at its best, over the model grid */
    {
        static const double gs[] = { 0, 0.4, 0.8 };
        static const double ks[] = { 6, 12, 24 };
        static const double pps[] = { 0.5, 1.5, 3 };
        static const double es[] = { 0, 10, 50 };
        const int pairs[3][2] = { { ARM_T1, ARM_P1 }, { ARM_T, ARM_P },
                                  { ARM_T3, ARM_P3 } };
        for (int pi = 0; pi < 3; pi++)
        {
            int ta = pairs[pi][0], pa = pairs[pi][1];
            int pwins = 0, total = 0, tvr = 0;
            double worst_t = 0, best_t = 1e9;
            if (best[ta] < 0 || best[pa] < 0)
                continue;
            for (int gi = 0; gi < 3; gi++)
             for (int ki = 0; ki < 3; ki++)
              for (int ppi = 0; ppi < 3; ppi++)
               for (int ei = 0; ei < 3; ei++)
                for (int vf = 0; vf < 2; vf++)
                 for (int mi = 0; mi < NM; mi++)
                 {
                     struct model md = { gs[gi], ks[ki], pps[ppi], es[ei], vf };
                     double et = mix(rs[ta][mi], &md);
                     double ep = mix(rs[pa][mi], &md);
                     double er = mix(rr[mi], &md);
                     total++;
                     if (ep < et)
                         pwins++;
                     if (et < er)
                         tvr++;
                     if (et / er > worst_t) worst_t = et / er;
                     if (et / er < best_t) best_t = et / er;
                 }
            printf("%s vs %s: P lower at %d/%d model points; %s below R at"
                   " %d/%d (ratio %.3f-%.3f)\n",
                   arm_name[ta], arm_name[pa], pwins, total, arm_name[ta],
                   tvr, total, best_t, worst_t);
        }
    }
}

int main(int argc, char **argv)
{
    struct pgov_params p;

    pgov_params_default(&p);
    if (argc > 1 && !strcmp(argv[1], "--grid"))
    {
        grid();
        return 0;
    }
    if (argc > 2 && !strcmp(argv[1], "--b2mask"))
        p.b2_mask = (uint8_t)strtol(argv[2], NULL, 0);
    table(&p, 0.2);
    return 0;
}
