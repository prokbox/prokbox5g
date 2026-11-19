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

/* #318: the statistics of flac_reduce_tpdf() (lib/rbcodec/codecs/
   libffmpegFLAC/dither.h), the reduction of 30..32-bit FLAC samples to the
   DSP chain's 29 bits.  Run by tests/flac32/run.sh.

   1. Exact, for every shift 1..8 and every value of the bits that go
      (f = 0..2^s-1): the error's mean and variance over all (r1, r2) are
      the same for every f.  That is what makes the dither TPDF and not
      RPDF (RPDF: the mean is the same, the variance is not) or none (the
      mean is not).  The mean is -1/2 of an input LSB; the variance at
      shift 3 (a 32-bit stream) is pinned: 63/4 input LSB^2, Delta^2 / 4
      less 1/4 with Delta = 8 (enumerated in Python, rational arithmetic).
   2. The generator as the decoder runs it, from the fixed seed (state 0):
      for each f, 2^18 samples of one value; the sample mean and variance
      within 1% of the enumeration's; consecutive errors uncorrelated
      (|lag-1 correlation| < 0.01).
   3. Extremes: INT32_MIN and INT32_MAX, a full block of each, land inside
      29 bits (the clamp), and the reduction is off by at most 2 output
      steps.
   4. The fixed seed: two runs of the same input give the same output, and
      the state after n samples is the LCG's, pinned as a literal.
   Prints one line a check; exits 1 on any failure. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "dither.h"

static int fails, checks;

static void check(int ok, const char *what, double a, double b)
{
    checks++;
    if (!ok) {
        fails++;
        printf("FAIL %s (%.6f vs %.6f)\n", what, a, b);
    }
}

/* the error in input LSBs of one reduction, from the formula */
static long err_of(int s, long f, long r1, long r2)
{
    long t = f + r1 + r2 - (1L << (s - 1));
    long q = t >= 0 ? t >> s : -((-t + (1L << s) - 1) >> s);   /* floor */
    return q * (1L << s) - f;
}

#define N (1 << 18)
static int32_t buf[N];

int main(void)
{
    int s;
    double var3 = 0;

    /* 1: exact enumeration */
    for (s = 1; s <= 8; s++) {
        long m = 1L << s, f;
        double mean0 = 0, var0 = 0;
        int same = 1;
        for (f = 0; f < m; f++) {
            double sum = 0, sq = 0;
            long r1, r2;
            for (r1 = 0; r1 < m; r1++)
                for (r2 = 0; r2 < m; r2++) {
                    long e = err_of(s, f, r1, r2);
                    sum += e;
                    sq += (double)e * e;
                }
            sum /= (double)(m * m);
            sq = sq / (double)(m * m) - sum * sum;
            if (f == 0) {
                mean0 = sum;
                var0 = sq;
            } else if (fabs(sum - mean0) > 1e-9 || fabs(sq - var0) > 1e-9) {
                same = 0;
            }
        }
        check(same, "mean and variance independent of the signal", mean0, var0);
        check(fabs(mean0 + 0.5) < 1e-9, "mean error -1/2 LSB", mean0, -0.5);
        if (s == 3)
            var3 = var0;
    }
    check(fabs(var3 - 15.75) < 1e-9, "shift 3 error variance 63/4 LSB^2",
          var3, 15.75);
    printf("     exact: TPDF statistics for shifts 1..8 (shift 3: mean -0.5, "
           "variance %.4f)\n", var3);

    /* 2: the generator as run, shift 3 (32 -> 29 bits) */
    {
        const int sh = 3;
        uint32_t st = 0;
        long f;
        for (f = 0; f < 8; f++) {
            const int32_t x = 0x01234560 + (int32_t)f;     /* low bits f */
            double sum = 0, sq = 0, lag = 0, prev = 0;
            int i;
            for (i = 0; i < N; i++)
                buf[i] = x;
            flac_reduce_tpdf(buf, N, sh, 29, &st);
            for (i = 0; i < N; i++) {
                double e = (double)buf[i] * 8 - x;
                sum += e;
                sq += e * e;
                if (i)
                    lag += (e - (-0.5)) * (prev - (-0.5));
                prev = e;
            }
            sum /= N;
            sq = sq / N - sum * sum;
            lag = lag / (N - 1) / sq;
            check(fabs(sum + 0.5) < 0.02, "sample mean", sum, -0.5);
            check(fabs(sq - var3) < 0.01 * var3, "sample variance", sq, var3);
            check(fabs(lag) < 0.01, "lag-1 correlation", lag, 0);
        }
        printf("     generator: 8 x %d samples at shift 3 match the "
               "enumeration (mean, variance, no lag-1 correlation)\n", N);
    }

    /* 3: extremes stay in 29 bits */
    {
        uint32_t st = 12345;
        int i, ok = 1;
        for (i = 0; i < 4096; i++)
            buf[i] = i & 1 ? INT32_MAX : INT32_MIN;
        flac_reduce_tpdf(buf, 4096, 3, 29, &st);
        for (i = 0; i < 4096; i++) {
            int32_t lim = i & 1 ? (1 << 28) - 1 : -(1 << 28);
            if (i & 1 ? buf[i] < lim - 2 || buf[i] > lim
                      : buf[i] > lim + 2 || buf[i] < lim)
                ok = 0;
        }
        check(ok, "full-scale samples clamp inside 29 bits", 0, 0);
        /* the downmix's reduction, 32 -> 24 bits */
        for (i = 0; i < 4096; i++)
            buf[i] = i & 1 ? INT32_MAX : INT32_MIN;
        flac_reduce_tpdf(buf, 4096, 8, 24, &st);
        for (i = 0; i < 4096; i++)
            if (buf[i] > (1 << 23) - 1 || buf[i] < -(1 << 23))
                ok = 0;
        check(ok, "full-scale samples clamp inside 24 bits", 0, 0);
        printf("     extremes: INT32_MIN/MAX clamp inside 29 and 24 bits\n");
    }

    /* 4: fixed seed */
    {
        static int32_t a[1000], b[1000];
        uint32_t sa = 0, sb = 0;
        int i, same = 1;
        for (i = 0; i < 1000; i++)
            a[i] = b[i] = (int32_t)(i * 2654435761u);
        flac_reduce_tpdf(a, 1000, 3, 29, &sa);
        flac_reduce_tpdf(b, 1000, 3, 29, &sb);
        for (i = 0; i < 1000; i++)
            same &= a[i] == b[i];
        check(same && sa == sb, "same seed, same output", sa, sb);
        /* x(1000) of x' = 1664525 x + 1013904223 from 0 (python3 -c "x=0
           exec('x=(x*1664525+1013904223)%2**32;'*1000); print(x)") */
        check(sa == 3926946568u, "state after 1000 samples", sa, 3926946568u);
        printf("     seed: fixed, state after 1000 samples %u\n", sa);
    }

    printf("dither: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
