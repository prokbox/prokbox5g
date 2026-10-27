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
 * iPod UI shell
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
#ifndef IPODUI_CLICKTONE_H
#define IPODUI_CLICKTONE_H

/* The Clicker's headphone click (owner device feedback 2026-10-20: the
 * upstream keyclick, a 2 ms 4 kHz square wave at amplitude 5000, was "too
 * tinny and sharp, and too loud"). Pure integer code, used by
 * apps/ipodui/click.c and by tests/click/. Include it from one file only.
 *
 * The click is a damped sine: a smoothstep attack (no step at the start),
 * an exponential decay, and a smoothstep release that lands on exactly zero
 * (no step at the end). It is rendered once per mixer rate into a buffer the
 * caller owns, at CLICK_PEAK_MAX; each click then only sets the beep
 * channel's mixer amplitude (click_amp()) and plays the buffer.
 *
 * Every constant below is for tuning by ear. */

#include <stdint.h>

#define CLICK_FREQ_HZ      1800    /* tone; upstream: 4000 Hz square */
#define CLICK_LEN_US       4000    /* total length */
#define CLICK_ATTACK_US     250    /* smoothstep rise from 0 */
#define CLICK_DECAY_US      800    /* exponential time constant (1/e) */
#define CLICK_RELEASE_US    750    /* smoothstep fall to 0 at the end */
#define CLICK_PEAK_MAX     2500    /* rendered peak = loudest ever played:
                                    * half upstream's level-2 amplitude
                                    * (2 x 2500), and a sine, not a square */
#define CLICK_RATE_MAX    48000    /* this target's top mixer rate */
#define CLICK_FRAMES_MAX  (CLICK_RATE_MAX / 1000 * CLICK_LEN_US / 1000)

/* Level against the master volume. The WM8758's volume scales the beep
 * channel like the music, so a fixed digital level would follow the
 * volume dB for dB. Stock behaviour is not documented (docs/design: the
 * 7G only says the click plays through headphones and speaker), so the
 * click follows the volume gently instead: half a dB per dB around
 * CLICK_VOL_REF_DB, where it plays CLICK_DB_AT_REF below CLICK_PEAK_MAX,
 * clamped to [CLICK_DB_MIN, 0]. At the reference that is a peak of 1250,
 * a quarter of upstream's 5000 (-12 dB), and it never goes above
 * CLICK_PEAK_MAX at any volume. */
#define CLICK_VOL_REF_DB   (-20)   /* master volume, dB */
#define CLICK_DB_AT_REF      (-6)  /* below CLICK_PEAK_MAX */
#define CLICK_DB_MIN        (-18)

/* MIX_AMP_UNITY (0x10000) * 10^(-k / 20), k = 0..-CLICK_DB_MIN */
static const uint32_t click_db_amp[1 - CLICK_DB_MIN] = {
    65536, 58409, 52057, 46396, 41350, 36854, 32846, 29274, 26090,
    23253, 20724, 18471, 16462, 14672, 13076, 11654, 10387,  9257,
     8250,
};

/* Beep-channel mixer amplitude for a master volume in dB */
static inline uint32_t click_amp(int volume_db)
{
    int d = volume_db - CLICK_VOL_REF_DB;
    /* half a dB per dB, rounded away from the reference */
    int half = d >= 0 ? (d + 1) / 2 : -((1 - d) / 2);
    int db = CLICK_DB_AT_REF - half;

    if (db > 0)
        db = 0;
    if (db < CLICK_DB_MIN)
        db = CLICK_DB_MIN;
    return click_db_amp[-db];
}

#define CLICK_Q     30
#define CLICK_ONE   ((int64_t)1 << CLICK_Q)

static inline int64_t click_mul(int64_t a, int64_t b)
{
    return (a * b) >> CLICK_Q;   /* both |a|, |b| <= 2 in Q30 */
}

/* smoothstep x^2 (3 - 2x) of i/n, Q30 */
static inline int64_t click_smooth(int64_t i, int64_t n)
{
    int64_t x = (i << CLICK_Q) / n;
    return click_mul(click_mul(x, x), 3 * CLICK_ONE - 2 * x);
}

/* One pass of the generator. With out == NULL it only measures the peak
 * (Q30 units of the unscaled waveform); otherwise it writes
 * scale * waveform as interleaved stereo int16. */
static int64_t click_pass(int16_t *out, int n, unsigned int fs,
                          int64_t peak_q30)
{
    /* w = 2 pi f / fs; cos and sin by Taylor series (w < pi/2 here for
     * every fs >= 8 kHz, so the series converges well) */
    const int64_t two_pi_q30 = 6746518852LL;
    int64_t w  = two_pi_q30 * CLICK_FREQ_HZ / fs;
    int64_t w2 = click_mul(w, w);
    int64_t c  = CLICK_ONE - w2 / 2 + click_mul(w2, w2) / 24
                 - click_mul(click_mul(w2, w2), w2) / 720
                 + click_mul(click_mul(w2, w2), click_mul(w2, w2)) / 40320;
    int64_t s  = click_mul(w, CLICK_ONE - w2 / 6 + click_mul(w2, w2) / 120
                 - click_mul(click_mul(w2, w2), w2) / 5040
                 + click_mul(click_mul(w2, w2), click_mul(w2, w2)) / 362880);
    /* per-sample decay exp(-a), a = 1 / (tau fs) */
    int64_t a  = (CLICK_ONE * 1000000) / ((int64_t)CLICK_DECAY_US * fs);
    int64_t d  = CLICK_ONE - a + click_mul(a, a) / 2
                 - click_mul(click_mul(a, a), a) / 6;
    int na = (int)((int64_t)fs * CLICK_ATTACK_US / 1000000);
    int nr = (int)((int64_t)fs * CLICK_RELEASE_US / 1000000);
    int64_t ys = 0, yc = CLICK_ONE;     /* sin, cos of the phase */
    int64_t env = CLICK_ONE;
    int64_t peak = 0;

    if (na < 1)
        na = 1;
    if (nr < 1)
        nr = 1;

    for (int i = 0; i < n; i++)
    {
        int64_t v = click_mul(ys, env);
        if (i < na)
            v = click_mul(v, click_smooth(i, na));
        if (i >= n - nr)
            v = click_mul(v, click_smooth(n - 1 - i, nr));

        if (out)
        {
            int32_t q = (int32_t)((v * CLICK_PEAK_MAX
                                   + (v >= 0 ? peak_q30 : -peak_q30) / 2)
                                  / peak_q30);
            out[2 * i] = out[2 * i + 1] = (int16_t)q;
        }
        if (v < 0)
            v = -v;
        if (v > peak)
            peak = v;

        /* rotate the phase by w; decay the envelope */
        int64_t ns = click_mul(ys, c) + click_mul(yc, s);
        yc = click_mul(yc, c) - click_mul(ys, s);
        ys = ns;
        env = click_mul(env, d);
    }
    return peak;
}

/* Renders the click for mixer rate fs into out (interleaved stereo int16,
 * room for max_frames frames), peak CLICK_PEAK_MAX. Returns the frame
 * count; a rate above CLICK_RATE_MAX gives a shorter click that still
 * fades to zero. Run once per rate change, not per click. */
static int click_render(int16_t *out, int max_frames, unsigned int fs)
{
    int n = (int)((int64_t)fs * CLICK_LEN_US / 1000000);
    int64_t peak;

    if (n > max_frames)
        n = max_frames;
    if (n < 2)
        return 0;
    peak = click_pass(0, n, fs, 0);
    if (peak <= 0)
        return 0;
    click_pass(out, n, fs, peak);
    return n;
}

#endif /* IPODUI_CLICKTONE_H */
