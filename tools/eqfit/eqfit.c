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
 * eqfit: fits the shell's 22 iTunes EQ presets to the WM8758's 5-band
 * hardware EQ (docs/phase15-brief.md sections 2.2, 3.1). Host C99, libm
 * only. The build never runs it: its output is committed.
 *
 *   cc -O2 -std=c99 -o build/eqfit/eqfit tools/eqfit/eqfit.c -lm
 *   build/eqfit/eqfit -w            (from the repo root: rewrites
 *                                    apps/ipodui/eq_hw_presets.c)
 *   build/eqfit/eqfit -o FILE       (writes the table to FILE instead)
 *   build/eqfit/eqfit --plot FILE   (also writes the curves as TSV for
 *                                    tools/eqfit/plot.py -> PNG)
 * Without -w/-o it only prints the error table.
 *
 * Input: the preset gains are read from apps/ipodui/screens/eq.c itself
 * (its EQP(...) rows, in presets[] order), so the table can't drift from
 * the presets; tests/eqfit/ re-runs the tool and compares.
 *
 * MODEL AND ASSUMPTIONS
 *  1. Target (the "iTunes curve") = Rockbox's software EQ as screens/eq.c
 *     configures it and lib/rbcodec/dsp/eq.c + dsp_filter.c build it:
 *     band 0 low shelf at 32 Hz, bands 1..8 peaks at 64 Hz .. 8 kHz, band 9
 *     high shelf at 16 kHz; RBJ cookbook biquads (filter_ls/pk/hs_coefs)
 *     designed at the output rate (44.1 or 48 kHz). Q is the settings'
 *     default (eq_defaults[] in apps/settings_list.c: 0.7 for the shelves,
 *     1.0 for the peaks; screens/eq.c never writes q). Modelled in double
 *     precision; the DSP's fixed-point rounding (s3.28 coefficients) is
 *     well under 0.01 dB in the audio band and is ignored.
 *  2. Hardware: WM8758 EQ1 low shelf, EQ2..4 peaks, EQ5 high shelf,
 *     -12..+12 dB in 1 dB steps, 4 corner/centre frequencies per band
 *     (the WM8978's table, the same EQ block: firmware/export/wm8978.h).
 *     Those are for a 48 kHz sample rate. At 44.1 kHz the codec runs with
 *     ADDCTRL_SR_48kHz (wm8758.c), i.e. the same digital filters clocked
 *     at 44.1 kHz, so every corner scales by 44.1/48 = 0.919. Modelled
 *     exactly that way: biquads designed at 48 kHz, evaluated at fs.
 *  3. Hardware filter shapes, NOT given numerically by the datasheet
 *     (it only plots them): shelves are RBJ shelves with S = 1
 *     (Q = 0.707, the corner frequency being the half-gain point);
 *     peaks are RBJ peaks with Q = HW_Q_NARROW (EQxBW = 0, about one
 *     octave) or HW_Q_WIDE (EQxBW = 1, about two octaves). If the device
 *     shows different shapes, change the three HW_Q_* values and re-run;
 *     nothing else depends on them.
 *  4. The EQ sits on the DAC path (EQ1_EQ3DMODE set); the precut is DAC
 *     digital attenuation applied before it (agent A confirms the order).
 *  5. Error = the largest |hardware - software| response in dB over
 *     20 Hz .. 20 kHz on a 1/12-octave grid (20 Hz * 2^(k/12), k = 0..119,
 *     plus 20 kHz), taken at both 44.1 and 48 kHz; maxerr is the worse of
 *     the two, rounded UP to 0.1 dB. Precuts are a level offset and are
 *     not part of the curve error.
 *  6. Precut = the hardware response's own peak boost (max over the grid
 *     and both rates), rounded up to a whole dB, clamped 0..12: the
 *     codec's EQ output can't exceed full scale. The software precut is
 *     the largest band gain (0.1 dB), so the two levels differ by < 1 dB
 *     plus any band-overlap boost.
 *  7. Search: every frequency/width combination (4 * 8 * 8 * 8 * 4 =
 *     8192) is fitted by integer coordinate descent on the gains (all 25
 *     gains per band, sweeps until stable) minimising the minimax error
 *     (tie: the squared-error sum, then search order). The best 32
 *     combinations are then searched exhaustively in a +-2 dB box around
 *     their gains (5^5). Deterministic; about a second on the Mac.
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
/* The ProkBox banner (docs/templates/prokbox-header.txt), at the top of
 * every file this generator writes. */
#define PROKBOX_BANNER \
    "/***************************************************************************\n" \
    " *              __________                   __    __________\n" \
    " *   Open       \\______   \\_______    ____  |  | __\\______   \\  ____  ___  ___\n" \
    " *   Source      |     ___/\\_  __ \\  /  _ \\ |  |/ / |    |  _/ /  _ \\ \\  \\/  /\n" \
    " *   Player      |    |     |  | \\/ (  <_> )|    <  |    |   \\(  <_> ) >    <\n" \
    " *   Firmware    |____|     |__|     \\____/ |__|_ \\ |______  / \\____/ /__/\\_ \\\n" \
    " *                                               \\/        \\/               \\/\n" \
    " *\n" \
    " * Custom firmware for the iPod Video, 5th generation (5G and 5.5G).\n" \
    " * Not affiliated with or endorsed by Apple Inc.\n" \
    " *\n" \
    " * Copyright (C) 2026 prokopto-dev\n" \
    " *\n" \
    " * This program is free software; you can redistribute it and/or\n" \
    " * modify it under the terms of the GNU General Public License\n" \
    " * as published by the Free Software Foundation; either version 2\n" \
    " * of the License, or (at your option) any later version.\n" \
    " *\n" \
    " * This software is distributed on an \"AS IS\" basis, WITHOUT WARRANTY OF ANY\n" \
    " * KIND, either express or implied.\n" \
    " *\n" \
    " ****************************************************************************/\n"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NPRESETS    22
#define SW_BANDS    10
#define HW_BANDS    5
#define NRATES      2
#define NPTS        121
#define NGAINS      25          /* -12 .. +12 dB */
#define TOPK        32
#define BOX         2           /* +-2 dB refinement */
#define SCREEN_STRIDE 1         /* screening grid stride (1 = full) */
#define EQ_HW_MAXERR 255        /* eq_hw.h: every preset on the codec (owner) */

#define HW_Q_SHELF  0.7071
#define HW_Q_NARROW 1.41        /* assumption 3 */
#define HW_Q_WIDE   0.67        /* assumption 3 */

static const char *src_path = "apps/ipodui/screens/eq.c";
static const char *out_default = "apps/ipodui/eq_hw_presets.c";

static const double rates[NRATES] = { 44100.0, 48000.0 };
static const double sw_hz[SW_BANDS] = { 32, 64, 125, 250, 500, 1000, 2000,
                                        4000, 8000, 16000 };
static const double hw_hz[HW_BANDS][4] = {
    {   80,  105,  135,   175 },
    {  230,  300,  385,   500 },
    {  650,  850, 1100,  1400 },
    { 1800, 2400, 3200,  4100 },
    { 5300, 6900, 9000, 11700 },
};

struct preset {
    char name[32];
    int gain[SW_BANDS];         /* 0.1 dB */
};
static struct preset presets[NPRESETS];
static int npresets;

static double pts[NPTS];

enum { LS, PK, HS };

/* |H(e^jw)| in dB of the RBJ biquad of type t at w0 with gain db, Q */
static double biquad_db(int t, double w0, double db, double q, double w)
{
    double A = pow(10.0, db / 40.0), c = cos(w0), s = sin(w0);
    double alpha = s / (2.0 * q), sa = 2.0 * sqrt(A) * alpha;
    double b0, b1, b2, a0, a1, a2;

    if (db == 0.0)
        return 0.0;
    switch (t)
    {
    case LS:
        b0 = A * ((A + 1) - (A - 1) * c + sa);
        b1 = 2 * A * ((A - 1) - (A + 1) * c);
        b2 = A * ((A + 1) - (A - 1) * c - sa);
        a0 = (A + 1) + (A - 1) * c + sa;
        a1 = -2 * ((A - 1) + (A + 1) * c);
        a2 = (A + 1) + (A - 1) * c - sa;
        break;
    case HS:
        b0 = A * ((A + 1) + (A - 1) * c + sa);
        b1 = -2 * A * ((A - 1) + (A + 1) * c);
        b2 = A * ((A + 1) + (A - 1) * c - sa);
        a0 = (A + 1) - (A - 1) * c + sa;
        a1 = 2 * ((A - 1) - (A + 1) * c);
        a2 = (A + 1) - (A - 1) * c - sa;
        break;
    default:
        b0 = 1 + alpha * A;
        b1 = -2 * c;
        b2 = 1 - alpha * A;
        a0 = 1 + alpha / A;
        a1 = -2 * c;
        a2 = 1 - alpha / A;
        break;
    }
    /* z^-1 = cos w - j sin w */
    double c1 = cos(w), s1 = sin(w), c2 = cos(2 * w), s2 = sin(2 * w);
    double nr = b0 + b1 * c1 + b2 * c2, ni = -(b1 * s1 + b2 * s2);
    double dr = a0 + a1 * c1 + a2 * c2, di = -(a1 * s1 + a2 * s2);
    return 10.0 * log10((nr * nr + ni * ni) / (dr * dr + di * di));
}

/* target[r][p]: the software EQ response for the current preset */
static double target[NRATES][NPTS];
/* resp[band][freq][wide][gain][r][p] */
static double resp[HW_BANDS][4][2][NGAINS][NRATES][NPTS];

static void build_hw_table(void)
{
    for (int b = 0; b < HW_BANDS; b++)
        for (int f = 0; f < 4; f++)
            for (int w = 0; w < 2; w++)
                for (int g = 0; g < NGAINS; g++)
                    for (int r = 0; r < NRATES; r++)
                        for (int p = 0; p < NPTS; p++)
                        {
                            int t = b == 0 ? LS : b == 4 ? HS : PK;
                            double q = t != PK ? HW_Q_SHELF :
                                       w ? HW_Q_WIDE : HW_Q_NARROW;
                            /* designed at 48 kHz, clocked at fs */
                            double w0 = 2 * M_PI * hw_hz[b][f] / 48000.0;
                            double wp = 2 * M_PI * pts[p] / rates[r];
                            resp[b][f][w][g][r][p] =
                                (t != PK && w) ? 0.0 :
                                biquad_db(t, w0, g - 12, q, wp);
                        }
}

static void build_target(const struct preset *ps)
{
    for (int r = 0; r < NRATES; r++)
        for (int p = 0; p < NPTS; p++)
        {
            double sum = 0, wp = 2 * M_PI * pts[p] / rates[r];
            for (int i = 0; i < SW_BANDS; i++)
            {
                int t = i == 0 ? LS : i == SW_BANDS - 1 ? HS : PK;
                double w0 = 2 * M_PI * sw_hz[i] / rates[r];
                sum += biquad_db(t, w0, ps->gain[i] / 10.0,
                                 t == PK ? 1.0 : 0.7, wp);
            }
            target[r][p] = sum;
        }
}

struct cfg {
    int f[HW_BANDS], w[HW_BANDS], g[HW_BANDS];  /* g: -12..12 */
    double err, sq;
    int order;
};

/* error of a full configuration; err per rate optional */
static void evaluate_s(struct cfg *c, double *err_r, int st)
{
    double err = 0, sq = 0;
    for (int r = 0; r < NRATES; r++)
    {
        double er = 0;
        for (int p = 0; p < NPTS; p += st)
        {
            double s = 0;
            for (int b = 0; b < HW_BANDS; b++)
                s += resp[b][c->f[b]][c->w[b]][c->g[b] + 12][r][p];
            double d = fabs(s - target[r][p]);
            if (d > er)
                er = d;
            sq += d * d;
        }
        if (err_r)
            err_r[r] = er;
        if (er > err)
            err = er;
    }
    c->err = err;
    c->sq = sq;
}

static void evaluate(struct cfg *c, double *err_r)
{
    evaluate_s(c, err_r, 1);
}

static int better(double e, double s, double be, double bs)
{
    if (e < be - 1e-9)
        return 1;
    if (e > be + 1e-9)
        return 0;
    return s < bs - 1e-9;
}

/* integer coordinate descent on the gains for fixed f/w */
static void descend(struct cfg *c, int st)
{
    static double sum[NRATES][NPTS], tot[NRATES][NPTS];
    int changed = 1, sweeps = 0, hr = 0, hp = 0;

    memset(c->g, 0, sizeof c->g);
    evaluate_s(c, NULL, st);
    /* tot = all bands - target; every band starts at 0 dB (0 response) */
    for (int r = 0; r < NRATES; r++)
        for (int p = 0; p < NPTS; p += st)
            tot[r][p] = -target[r][p];
    while (changed && sweeps++ < 20)
    {
        changed = 0;
        for (int b = 0; b < HW_BANDS; b++)
        {
            /* sum without band b */
            const double (*cur)[NPTS] = resp[b][c->f[b]][c->w[b]][c->g[b] + 12];
            for (int r = 0; r < NRATES; r++)
                for (int p = 0; p < NPTS; p += st)
                    sum[r][p] = tot[r][p] - cur[r][p];
            int bestg = c->g[b];
            double be = c->err, bs = c->sq;
            for (int g = 0; g < NGAINS; g++)
            {
                const double (*rb)[NPTS] = resp[b][c->f[b]][c->w[b]][g];
                double e = 0, sq = 0, lim = be + 1e-9;
                /* the last loser's worst point first: most gains fail there */
                if (fabs(sum[hr][hp] + rb[hr][hp]) > lim)
                    continue;
                for (int r = 0; r < NRATES && e <= lim; r++)
                    for (int p = 0; p < NPTS; p += st)
                    {
                        double d = fabs(sum[r][p] + rb[r][p]);
                        if (d > e)
                        {
                            e = d;
                            if (e > lim)
                            {
                                hr = r;
                                hp = p;
                                break;  /* can't win: skip this gain */
                            }
                        }
                        sq += d * d;
                    }
                if (e <= lim && better(e, sq, be, bs))
                {
                    be = e;
                    bs = sq;
                    bestg = g - 12;
                }
            }
            if (bestg != c->g[b])
            {
                c->g[b] = bestg;
                c->err = be;
                c->sq = bs;
                changed = 1;
            }
            const double (*nb)[NPTS] = resp[b][c->f[b]][c->w[b]][c->g[b] + 12];
            for (int r = 0; r < NRATES; r++)
                for (int p = 0; p < NPTS; p += st)
                    tot[r][p] = sum[r][p] + nb[r][p];
        }
    }
}

/* exhaustive +-BOX search around c's gains: depth-first over the bands
 * with running partial sums; a leaf aborts as soon as it is worse */
static double part[HW_BANDS + 1][NRATES][NPTS];

static void refine_rec(struct cfg *c, struct cfg *t, const struct cfg *base,
                       int b)
{
    if (b == HW_BANDS)
    {
        double e = 0, sq = 0;
        for (int r = 0; r < NRATES; r++)
            for (int p = 0; p < NPTS; p++)
            {
                double d = fabs(part[b][r][p]);
                if (d > c->err + 1e-9)
                    return;
                if (d > e)
                    e = d;
                sq += d * d;
            }
        if (better(e, sq, c->err, c->sq))
        {
            *c = *t;
            c->err = e;
            c->sq = sq;
        }
        return;
    }
    for (int o = -BOX; o <= BOX; o++)
    {
        int g = base->g[b] + o;
        if (g < -12 || g > 12)
            continue;
        t->g[b] = g;
        const double (*rb)[NPTS] = resp[b][t->f[b]][t->w[b]][g + 12];
        for (int r = 0; r < NRATES; r++)
            for (int p = 0; p < NPTS; p++)
                part[b + 1][r][p] = part[b][r][p] + rb[r][p];
        refine_rec(c, t, base, b + 1);
    }
}

static void refine(struct cfg *c)
{
    struct cfg base = *c, t = *c;

    for (int r = 0; r < NRATES; r++)
        for (int p = 0; p < NPTS; p++)
            part[0][r][p] = -target[r][p];
    refine_rec(c, &t, &base, 0);
}

static int cmp_cfg(const void *a, const void *b)
{
    const struct cfg *x = a, *y = b;
    if (better(x->err, x->sq, y->err, y->sq))
        return -1;
    if (better(y->err, y->sq, x->err, x->sq))
        return 1;
    return x->order - y->order;
}

static struct cfg all[8192];

static struct cfg fit(void)
{
    int n = 0;
    for (int f0 = 0; f0 < 4; f0++)
    for (int f1 = 0; f1 < 4; f1++)
    for (int w1 = 0; w1 < 2; w1++)
    for (int f2 = 0; f2 < 4; f2++)
    for (int w2 = 0; w2 < 2; w2++)
    for (int f3 = 0; f3 < 4; f3++)
    for (int w3 = 0; w3 < 2; w3++)
    for (int f4 = 0; f4 < 4; f4++)
    {
        struct cfg *c = &all[n];
        c->f[0] = f0; c->f[1] = f1; c->f[2] = f2; c->f[3] = f3; c->f[4] = f4;
        c->w[0] = 0;  c->w[1] = w1; c->w[2] = w2; c->w[3] = w3; c->w[4] = 0;
        c->order = n++;
        descend(c, SCREEN_STRIDE);
    }
    qsort(all, n, sizeof all[0], cmp_cfg);
    struct cfg best = all[0];
    for (int k = 0; k < TOPK && k < n; k++)
    {
        descend(&all[k], 1);
        refine(&all[k]);
        if (cmp_cfg(&all[k], &best) < 0)
            best = all[k];
    }
    /* a band at 0 dB: frequency and width don't matter; write 0 */
    for (int b = 0; b < HW_BANDS; b++)
        if (best.g[b] == 0)
            best.f[b] = best.w[b] = 0;
    return best;
}

static void read_presets(void)
{
    FILE *f = fopen(src_path, "r");
    char line[512];

    if (!f)
    {
        fprintf(stderr, "eqfit: can't open %s (run from the repo root)\n",
                src_path);
        exit(2);
    }
    while (fgets(line, sizeof line, f))
    {
        const char *s = line;
        while (*s == ' ')
            s++;
        if (strncmp(s, "EQP(", 4))
            continue;
        struct preset *p = &presets[npresets];
        int *g = p->gain;
        if (sscanf(s + 4, "%31[A-Z_], %d, %d, %d, %d, %d, %d, %d, %d, %d, %d",
                   p->name, &g[0], &g[1], &g[2], &g[3], &g[4], &g[5], &g[6],
                   &g[7], &g[8], &g[9]) != 11)
            continue;
        if (++npresets == NPRESETS)
            break;
    }
    fclose(f);
    if (npresets != NPRESETS)
    {
        fprintf(stderr, "eqfit: found %d presets in %s, need %d\n",
                npresets, src_path, NPRESETS);
        exit(2);
    }
}

int main(int argc, char **argv)
{
    const char *out = NULL, *plot = NULL;
    struct cfg res[NPRESETS];
    double er[NPRESETS][NRATES];
    int precut[NPRESETS], maxerr[NPRESETS];

    for (int i = 1; i < argc; i++)
    {
        if (!strcmp(argv[i], "-w"))
            out = out_default;
        else if (!strcmp(argv[i], "-o") && i + 1 < argc)
            out = argv[++i];
        else if (!strcmp(argv[i], "--plot") && i + 1 < argc)
            plot = argv[++i];
        else if (!strcmp(argv[i], "--src") && i + 1 < argc)
            src_path = argv[++i];
        else
        {
            fprintf(stderr, "usage: eqfit [-w | -o FILE] [--plot FILE] "
                            "[--src screens/eq.c]\n");
            return 2;
        }
    }

    for (int k = 0; k < NPTS - 1; k++)
        pts[k] = 20.0 * pow(2.0, k / 12.0);
    pts[NPTS - 1] = 20000.0;
    read_presets();
    build_hw_table();

    FILE *pf = NULL;
    if (plot && !(pf = fopen(plot, "w")))
    {
        perror(plot);
        return 2;
    }
    if (pf)
        fprintf(pf, "#preset\thz\tsw44\thw44\tsw48\thw48\n");

    printf("%-15s %6s %6s %6s %6s %5s %-3s  %s\n", "preset", "44.1k", "48k",
           "maxerr", "precut", "swpk", "eng", "bands gain@Hz(48k) n/w");
    int nhw = 0;
    for (int i = 0; i < NPRESETS; i++)
    {
        build_target(&presets[i]);
        res[i] = fit();
        evaluate(&res[i], er[i]);
        double peak = 0, swpeak = 0;
        for (int r = 0; r < NRATES; r++)
            for (int p = 0; p < NPTS; p++)
            {
                if (target[r][p] > swpeak)
                    swpeak = target[r][p];
                double s = 0;
                for (int b = 0; b < HW_BANDS; b++)
                    s += resp[b][res[i].f[b]][res[i].w[b]]
                             [res[i].g[b] + 12][r][p];
                if (s > peak)
                    peak = s;
            }
        precut[i] = (int)ceil(peak - 1e-6);
        if (precut[i] > 12)
            precut[i] = 12;
        maxerr[i] = (int)ceil(res[i].err * 10.0 - 1e-6);
        if (maxerr[i] > 255)
            maxerr[i] = 255;
        int hw = maxerr[i] <= EQ_HW_MAXERR;
        nhw += hw;
        printf("%-15s %6.2f %6.2f %6.1f %6d %5.1f %-3s ", presets[i].name,
               er[i][0], er[i][1], maxerr[i] / 10.0, precut[i], swpeak,
               hw ? "hw" : "sw");
        for (int b = 0; b < HW_BANDS; b++)
            printf(" %+d@%g%s", res[i].g[b], hw_hz[b][res[i].f[b]],
                   (b >= 1 && b <= 3) ? (res[i].w[b] ? "w" : "n") : "");
        printf("\n");

        if (pf)
            for (int p = 0; p < NPTS; p++)
            {
                double h[NRATES];
                for (int r = 0; r < NRATES; r++)
                {
                    h[r] = 0;
                    for (int b = 0; b < HW_BANDS; b++)
                        h[r] += resp[b][res[i].f[b]][res[i].w[b]]
                                    [res[i].g[b] + 12][r][p];
                }
                fprintf(pf, "%s\t%.1f\t%.3f\t%.3f\t%.3f\t%.3f\n",
                        presets[i].name, pts[p], target[0][p], h[0],
                        target[1][p], h[1]);
            }
    }
    if (pf)
        fclose(pf);
    printf("%d of %d presets within %.1f dB -> hardware\n", nhw, NPRESETS,
           EQ_HW_MAXERR / 10.0);

    if (!out)
        return 0;
    FILE *f = fopen(out, "w");
    if (!f)
    {
        perror(out);
        return 2;
    }
    fprintf(f, PROKBOX_BANNER "\n");
    fprintf(f,
"/* GENERATED by tools/eqfit/eqfit.c from apps/ipodui/screens/eq.c's\n"
" * presets[]: do not edit. Regenerate from the repo root with\n"
" *   cc -O2 -std=c99 -o build/eqfit/eqfit tools/eqfit/eqfit.c -lm\n"
" *   build/eqfit/eqfit -w\n"
" * tests/eqfit/run.sh fails when this file differs from a fresh run.\n"
" * Model and assumptions: the comment at the top of eqfit.c.\n"
" * Per row: bands { gain dB, freq 0..3, wide }, precut dB, maxerr 0.1 dB\n"
" * (worst of 44.1 and 48 kHz). */\n"
"#include \"ipodui/eq_hw.h\"\n\n"
"const struct eq_hw_preset eq_hw_presets[%d] = {\n", NPRESETS);
    for (int i = 0; i < NPRESETS; i++)
    {
        fprintf(f, "    /* %s: %.2f dB at 44.1 kHz, %.2f dB at 48 kHz */\n"
                   "    { {", presets[i].name, er[i][0], er[i][1]);
        for (int b = 0; b < HW_BANDS; b++)
            fprintf(f, " { %d, %d, %d }%s", res[i].g[b], res[i].f[b],
                    res[i].w[b], b < HW_BANDS - 1 ? "," : "");
        fprintf(f, " }, %d, %d },\n", precut[i], maxerr[i]);
    }
    fprintf(f, "};\n");
    fclose(f);
    return 0;
}
