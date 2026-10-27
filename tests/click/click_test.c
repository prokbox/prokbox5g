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
 * Host test for the Clicker's soft headphone click (apps/ipodui/clicktone.h,
 * played by apps/ipodui/click.c): peak, length, no step at either end, no
 * DC, spectral centroid near the tone, a smooth waveform (no sample-to-
 * sample jump bigger than the tone itself can make), and the volume curve.
 *
 *   tests/click/run.sh [--wav DIR]
 *
 * --wav DIR writes, at 44.1 kHz, the click at the reference volume, the
 * upstream keyclick it replaces (4 kHz square, 2 ms, amplitude 5000), and
 * a side-by-side train (7 old clicks, a pause, 7 new ones) for listening.
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
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ipodui/clicktone.h"

#define UNITY 0x10000   /* MIX_AMP_UNITY (firmware/export/pcm_mixer.h) */

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; \
    printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

static int16_t buf[CLICK_FRAMES_MAX * 2];

static void check_rate(unsigned int fs, int verbose)
{
    int n = click_render(buf, CLICK_FRAMES_MAX, fs);
    int want = (int)((long)fs * CLICK_LEN_US / 1000000);
    int peak = 0, maxstep = 0, i;
    double sum = 0;

    if (want > CLICK_FRAMES_MAX)
        want = CLICK_FRAMES_MAX;
    CHECK(n == want, "fs %u: %d frames, want %d", fs, n, want);
    for (i = 0; i < n; i++)
    {
        int v = buf[2 * i];
        CHECK(buf[2 * i + 1] == v, "fs %u: L != R at %d", fs, i);
        if (abs(v) > peak)
            peak = abs(v);
        if (i > 0 && abs(v - buf[2 * i - 2]) > maxstep)
            maxstep = abs(v - buf[2 * i - 2]);
        sum += v;
    }
    CHECK(abs(peak - CLICK_PEAK_MAX) <= 1, "fs %u: peak %d", fs, peak);
    /* clean ends: starts and ends at (near) zero, so no pop at either edge */
    CHECK(abs(buf[0]) <= 1, "fs %u: first sample %d", fs, buf[0]);
    CHECK(abs(buf[2 * (n - 1)]) <= 1, "fs %u: last sample %d", fs,
          buf[2 * (n - 1)]);
    /* the 250 us attack spans a few samples at the music rates */
    CHECK(fs < 22050 || abs(buf[2]) <= CLICK_PEAK_MAX / 8,
          "fs %u: second sample %d (step)", fs, buf[2]);
    /* a sine of peak P moves at most P * w per sample; the attack adds a
     * little slope on top */
    double wmax = 2 * M_PI * CLICK_FREQ_HZ / fs;
    if (wmax > 2)
        wmax = 2;
    CHECK(maxstep <= CLICK_PEAK_MAX * wmax * 1.3 + 2,
          "fs %u: largest step %d", fs, maxstep);
    /* DC */
    double dc = sum / n;
    CHECK(fabs(dc) <= CLICK_PEAK_MAX * 0.02, "fs %u: DC %.1f", fs, dc);

    /* power-spectrum centroid, zero-padded DFT */
    int nfft = 4096, k;
    double num = 0, den = 0, hi = 0;
    for (k = 1; k < nfft / 2; k++)
    {
        double re = 0, im = 0, f = (double)k * fs / nfft;
        for (i = 0; i < n; i++)
        {
            double ph = 2 * M_PI * k * i / nfft;
            re += buf[2 * i] * cos(ph);
            im -= buf[2 * i] * sin(ph);
        }
        double p = re * re + im * im;
        num += f * p;
        den += p;
        if (f >= 4000)
            hi += p;
    }
    double centroid = num / den;
    CHECK(centroid >= 1400 && centroid <= 2400, "fs %u: centroid %.0f Hz",
          fs, centroid);
    /* "not tinny": little energy at and above upstream's 4 kHz */
    CHECK(hi / den <= 0.05, "fs %u: %.1f%% of power >= 4 kHz", fs,
          100 * hi / den);
    if (verbose)
        printf("fs %5u: %3d frames (%d B), peak %d, max step %d, DC %.2f, "
               "centroid %.0f Hz, >=4 kHz %.2f%%\n", fs, n,
               (int)(n * 2 * sizeof (int16_t)), peak, maxstep, dc, centroid,
               100 * hi / den);
}

static void check_volume(void)
{
    int v;
    uint32_t prev = UNITY;

    for (int k = 0; k <= -CLICK_DB_MIN; k++)
        CHECK(fabs(click_db_amp[k] - UNITY * pow(10, -k / 20.0)) <= 0.5,
              "db table %d", k);
    CHECK(click_amp(CLICK_VOL_REF_DB) == click_db_amp[-CLICK_DB_AT_REF],
          "reference volume plays CLICK_DB_AT_REF");
    for (v = -90; v <= 6; v++)
    {
        uint32_t a = click_amp(v);
        CHECK(a <= UNITY, "vol %d: amp %u above unity", v, (unsigned)a);
        CHECK(a >= click_db_amp[-CLICK_DB_MIN], "vol %d: below floor", v);
        CHECK(a <= prev, "vol %d: amp rises with volume", v);
        prev = a;
    }
    /* half a dB per dB: 10 dB louder music, 5 dB quieter click digitally */
    CHECK(click_amp(CLICK_VOL_REF_DB + 10)
          == click_db_amp[-CLICK_DB_AT_REF + 5], "half slope up");
    CHECK(click_amp(CLICK_VOL_REF_DB - 6)
          == click_db_amp[-CLICK_DB_AT_REF - 3], "half slope down");
    printf("click peak at volume -40/-20/-10/0/+6 dB: %d/%d/%d/%d/%d "
           "(upstream: 5000 square)\n",
           (int)(CLICK_PEAK_MAX * (int64_t)click_amp(-40) / UNITY),
           (int)(CLICK_PEAK_MAX * (int64_t)click_amp(-20) / UNITY),
           (int)(CLICK_PEAK_MAX * (int64_t)click_amp(-10) / UNITY),
           (int)(CLICK_PEAK_MAX * (int64_t)click_amp(0) / UNITY),
           (int)(CLICK_PEAK_MAX * (int64_t)click_amp(6) / UNITY));
}

/* ---- listening WAVs ---- */

static void put32(FILE *f, uint32_t v)
{
    fputc(v, f); fputc(v >> 8, f); fputc(v >> 16, f); fputc(v >> 24, f);
}

static void put16(FILE *f, uint16_t v)
{
    fputc(v, f); fputc(v >> 8, f);
}

static int write_wav(const char *dir, const char *name, const int16_t *pcm,
                     int frames, unsigned int fs)
{
    char path[1024];
    FILE *f;

    snprintf(path, sizeof path, "%s/%s", dir, name);
    f = fopen(path, "wb");
    if (!f)
        return -1;
    fwrite("RIFF", 1, 4, f); put32(f, 36 + frames * 4);
    fwrite("WAVEfmt ", 1, 8, f); put32(f, 16); put16(f, 1); put16(f, 2);
    put32(f, fs); put32(f, fs * 4); put16(f, 4); put16(f, 16);
    fwrite("data", 1, 4, f); put32(f, frames * 4);
    for (int i = 0; i < frames * 2; i++)
        put16(f, (uint16_t)pcm[i]);
    fclose(f);
    printf("wrote %s\n", path);
    return 0;
}

/* upstream beep_play(4000, 2, 2500 * 2): square wave starting at phase 0 */
static int render_old(int16_t *out, unsigned int fs)
{
    int n = fs / 1000 * 2;
    for (int i = 0; i < n; i++)
    {
        double ph = fmod((double)i * 4000 / fs, 1.0);
        out[2 * i] = out[2 * i + 1] = ph < 0.5 ? 5000 : -5000;
    }
    return n;
}

static void write_wavs(const char *dir)
{
    const unsigned int fs = 44100;
    const int gap = fs * 120 / 1000;      /* a brisk wheel spin */
    int16_t old[2 * 200];
    int nn = click_render(buf, CLICK_FRAMES_MAX, fs);
    int no = render_old(old, fs);
    uint32_t amp = click_amp(CLICK_VOL_REF_DB);
    int16_t *one = calloc((size_t)fs * 2, sizeof *one);
    int16_t *train = calloc((size_t)fs * 2 * 3, sizeof *train);
    int i, c, at = 0;

    /* the new click as played at the reference volume, in 250 ms */
    for (i = 0; i < nn * 2; i++)
        one[i + fs / 20 * 2] = (int16_t)(buf[i] * (int64_t)amp / UNITY);
    write_wav(dir, "click-soft.wav", one, fs / 4, fs);
    memset(one, 0, (size_t)fs * 2 * sizeof *one);
    for (i = 0; i < no * 2; i++)
        one[i + fs / 20 * 2] = old[i];
    write_wav(dir, "click-upstream.wav", one, fs / 4, fs);

    /* 7 upstream clicks, 0.6 s pause, 7 soft clicks */
    at = fs / 10;
    for (c = 0; c < 7; c++, at += gap)
        for (i = 0; i < no * 2; i++)
            train[at * 2 + i] = old[i];
    at += fs * 6 / 10;
    for (c = 0; c < 7; c++, at += gap)
        for (i = 0; i < nn * 2; i++)
            train[at * 2 + i] = (int16_t)(buf[i] * (int64_t)amp / UNITY);
    write_wav(dir, "click-upstream-then-soft.wav", train, at + fs / 5, fs);
    free(one);
    free(train);
}

int main(int argc, char **argv)
{
    static const unsigned int rates[] = {
        8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000,
        96000,
    };

    for (size_t r = 0; r < sizeof rates / sizeof rates[0]; r++)
        check_rate(rates[r], rates[r] == 44100 || rates[r] == 48000);
    check_volume();

    if (argc == 3 && !strcmp(argv[1], "--wav"))
        write_wavs(argv[2]);

    if (fails)
    {
        printf("click: %d FAILED\n", fails);
        return 1;
    }
    printf("click: all passed\n");
    return 0;
}
