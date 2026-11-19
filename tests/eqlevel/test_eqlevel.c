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
 * Host test: the EQ level plan (#203) and the custom slot format (#82)
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

/* The device code (apps/ipodui/eqlab_core.c, integer only) against an
 * independent double-precision reference: the RBJ cookbook biquads of
 * tools/eqfit (its assumption 3: shelves Q 0.707, peaks Q 1.41 / 0.67,
 * designed at 48 kHz), K-weighting from the BS.1770 biquads themselves
 * (not the device's table) on a 1/48-octave grid of pink noise.
 *
 *  1. model:   the device's curve vs the reference, every preset, every
 *              point 20 Hz..20 kHz: max |error| <= MODEL_TOL_DB.
 *  2. precut:  the device precut >= the reference peak - 0.5 dB (a sine
 *              at the peak clips by at most 0.5 dB) and <= peak + 1.5.
 *  3. loudness: with the device's precut and make-up, the K-weighted
 *              pink-noise level of every boost preset (make-up not at a
 *              clamp) is within LOUD_TOL_DB of EQ off. LEVEL_RED=1 builds
 *              the pre-#203 plan (no make-up) and must fail here.
 *  4. format:  every preset and a set of edge curves round-trips through
 *              eqlab_format()/eqlab_parse(), fits EQLAB_TEXT_LEN, and the
 *              parser refuses malformed lines without touching its output.
 *
 * Prints one line per preset (precut, loudness change, make-up, net). */
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "ipodui/eqlab_core.h"
#include "ipodui/eq_hw.h"

#ifndef LEVEL_RED
#define LEVEL_RED 0
#endif
#ifndef MODEL_TOL_DB
#define MODEL_TOL_DB    1.0
#endif
#define LOUD_TOL_DB     1.0

static int fails, checks;

#define CHECK(cond, ...) do { checks++; if (!(cond)) { fails++; \
    printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

enum { LS, PK, HS };

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
    double c1 = cos(w), s1 = sin(w), c2 = cos(2 * w), s2 = sin(2 * w);
    double nr = b0 + b1 * c1 + b2 * c2, ni = -(b1 * s1 + b2 * s2);
    double dr = a0 + a1 * c1 + a2 * c2, di = -(a1 * s1 + a2 * s2);
    return 10.0 * log10((nr * nr + ni * ni) / (dr * dr + di * di));
}

/* the reference curve at f (nominal corners, 48 kHz) */
static double ref_db(const struct dac_eq_band *b, double f)
{
    double sum = 0;
    for (int i = 0; i < 5; i++)
    {
        int t = i == 0 ? LS : i == 4 ? HS : PK;
        double q = t != PK ? 0.7071 : b[i].wide ? 0.67 : 1.41;
        sum += biquad_db(t, 2 * M_PI * eqlab_hz[i][b[i].freq & 3] / 48000.0,
                         b[i].gain, q, 2 * M_PI * f / 48000.0);
    }
    return sum;
}

/* |H|^2 of a biquad at f, 48 kHz */
static double bq_pow(const double *bb, const double *aa, double f)
{
    double w = 2 * M_PI * f / 48000.0;
    double nr = bb[0] + bb[1] * cos(w) + bb[2] * cos(2 * w);
    double ni = -(bb[1] * sin(w) + bb[2] * sin(2 * w));
    double dr = aa[0] + aa[1] * cos(w) + aa[2] * cos(2 * w);
    double di = -(aa[1] * sin(w) + aa[2] * sin(2 * w));
    return (nr * nr + ni * ni) / (dr * dr + di * di);
}

static double kweight(double f)
{
    static const double b1[3] = { 1.53512485958697, -2.69169618940638,
                                  1.19839281085285 };
    static const double a1[3] = { 1, -1.69065929318241, 0.73248077421585 };
    static const double b2[3] = { 1, -2, 1 };
    static const double a2[3] = { 1, -1.99004745483398, 0.99007225036621 };
    return bq_pow(b1, a1, f) * bq_pow(b2, a2, f);
}

/* K-weighted pink-noise loudness change of the reference curve, dB */
static double ref_loud(const struct dac_eq_band *b)
{
    double num = 0, den = 0;
    for (int k = 0; k <= 480; k++)
    {
        double f = 20.0 * pow(2.0, k / 48.0), w = kweight(f);
        num += w * pow(10.0, ref_db(b, f) / 10.0);
        den += w;
    }
    return 10.0 * log10(num / den);
}

static double ref_peak(const struct dac_eq_band *b)
{
    double top = 0;
    for (int k = 0; k <= 480; k++)
    {
        double d = ref_db(b, 20.0 * pow(2.0, k / 48.0));
        if (d > top)
            top = d;
    }
    return top;
}

static void preset_bands(int p, struct dac_eq_band *b)
{
    for (int i = 0; i < 5; i++)
    {
        b[i].gain = eq_hw_presets[p].band[i].gain;
        b[i].freq = eq_hw_presets[p].band[i].freq;
        b[i].wide = eq_hw_presets[p].band[i].wide;
    }
}

static bool loud_cover(const struct dac_eq_band *b,
                       const struct eqlab_level *lv)
{
    return ref_loud(b) > 0.5 && lv->makeup_db10 > 0
           && lv->makeup_db10 < 10 * lv->precut_db;
}

static void check_model(const struct dac_eq_band *b, const char *what)
{
    double worst = 0, at = 0;
    for (int lf = EQLAB_LF_MIN; lf <= EQLAB_LF_MAX; lf += 8)
    {
        double f = pow(2.0, lf / 256.0);
        double e = fabs(eqlab_db_q8(b, lf, 0) / 256.0 - ref_db(b, f));
        if (e > worst)
        {
            worst = e;
            at = f;
        }
    }
    CHECK(worst <= MODEL_TOL_DB, "%s: model off by %.2f dB at %.0f Hz",
          what, worst, at);
}

static void check_format(const struct dac_eq_band *b, const char *what)
{
    char line[EQLAB_TEXT_LEN];
    struct dac_eq_band back[5];
    int n = eqlab_format(b, line, sizeof(line));

    CHECK(n > 0 && n < EQLAB_TEXT_LEN && (int)strlen(line) == n,
          "%s: format length %d", what, n);
    memset(back, 0x55, sizeof(back));
    CHECK(eqlab_parse(line, back), "%s: '%s' does not parse", what, line);
    for (int i = 0; i < 5; i++)
        CHECK(back[i].gain == b[i].gain && back[i].freq == b[i].freq &&
              back[i].wide == (eqlab_has_width(i) && b[i].wide),
              "%s: band %d differs after '%s'", what, i + 1, line);
}

int main(void)
{
    struct dac_eq_band b[5];
    double worst_net = 0;
    int boosts = 0;

    printf("preset         precut  loud(dev/ref)  makeup   net\n");
    for (int p = 0; p < 22; p++)
    {
        struct eqlab_level lv;
        char what[16];
        double peak, loud, net;

        snprintf(what, sizeof(what), "preset %d", p + 1);
        preset_bands(p, b);
        check_model(b, what);
        check_format(b, what);
        eqlab_level(b, &lv);
        /* which presets the loudness check covers is decided by the curve
         * (a boost on balance, make-up off its clamps), not by the arm */
        bool covered = loud_cover(b, &lv);
#if LEVEL_RED
        lv.makeup_db10 = 0;     /* the pre-#203 plan: precut only */
#endif
        peak = ref_peak(b);
        loud = ref_loud(b);
        net = loud - lv.precut_db + lv.makeup_db10 / 10.0;
        printf("%-12s %6d %7.1f/%5.1f %8.1f %6.2f\n", what, lv.precut_db,
               lv.loud_db10 / 10.0, loud, lv.makeup_db10 / 10.0, net);
        CHECK(lv.precut_db >= peak - 0.5 && lv.precut_db <= peak + 1.5,
              "%s: precut %d vs peak %.2f", what, lv.precut_db, peak);
        CHECK(fabs(lv.loud_db10 / 10.0 - loud) <= 0.5,
              "%s: loudness %.1f vs reference %.2f", what,
              lv.loud_db10 / 10.0, loud);
        /* boost curves whose make-up is not at a clamp: loudness kept */
        if (covered)
        {
            boosts++;
            if (fabs(net) > worst_net)
                worst_net = fabs(net);
            CHECK(fabs(net) <= LOUD_TOL_DB,
                  "%s: %.2f dB from EQ off's loudness", what, net);
        }
        /* never louder than EQ off by more than the tolerance */
        CHECK(net <= LOUD_TOL_DB, "%s: %.2f dB louder than EQ off", what, net);
    }
    /* the loudness check must have looked at the boost presets: Acoustic,
     * Bass Booster, Dance, ... (at least 12 of the 22 boost on balance) */
    CHECK(boosts >= 12, "only %d boost presets checked", boosts);
    printf("loudness checked on %d boost presets, worst |net| %.2f dB\n",
           boosts, worst_net);

    /* edge curves: every band at +12 / -12, each frequency and width */
    for (int f = 0; f < 4; f++)
        for (int w = 0; w < 2; w++)
            for (int g = -12; g <= 12; g += 24)
            {
                char what[32];
                for (int i = 0; i < 5; i++)
                {
                    b[i].gain = g;
                    b[i].freq = f;
                    b[i].wide = w && eqlab_has_width(i);
                }
                snprintf(what, sizeof(what), "all %+d f%d w%d", g, f, w);
                check_format(b, what);
                struct eqlab_level lv;
                eqlab_level(b, &lv);
                CHECK(lv.precut_db >= 0 && lv.precut_db <= 12 &&
                      lv.makeup_db10 >= 0 &&
                      lv.makeup_db10 <= 10 * lv.precut_db,
                      "%s: level out of range", what);
            }
    /* single bands at +12 dB: the model, each band, frequency and width */
    for (int i = 0; i < 5; i++)
        for (int f = 0; f < 4; f++)
            for (int w = 0; w < 2; w++)
            {
                char what[32];
                if (w && !eqlab_has_width(i))
                    continue;
                eqlab_flat(b);
                b[i].gain = 12;
                b[i].freq = f;
                b[i].wide = w;
                snprintf(what, sizeof(what), "band %d f%d w%d", i + 1, f, w);
                check_model(b, what);
            }

    /* the parser: flat, and refusals that leave the output alone */
    {
        static const char *bad[] = {
            "eq2 +0@105 +0@300 +0@850 +0@2400 +0@6900",
            "eq1 +0@105 +0@300 +0@850 +0@2400",
            "eq1 +0@105 +0@300 +0@850 +0@2400 +0@6900 ",
            "eq1 +13@105 +0@300 +0@850 +0@2400 +0@6900",
            "eq1 +0@104 +0@300 +0@850 +0@2400 +0@6900",
            "eq1 +0@105w +0@300 +0@850 +0@2400 +0@6900",
            "eq1 +0@105 +0@300 +0@850 +0@2400 +0@6900w",
            "eq1 0@105 +0@300 +0@850 +0@2400 +0@6900",
            "eq1 +0@105 +0@300 +0@850 +0@2400 +0@69000",
            "garbage",
        };
        struct dac_eq_band keep[5];

        CHECK(eqlab_parse("", b) && b[0].gain == 0 && b[0].freq == 1,
              "empty line is not flat");
        CHECK(eqlab_parse("eq1 -12@80 +12@500w -3@1100 +0@4100w +7@11700", b)
              && b[0].gain == -12 && b[1].wide && b[3].wide &&
              b[4].freq == 3 && b[2].freq == 2, "a valid line misparsed");
        for (unsigned k = 0; k < sizeof(bad) / sizeof(bad[0]); k++)
        {
            memcpy(keep, b, sizeof(keep));
            CHECK(!eqlab_parse(bad[k], b) && !memcmp(keep, b, sizeof(keep)),
                  "accepted or touched: '%s'", bad[k]);
        }
    }

    printf("%s: %d checks, %d failed\n", fails ? "FAIL" : "PASS", checks,
           fails);
    return fails ? 1 : 0;
}
