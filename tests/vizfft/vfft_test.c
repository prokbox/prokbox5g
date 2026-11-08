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

/* Host test of the visualiser FFT for the BCM2722's vector unit (#126,
 * docs/design/viz-fft-vc.md): the definition (vfft_ref.c), the kernel on
 * the vector-unit model (vfft_vpu.c) and today's reference (jviz.c).
 *
 *  1. tables      vfft_tabs_make() equals gen_viz.py's jviz_tab.h
 *  2. today       VFFT_EXACT equals jviz_raw_ref() (the exact sums) and
 *                 VFFT_SEP equals jviz_power_ref() and jviz_raw_sep512()
 *                 (the BCM's 512-sample definition since batch 5) on every
 *                 bin
 *  3. kernel      the kernel on the model equals the definition, both modes
 *  4. forms       VFFT_SEP uses only forms proven_forms.txt files as
 *                 silicon or apple; VFFT_EXACT's others are listed
 *  5. precision   against a double-precision DFT of the same windowed
 *                 samples: per class SNR and worst bin, per band dB error
 *                 by level
 *  6. bands       vfft_bands()/vfft_l2() equal jviz_bands()/jviz_l2()
 *  7. mutations   each deliberate kernel bug is caught
 *  8. cost        instructions, cycles and code bytes by phase
 *
 * usage: vfft_test [path to tools/bcmre/proven_forms.txt] */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vfft.h"
#include "jviz.h"
#include "viz_core.h"
#pragma GCC diagnostic ignored "-Wunused-const-variable"
#include "jviz_tab.h"

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; \
    printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); \
    printf("\n"); } } while (0)

#define RATE        44100
#define NSIG_MAX    96

static struct vfft_tabs tabs;

/* ---- signals ---------------------------------------------------------- */

enum cls { C_SINE, C_SWEEP, C_NOISE, C_SILENCE, C_CLIP, C_MUSIC, C_CLASSES };
static const char *const cls_name[C_CLASSES] = {
    "sines", "sweeps", "noise", "silence", "clipping", "music-like"
};

struct sig
{
    int16_t pcm[VFFT_N] __attribute__((aligned(32)));   /* split order */
    enum cls cls;
    char name[40];
};

static struct sig sigs[NSIG_MAX];
static int nsig;

static uint32_t rng = 0x2545F491u;

static double urand(void)              /* uniform in [-1, 1) */
{
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return (double)rng / 2147483648.0 - 1.0;
}

static double grand(void)              /* about N(0, 1) */
{
    double s = 0;
    int i;

    for (i = 0; i < 12; i++)
        s += urand();
    return s / 2.0;
}

static int16_t clip16(double v)
{
    long r = lrint(v);

    return (int16_t)(r > 32767 ? 32767 : r < -32768 ? -32768 : r);
}

/* x[0..511] (time order) into a new signal, split order */
static void add(const double *x, enum cls c, const char *name)
{
    struct sig *s = &sigs[nsig++];
    int i;

    for (i = 0; i < VFFT_N; i++)
        s->pcm[(i & 1) ? VFFT_M + i / 2 : i / 2] = clip16(x[i]);
    s->cls = c;
    snprintf(s->name, sizeof(s->name), "%s", name);
}

/* a few seconds of synthetic music, sample t (44.1 kHz): a chord with
 * decaying harmonics over a bass line, a kick, hi-hats, a voice-like
 * formant tone; mastered loud (soft clip) when loud */
static double music(long t, int part, int loud)
{
    static const double chord[3] = { 220.0, 261.63, 329.63 };
    double s = 0, tt = (double)t / RATE, env, f;
    int n, h;

    if (part == 0 || part == 4)                    /* chord + bass */
    {
        for (n = 0; n < 3; n++)
            for (h = 1; h <= 8; h++)
                s += 2500.0 / h * sin(2 * M_PI * chord[n] * h * tt *
                                      (1 + 0.002 * sin(2 * M_PI * 5 * tt)) + n + h);
        s += 6000 * sin(2 * M_PI * 55 * tt) + 2000 * sin(2 * M_PI * 110 * tt + 1);
    }
    if (part == 1 || part == 4)                    /* kick every 0.5 s */
    {
        double k = fmod(tt, 0.5);

        env = exp(-k * 18);
        f = 50 + 90 * exp(-k * 40);
        s += 14000 * env * sin(2 * M_PI * f * k) + 3000 * exp(-k * 60) * grand();
    }
    if (part == 2 || part == 4)                    /* hi-hats, 8th notes */
    {
        static double prev;
        double k = fmod(tt, 0.125), w = grand();

        s += 4000 * exp(-k * 90) * (w - prev);      /* differenced: bright */
        prev = w;
    }
    if (part == 3)                                 /* a voice: 150 Hz, formants */
        for (h = 1; h <= 30; h++)
        {
            double fh = 150.0 * h, a = 0;

            a += exp(-pow((fh - 700) / 150, 2));
            a += 0.6 * exp(-pow((fh - 1200) / 200, 2));
            a += 0.3 * exp(-pow((fh - 2500) / 300, 2));
            s += 9000 * a / sqrt(h) * sin(2 * M_PI * fh * tt + h * 0.7);
        }
    if (loud)
        s = 32767 * tanh(s * 1.8 / 32767);
    return s;
}

static void make_signals(void)
{
    static const double fs[] = { 60, 440, 1000, 1033.6, 3186.9, 5000, 15000 };
    static const double db[] = { 0, -12, -41 };
    double x[VFFT_N], ph, a, f0, f1;
    char nm[40];
    int i, j, k;
    long t0;

    for (i = 0; i < 7; i++)
        for (j = 0; j < 3; j++)
        {
            a = 32767 * pow(10, db[j] / 20) * 0.999;
            ph = urand() * M_PI;
            for (k = 0; k < VFFT_N; k++)
                x[k] = a * sin(2 * M_PI * fs[i] * k / RATE + ph);
            snprintf(nm, sizeof(nm), "sine %.0f Hz %.0f dB", fs[i], db[j]);
            add(x, C_SINE, nm);
        }
    for (j = 0; j < 6; j++)                     /* windows along a 0.5 s chirp */
    {
        t0 = j * 3500;
        for (k = 0; k < VFFT_N; k++)
        {
            double t = (double)(t0 + k) / RATE;

            x[k] = 30000 * sin(2 * M_PI * (50 * t + (16000 - 50) / (2 * 0.5) * t * t));
        }
        snprintf(nm, sizeof(nm), "chirp window %d", j);
        add(x, C_SWEEP, nm);
    }
    f0 = 100;
    f1 = 16000;                                 /* a log sweep inside the window */
    for (k = 0; k < VFFT_N; k++)
        x[k] = 20000 * sin(2 * M_PI * f0 * VFFT_N / RATE / log(f1 / f0) *
                           (pow(f1 / f0, (double)k / VFFT_N) - 1));
    add(x, C_SWEEP, "log sweep in-window");
    for (j = 0; j < 3; j++)
    {
        for (k = 0; k < VFFT_N; k++)
            x[k] = 32767 * urand();
        add(x, C_NOISE, "uniform noise full scale");
    }
    for (j = 0; j < 3; j++)
    {
        for (k = 0; k < VFFT_N; k++)
            x[k] = 32767 * 0.25 * grand();
        add(x, C_NOISE, "gaussian noise -12 dB");
    }
    for (j = 0; j < 2; j++)                     /* pink-ish: Paul Kellet's filter */
    {
        double b0 = 0, b1 = 0, b2 = 0, w;

        for (k = -2000; k < VFFT_N; k++)
        {
            w = grand();
            b0 = 0.99765 * b0 + w * 0.0990460;
            b1 = 0.96300 * b1 + w * 0.2965164;
            b2 = 0.57000 * b2 + w * 1.0526913;
            if (k >= 0)
                x[k] = 4000 * (b0 + b1 + b2 + w * 0.1848);
        }
        add(x, C_NOISE, "pink noise");
    }
    memset(x, 0, sizeof(x));
    add(x, C_SILENCE, "digital silence");
    for (k = 0; k < VFFT_N; k++)
        x[k] = urand() < 0 ? -1 : 1;
    add(x, C_SILENCE, "1-LSB dither");
    for (k = 0; k < VFFT_N; k++)
        x[k] = lrint(2 * urand());
    add(x, C_SILENCE, "2-LSB dither");
    for (k = 0; k < VFFT_N; k++)
        x[k] = 4 * 32767 * sin(2 * M_PI * 440 * k / RATE);
    add(x, C_CLIP, "sine 440 Hz x4 clipped");
    for (k = 0; k < VFFT_N; k++)
        x[k] = 4 * 32767 * sin(2 * M_PI * 3000 * k / RATE + 0.3);
    add(x, C_CLIP, "sine 3 kHz x4 clipped");
    for (k = 0; k < VFFT_N; k++)
        x[k] = fmod(k * 1000.0 / RATE, 1.0) < 0.5 ? 32767 : -32768;
    add(x, C_CLIP, "square 1 kHz full scale");
    for (k = 0; k < VFFT_N; k++)
        x[k] = 32767;
    add(x, C_CLIP, "DC +32767");
    for (k = 0; k < VFFT_N; k++)
        x[k] = -32768;
    add(x, C_CLIP, "DC -32768");
    for (k = 0; k < VFFT_N; k++)
        x[k] = (k & 1) ? -32768 : 32767;
    add(x, C_CLIP, "Nyquist full scale");
    memset(x, 0, sizeof(x));
    x[256] = 32767;
    add(x, C_CLIP, "impulse at the centre");
    memset(x, 0, sizeof(x));
    x[0] = -32768;
    add(x, C_CLIP, "impulse at the window's zero");
    for (k = 0; k < VFFT_N; k++)
        x[k] = 4 * 32767 * urand();
    add(x, C_CLIP, "noise x4 clipped");
    for (j = 0; j < 5; j++)
        for (i = 0; i < 4; i++)
        {
            t0 = 4410 * (i * 7 + 3) + 100 * j;
            for (k = 0; k < VFFT_N; k++)
                x[k] = music(t0 + k, j, j == 4);
            snprintf(nm, sizeof(nm), "music %s at %ld",
                     (const char *[]){ "chord", "kick", "hats", "voice", "mix loud" }[j],
                     t0);
            add(x, C_MUSIC, nm);
        }
}

/* ---- the double-precision reference ------------------------------------ */

/* X[k] / 256 of the windowed samples (the window table as the kernel uses
 * it, so only the arithmetic differs), k < 256 */
static void dft(double *xr, double *xi, const int16_t *pcm)
{
    static double c[VFFT_N], s[VFFT_N];
    static int init;
    double w[VFFT_N];
    int t, k;

    if (!init)
    {
        for (t = 0; t < VFFT_N; t++)
        {
            c[t] = cos(2 * M_PI * t / VFFT_N);
            s[t] = sin(2 * M_PI * t / VFFT_N);
        }
        init = 1;
    }
    for (t = 0; t < VFFT_N; t++)
    {
        int at = (t & 1) ? VFFT_M + t / 2 : t / 2;

        w[t] = (double)pcm[at] * tabs.win[at] / 65536.0;
    }
    for (k = 0; k < VFFT_M; k++)
    {
        double re = 0, im = 0;

        for (t = 0; t < VFFT_N; t++)
        {
            re += w[t] * c[(k * t) & (VFFT_N - 1)];
            im -= w[t] * s[(k * t) & (VFFT_N - 1)];
        }
        xr[k] = re / VFFT_M;
        xi[k] = im / VFFT_M;
    }
}

/* ---- 1. tables -------------------------------------------------------- */

static void test_tables(void)
{
    const struct jviz_tabs *j = jviz_tabs(512);
    int k, bad = 0;

    for (k = 0; k < VFFT_N; k++)
        bad += tabs.win[k] != j->win[k] || tabs.tw[k] != j->tw[k];
    /* jviz keeps W_512^k at row k & 15, lane k >> 4 (bin k = k2 + 16 k1);
       vfft at k (natural order): the same values, transposed */
    for (k = 0; k < VFFT_M; k++)
        bad += tabs.ws[k] != j->ws[16 * (k & 15) + (k >> 4)] ||
               tabs.ws[VFFT_M + k] != j->ws[VFFT_M + 16 * (k & 15) + (k >> 4)] ||
               tabs.ws[k] != jviz_vfft_tabs.wsn[k] ||
               tabs.ws[VFFT_M + k] != jviz_vfft_tabs.wsn[VFFT_M + k];
    for (k = 0; k < VFFT_N; k++)
        bad += jviz_vfft_tabs.win[k] != tabs.win[k] || jviz_vfft_tabs.tw[k] != tabs.tw[k];
    for (k = 0; k < 16; k++)
        bad += jviz_vfft_tabs.lane0[k] != (k ? 0 : -1);
    for (k = 0; k < 16; k++)
        bad += tabs.w16[k] != jviz_w16[k] || tabs.log16[k] != jviz_log16[k];
    CHECK(bad == 0, "tables: %d values differ from jviz_tab.h", bad);
    printf("  1 tables: window, twiddles, W_16, log16 equal jviz_tab.h (%d values)\n",
           2 * VFFT_N + 2 * VFFT_M + 32);
}

/* ---- 2, 3. today's arithmetic and the kernel --------------------------- */

static void test_exact_and_kernel(void)
{
    uint16_t raw[VFFT_RAW_HW] __attribute__((aligned(32)));
    uint16_t rv[VFFT_RAW_HW] __attribute__((aligned(32)));
    uint16_t rj[VFFT_RAW_HW];
    uint32_t p[VFFT_M], pj[VFFT_M], top = 0;
    int i, k, m, d_today = 0, d_sep = 0, d_kern[2] = { 0, 0 };

    for (i = 0; i < nsig; i++)
    {
        vfft_ref(raw, sigs[i].pcm, &tabs, VFFT_SEP);
        vfft_power(p, raw);
        jviz_power_ref(pj, sigs[i].pcm, 512);
        jviz_raw_sep512(rj, sigs[i].pcm);
        for (k = 0; k < VFFT_M; k++)
            d_sep += p[k] != pj[k] || raw[k] != rj[k] || raw[VFFT_M + k] != rj[VFFT_M + k];
        vfft_ref(raw, sigs[i].pcm, &tabs, VFFT_EXACT);
        vfft_power(p, raw);
        jviz_raw_ref(rj, sigs[i].pcm, 512);
        for (k = 0; k < VFFT_M; k++)
            pj[k] = rj[16 * (k & 15) + (k >> 4)] |
                    (uint32_t)rj[VFFT_M + 16 * (k & 15) + (k >> 4)] << 16;
        for (k = 0; k < VFFT_M; k++)
            if (p[k] != pj[k])
            {
                if (!d_today++)
                    printf("     %s: bin %d: exact %u, jviz %u\n", sigs[i].name,
                           k, p[k], pj[k]);
            }
        for (m = 0; m < 2; m++)
        {
            vfft_ref(raw, sigs[i].pcm, &tabs, (enum vfft_mode)m);
            vfft_vpu(rv, sigs[i].pcm, &tabs, (enum vfft_mode)m, VFFT_MUT_NONE, 0, 0, 0);
            if (memcmp(raw, rv, sizeof(raw)))
            {
                if (!d_kern[m]++)
                    for (k = 0; k < VFFT_RAW_HW; k++)
                        if (raw[k] != rv[k])
                        {
                            printf("     %s mode %d: hw %d: ref %04x, kernel %04x\n",
                                   sigs[i].name, m, k, raw[k], rv[k]);
                            break;
                        }
            }
            vfft_power(p, raw);
            for (k = 0; k < VFFT_M; k++)
                top = p[k] > top ? p[k] : top;
        }
    }
    CHECK(d_today == 0, "VFFT_EXACT differs from jviz_raw_ref on %d bins", d_today);
    CHECK(d_sep == 0, "VFFT_SEP differs from jviz_power_ref/jviz_raw_sep512 on %d bins",
          d_sep);
    CHECK(d_kern[0] == 0 && d_kern[1] == 0,
          "the kernel differs from the definition on %d (SEP) / %d (EXACT) signals",
          d_kern[0], d_kern[1]);
    /* the power is |X / 256|^2 < 2^31: the accumulator never saturates */
    CHECK(top < 0x7FFFFFFFu, "a power reached the accumulator's limit");
    printf("  2 jviz.c: VFFT_EXACT = jviz_raw_ref, VFFT_SEP = jviz_power_ref = "
           "jviz_raw_sep512, all %d signals x 256 bins\n", nsig);
    printf("  3 kernel: the model's instructions = the definition, bit for bit, "
           "both modes, %d signals (largest power %u, %.1f%% of 2^31)\n",
           nsig, top, 100.0 * top / 2147483648.0);
}

/* ---- 4. forms --------------------------------------------------------- */

#define FORMS_MAX 64
struct forms
{
    char key[FORMS_MAX][96];
    uint32_t n[FORMS_MAX], rows[FORMS_MAX];
    int count;
};

static void form_cb(const char *key, uint32_t rep, void *arg)
{
    struct forms *f = arg;
    int i;

    for (i = 0; i < f->count; i++)
        if (!strcmp(f->key[i], key))
            break;
    if (i == f->count)
    {
        if (f->count == FORMS_MAX)
            return;
        snprintf(f->key[f->count++], 96, "%s", key);
    }
    f->n[i]++;
    f->rows[i] += rep;
}

/* the class tools/bcmre/proven_forms.txt gives a vector form key, "absent"
 * if none */
static const char *form_class(const char *path, const char *key, char *evid,
                              size_t en)
{
    static char cls[32];
    char line[1024], want[128];
    FILE *fp = fopen(path, "r");

    snprintf(cls, sizeof(cls), "absent");
    if (evid)
        evid[0] = 0;
    if (!fp)
        return 0;
    snprintf(want, sizeof(want), "vector | %s | ", key);
    while (fgets(line, sizeof(line), fp))
        if (!strncmp(line, want, strlen(want)))
        {
            char *c = line + strlen(want), *e = strstr(c, " | ");

            if (e)
            {
                *e = 0;
                if (evid)
                    snprintf(evid, en, "%.*s", (int)strcspn(e + 3, "\n"), e + 3);
            }
            snprintf(cls, sizeof(cls), "%s", c);
            break;
        }
    fclose(fp);
    return cls;
}

static void test_forms(const char *path)
{
    static struct forms f[2];
    uint16_t raw[VFFT_RAW_HW] __attribute__((aligned(32)));
    static const char *const want_unproven[] = {
        "vmul Ddiscard AH16 BH16 clra x15 80",
        "vmul Ddiscard AH16 BH16 x15 80",
        "vmulhd Ddiscard AH16 BH16 acc 80",
        "vmulhd DH16 AH16 BH16 acc 80",
    };
    int m, i, j, unproven = 0;
    const char *c;

    for (m = 0; m < 2; m++)
        vfft_vpu(raw, sigs[0].pcm, &tabs, (enum vfft_mode)m, VFFT_MUT_NONE, 0,
                 form_cb, &f[m]);
    if (!form_class(path, "vadd DH16 AH16 BH16 - 80", 0, 0))
    {
        printf("  4 forms: SKIPPED (no %s)\n", path);
        return;
    }
    printf("  4 forms of VFFT_SEP (the proposal), %d kinds:\n", f[0].count);
    for (i = 0; i < f[0].count; i++)
    {
        char ev[160];

        c = form_class(path, f[0].key[i], ev, sizeof(ev));
        printf("     %-40s %4u insns %5u rows  %-7s %.60s\n", f[0].key[i],
               f[0].n[i], f[0].rows[i], c, ev);
        CHECK(!strcmp(c, "silicon") || !strcmp(c, "apple"),
              "VFFT_SEP uses %s (%s): not proven", f[0].key[i], c);
    }
    printf("     VFFT_EXACT's forms beyond those (each needs a bench row):\n");
    for (i = 0; i < f[1].count; i++)
    {
        c = form_class(path, f[1].key[i], 0, 0);
        if (!strcmp(c, "silicon") || !strcmp(c, "apple"))
            continue;
        unproven++;
        printf("     %-40s %4u insns  %s\n", f[1].key[i], f[1].n[i], c);
        for (j = 0; j < 4; j++)
            if (!strcmp(f[1].key[i], want_unproven[j]))
                break;
        CHECK(j < 4, "VFFT_EXACT: an unexpected unproven form %s", f[1].key[i]);
    }
    CHECK(unproven == 4, "VFFT_EXACT: %d unproven forms, expected 4", unproven);
}

/* ---- 5. precision ----------------------------------------------------- */

/* band levels in dB above 1 unit^2 (full scale is about 87 dB; the shell
 * shows 48 dB below its auto-gain top) */
#define BUCKETS 7
static const double bucket_lo[BUCKETS] = { 70, 60, 50, 40, 30, 20, -1e9 };
static const char *const bucket_name[BUCKETS] = {
    ">= 70 dB", "60-70 dB", "50-60 dB", "40-50 dB", "30-40 dB", "20-30 dB", "< 20 dB"
};
#define BAND_E      6.0         /* units: the bound on a band's amplitude
                                   error the precision test holds */

struct prec
{
    double sig[C_CLASSES], err[C_CLASSES], worst[C_CLASSES];
    uint32_t bins[C_CLASSES];
    double band_db[BUCKETS], band_amp;
    uint32_t band_n[BUCKETS];
};

static void precision(enum vfft_mode mode, const uint16_t *e, unsigned nb,
                      struct prec *pr)
{
    int16_t xr[VFFT_M], xi[VFFT_M];
    double dr[VFFT_M], di[VFFT_M];
    int i, k, b, q;

    memset(pr, 0, sizeof(*pr));
    for (i = 0; i < nsig; i++)
    {
        enum cls c = sigs[i].cls;

        vfft_ref_x(xr, xi, sigs[i].pcm, &tabs, mode);
        dft(dr, di, sigs[i].pcm);
        for (k = 0; k < VFFT_M; k++)
        {
            double er = xr[k] - dr[k], ei = xi[k] - di[k], ea = hypot(er, ei);

            pr->sig[c] += dr[k] * dr[k] + di[k] * di[k];
            pr->err[c] += er * er + ei * ei;
            pr->bins[c]++;
            if (ea > pr->worst[c])
                pr->worst[c] = ea;
        }
        /* bands: the largest power in each band's bins, model and double */
        for (b = 0; b < (int)nb; b++)
        {
            double pm = 0, pd = 0, lvl, ed, ea;

            for (k = e[b]; k < e[b + 1]; k++)
            {
                double m2 = (double)xr[k] * xr[k] + (double)xi[k] * xi[k];
                double d2 = dr[k] * dr[k] + di[k] * di[k];

                pm = m2 > pm ? m2 : pm;
                pd = d2 > pd ? d2 : pd;
            }
            ea = fabs(sqrt(pm) - sqrt(pd));     /* amplitude error, units */
            pr->band_amp = ea > pr->band_amp ? ea : pr->band_amp;
            if (pd < 1)
                continue;                       /* no level to compare */
            lvl = 10 * log10(pd);               /* dB above 1 unit^2 */
            ed = fabs(10 * log10((pm > 0 ? pm : 0.25) / pd));
            for (q = 0; q < BUCKETS && lvl < bucket_lo[q]; q++)
                ;
            if (ed > pr->band_db[q])
                pr->band_db[q] = ed;
            pr->band_n[q]++;
        }
    }
}

static void test_precision(void)
{
    static struct prec pr[2];
    uint16_t e[VIZ_BANDS_MAX + 1];
    unsigned nb = viz_edges(e, VIZ_BANDS, VFFT_N, RATE), b, q, m, c;
    double fs = 20 * log10(23000.0);            /* the largest |X / 256| */

    for (m = 0; m < 2; m++)
        precision((enum vfft_mode)m, e, nb, &pr[m]);
    printf("  5 precision against a double DFT of the same windowed samples "
           "(units: X / 256; full scale about %.0f dB):\n", fs);
    printf("     %-11s %24s %24s\n", "", "SEP: SNR, floor, worst",
           "EXACT: SNR, floor, worst");
    for (c = 0; c < C_CLASSES; c++)
    {
        double fl[2], snr[2];

        for (m = 0; m < 2; m++)
        {
            /* the floor: the mean error power of a bin, in dB below full
               scale (23000 units) */
            fl[m] = 10 * log10(pr[m].err[c] / pr[m].bins[c]) - fs;
            snr[m] = pr[m].sig[c] > 0 ? 10 * log10(pr[m].sig[c] / pr[m].err[c]) : 0;
        }
        printf("     %-11s %6.1f dB %6.1f dB %4.1f u %6.1f dB %6.1f dB %4.1f u\n",
               cls_name[c], snr[0], fl[0], pr[0].worst[c], snr[1], fl[1],
               pr[1].worst[c]);
        /* every bin within a handful of units of the double DFT, and the
           mean error power a bin at least 80 dB below full scale */
        CHECK(pr[0].worst[c] < 12 && pr[1].worst[c] < 12,
              "%s: a bin is off by %.1f / %.1f units", cls_name[c],
              pr[0].worst[c], pr[1].worst[c]);
        CHECK(fl[0] < -80 && fl[1] < -80, "%s: floor %.1f / %.1f dB", cls_name[c],
              fl[0], fl[1]);
    }
    printf("     bands (%u, 50 Hz to 16 kHz at 512 / 44.1 kHz): worst |dB error| "
           "by the band's level (dB above 1 unit^2):\n", nb);
    printf("     %-10s %10s %10s %8s %s\n", "level", "SEP", "EXACT", "bands",
           "bound (E = 6 u)");
    for (q = 0; q < BUCKETS; q++)
    {
        /* a band whose amplitude is A units and off by at most E units is
           off by at most 20 log10(A / (A - E)) dB */
        double a = pow(10, bucket_lo[q] / 20);

        if (q < BUCKETS - 1)
        {
            double bound = 20 * log10(a / (a - BAND_E));

            printf("     %-10s %8.2f dB %8.2f dB %8u %8.2f dB\n", bucket_name[q],
                   pr[0].band_db[q], pr[1].band_db[q], pr[0].band_n[q], bound);
            CHECK(pr[0].band_db[q] <= bound && pr[1].band_db[q] <= bound,
                  "bands %s: %.2f / %.2f dB past %.2f", bucket_name[q],
                  pr[0].band_db[q], pr[1].band_db[q], bound);
        }
        else
            printf("     %-10s %8.2f dB %8.2f dB %8u %8s\n", bucket_name[q],
                   pr[0].band_db[q], pr[1].band_db[q], pr[0].band_n[q], "none");
    }
    printf("     band amplitude error at most %.1f (SEP) / %.1f (EXACT) units\n",
           pr[0].band_amp, pr[1].band_amp);
    CHECK(pr[0].band_amp <= BAND_E && pr[1].band_amp <= BAND_E,
          "band amplitude error %.1f / %.1f units", pr[0].band_amp, pr[1].band_amp);
    printf("     band edges (bins of 86.1 Hz):");
    for (b = 0; b <= nb; b++)
        printf(" %u", e[b]);
    printf("\n");
}

/* ---- 6. bands --------------------------------------------------------- */

static void test_bands(void)
{
    uint16_t raw[VFFT_RAW_HW] __attribute__((aligned(32)));
    uint16_t e[VIZ_BANDS_MAX + 1], l2a[VIZ_BANDS_MAX], l2b[VIZ_BANDS_MAX];
    uint32_t p[VFFT_M], v;
    unsigned nb = viz_edges(e, VIZ_BANDS, VFFT_N, RATE);
    int i, bad = 0, badl = 0;

    for (i = 0; i < nsig; i++)
    {
        vfft_ref(raw, sigs[i].pcm, &tabs, VFFT_SEP);
        vfft_power(p, raw);
        vfft_bands(l2a, raw, e, nb, &tabs);
        jviz_bands(l2b, p, VFFT_M, e, nb);
        bad += memcmp(l2a, l2b, nb * sizeof(l2a[0])) != 0;
    }
    for (v = 0; v < 70000; v++)
        badl += vfft_l2(v, &tabs) != jviz_l2(v);
    for (v = 1; v && v < 0x80000000u; v += v / 7 + 1)
        badl += vfft_l2(v, &tabs) != jviz_l2(v);
    CHECK(bad == 0 && badl == 0, "bands: %d signals, l2: %d values differ", bad, badl);
    printf("  6 bands: vfft_bands = jviz_bands (%d signals), vfft_l2 = jviz_l2\n", nsig);
}

/* ---- 7. mutations ----------------------------------------------------- */

static void test_mutations(void)
{
    static const char *const what[VFFT_MUTATIONS] = {
        0, "partner lane-0 fix skipped", "step 2 sine sign flipped",
        "step 3 bit reversal rows 1, 2 swapped"
    };
    uint16_t raw[VFFT_RAW_HW] __attribute__((aligned(32)));
    uint16_t rv[VFFT_RAW_HW] __attribute__((aligned(32)));
    int m, i, caught;

    for (m = 1; m < VFFT_MUTATIONS; m++)
    {
        caught = 0;
        for (i = 0; i < nsig; i++)
        {
            vfft_ref(raw, sigs[i].pcm, &tabs, VFFT_SEP);
            vfft_vpu(rv, sigs[i].pcm, &tabs, VFFT_SEP, (enum vfft_mutation)m, 0, 0, 0);
            caught += memcmp(raw, rv, sizeof(raw)) != 0;
        }
        CHECK(caught > nsig / 2, "mutation %s caught by only %d signals", what[m],
              caught);
        printf("  7 mutation '%s': %d of %d signals differ\n", what[m], caught, nsig);
    }
}

/* ---- 8. cost ---------------------------------------------------------- */

static void test_cost(void)
{
    uint16_t raw[VFFT_RAW_HW] __attribute__((aligned(32)));
    struct vfft_stats st;
    uint32_t ti = 0, tr = 0, tb = 0, ts = 0;
    double cyc, tc = 0;
    int m, ph;

    for (m = 0; m < 2; m++)
    {
        vfft_vpu(raw, sigs[0].pcm, &tabs, (enum vfft_mode)m, VFFT_MUT_NONE, &st, 0, 0);
        printf("  8 cost, %s (a vector instruction of REP n: n + 0.5 cycles; "
               "a scalar one: 1):\n", m ? "VFFT_EXACT" : "VFFT_SEP");
        printf("     %-13s %6s %6s %6s %6s %8s\n", "phase", "insns", "rows",
               "scalar", "bytes", "cycles");
        ti = tr = tb = ts = 0;
        tc = 0;
        for (ph = 0; ph < VFFT_PHASES && st.name[ph]; ph++)
        {
            cyc = st.rows[ph] + 0.5 * st.insns[ph] + st.scalar[ph];
            printf("     %-13s %6u %6u %6u %6u %8.1f\n", st.name[ph], st.insns[ph],
                   st.rows[ph], st.scalar[ph], st.bytes[ph], cyc);
            ti += st.insns[ph];
            tr += st.rows[ph];
            tb += st.bytes[ph];
            ts += st.scalar[ph];
            tc += cyc;
        }
        printf("     %-13s %6u %6u %6u %6u %8.1f = %.1f us at 27 MHz, %.1f us "
               "at 150 MHz\n", "total", ti, tr, ts, tb, tc, tc / 27, tc / 150);
        if (m == 0)
            CHECK(tc < 2428, "VFFT_SEP: %.0f cycles, over today's jv_fft512 (2,428)",
                  tc);
    }
}

int main(int argc, char **argv)
{
    const char *forms = argc > 1 ? argv[1] : "tools/bcmre/proven_forms.txt";

    vfft_tabs_make(&tabs);
    make_signals();
    printf("vfft: %d signals\n", nsig);
    test_tables();
    test_exact_and_kernel();
    test_forms(forms);
    test_precision();
    test_bands();
    test_mutations();
    test_cost();
    printf("vfft: %d checks, %d failures\n", checks, fails);
    return fails != 0;
}
