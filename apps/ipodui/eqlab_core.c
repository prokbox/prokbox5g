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
 * The codec EQ's band model (pure): the WM8758's 5 bands, their frequency
 * choices, an approximate response curve, the precut and the saved form.
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
#include "eqlab_core.h"

const unsigned short eqlab_hz[EQLAB_BANDS][EQLAB_FREQS] = {
    {   80,  105,  135,   175 },    /* EQ1 low shelf (cut-off) */
    {  230,  300,  385,   500 },    /* EQ2 peak (centre) */
    {  650,  850, 1100,  1400 },    /* EQ3 peak */
    { 1800, 2400, 3200,  4100 },    /* EQ4 peak */
    { 5300, 6900, 9000, 11700 },    /* EQ5 high shelf (cut-off) */
};

/* log2(eqlab_hz) in Q8, rounded (eqlab_log2_q8() agrees within 1) */
static const short eqlab_lf[EQLAB_BANDS][EQLAB_FREQS] = {
    { 1618, 1719, 1812, 1908 },
    { 2008, 2107, 2199, 2295 },
    { 2392, 2491, 2586, 2676 },
    { 2768, 2875, 2981, 3072 },
    { 3167, 3265, 3363, 3460 },
};

const unsigned short eqlab_edge_hz[EQLAB_BANDS - 1] = {
    200, 570, 1590, 4660
};

/* Shapes, 0..255 of the band's gain, every quarter octave (64 in Q8),
 * from the RBJ cookbook responses at +12 dB, 48 kHz (computed offline):
 * the low shelf from 4 octaves below its corner to 4 above (the high
 * shelf is its mirror); the peaks from the centre outwards (symmetric to
 * within a step in the audio band) */
static const unsigned char shape_shelf[33] = {
    255, 255, 255, 255, 255, 255, 255, 254, 254, 252, 250, 245, 236, 221,
    197, 165, 127, 90, 58, 34, 19, 10, 5, 2, 1, 1, 0, 0, 0, 0, 0, 0, 0
};
static const unsigned char shape_narrow[17] = {
    255, 198, 128, 82, 54, 36, 24, 17, 11, 8, 6, 4, 3, 2, 1, 1, 1
};
static const unsigned char shape_wide[17] = {
    255, 238, 201, 162, 127, 98, 74, 56, 41, 30, 22, 16, 11, 8, 6, 4, 3
};

int eqlab_log2_q8(unsigned long v)
{
    int n = 0, i, r;
    uint32_t m;

    if (v < 1)
        return 0;
    while ((v >> n) > 1)
        n++;
    /* the mantissa in [1, 2), Q15 */
    m = n >= 15 ? (uint32_t)(v >> (n - 15)) : (uint32_t)(v << (15 - n));
    r = n << 8;
    for (i = 7; i >= 0; i--)
    {
        m = (m * m) >> 15;              /* m < 2^16: no overflow */
        if (m >= (2u << 15))
        {
            m >>= 1;
            r |= 1 << i;
        }
    }
    return r;
}

/* table t (n entries, a quarter octave apart) at x (Q8 octaves from its
 * first entry), linear between entries, its end value beyond */
static int shape_at(const unsigned char *t, int n, int x)
{
    int i, f;

    if (x <= 0)
        return t[0];
    i = x >> 6;
    if (i >= n - 1)
        return t[n - 1];
    f = x & 63;
    return (t[i] * (64 - f) + t[i + 1] * f) >> 6;
}

/* The bilinear warp of a filter designed at 48 kHz: its response at f is
 * the analogue prototype's at tan(pi f / 48k) / tan(pi f0 / 48k), so the
 * shapes (analogue, octaves from the corner) are read at warped log
 * frequencies. 256 log2(tan(x) / x), x = pi f / 48 kHz, at lf = 2560 +
 * 32 k (1 kHz .. 20.8 kHz); 0 below (under 1/256 octave). Without it the
 * top bands were off by up to 1.8 dB near 17 kHz (tests/eqlevel) */
static const unsigned short warp_tab[36] = {
      1,   1,   1,   1,   1,   1,   2,   2,   2,   3,   3,   4,   4,   5,
      6,   8,   9,  11,  13,  15,  18,  22,  26,  32,  38,  46,  55,  67,
     82, 101, 125, 156, 199, 259, 349, 507
};

static int warp_lf(int lf)
{
    int i, f;

    if (lf <= 2560)
        return lf;
    i = (lf - 2560) >> 5;
    if (i >= 35)
        return lf + warp_tab[35];
    f = (lf - 2560) & 31;
    return lf + ((warp_tab[i] * (32 - f) + warp_tab[i + 1] * f) >> 5);
}

int eqlab_db_q8(const struct dac_eq_band *b, int lf_q8, int shift_q8)
{
    int i, sum = 0;
    /* at a rate fs the filters (designed for 48 kHz) see f * 48k / fs */
    int lw = warp_lf(lf_q8 - shift_q8);

    for (i = 0; i < EQLAB_BANDS; i++)
    {
        int g = b[i].gain, x, s;

        if (!g)
            continue;
        x = lw - warp_lf(eqlab_lf[i][b[i].freq & 3]);
        if (i == 0)
            s = shape_at(shape_shelf, 33, x + 4 * 256);
        else if (i == EQLAB_BANDS - 1)
            s = shape_at(shape_shelf, 33, 4 * 256 - x);
        else
            s = shape_at(b[i].wide ? shape_wide : shape_narrow, 17,
                         x < 0 ? -x : x);
        sum += g * s;                   /* dB * 255 */
    }
    return sum * 256 / 255;
}

int eqlab_precut(const struct dac_eq_band *b)
{
    int lf, top = 0, pc;

    /* every 1/16 octave across the audio band */
    for (lf = EQLAB_LF_MIN; lf <= EQLAB_LF_MAX; lf += 16)
    {
        int d = eqlab_db_q8(b, lf, 0);
        if (d > top)
            top = d;
    }
    pc = (top + 255) >> 8;
    return pc > EQLAB_GAIN_MAX ? EQLAB_GAIN_MAX : pc;
}

void eqlab_fmt_hz(char *buf, int len, unsigned hz, const char *sep)
{
    if (hz < 1000)
        snprintf(buf, len, "%u Hz", hz);
    else if (hz % 1000 < 100)
        snprintf(buf, len, "%u kHz", hz / 1000);
    else
        snprintf(buf, len, "%u%s%u kHz", hz / 1000, sep, (hz % 1000) / 100);
}

void eqlab_fmt_hz_short(char *buf, int len, unsigned hz, const char *sep)
{
    if (hz < 1000)
        snprintf(buf, len, "%u", hz);
    else if (hz % 1000 < 100)
        snprintf(buf, len, "%uk", hz / 1000);
    else
        snprintf(buf, len, "%u%s%uk", hz / 1000, sep, (hz % 1000) / 100);
}

#define PACK_MAGIC0 'E'
#define PACK_MAGIC1 'Q'
#define PACK_MAGIC2 'L'
#define PACK_VER    '1'

static uint8_t pack_check(const uint8_t *p)
{
    uint8_t c = 0x5a;
    int i;

    for (i = 0; i < EQLAB_PACKED - 1; i++)
        c = (uint8_t)((c << 1 | c >> 7) ^ p[i]);
    return c;
}

void eqlab_pack(const struct dac_eq_band *b, uint8_t *out)
{
    int i;

    out[0] = PACK_MAGIC0;
    out[1] = PACK_MAGIC1;
    out[2] = PACK_MAGIC2;
    out[3] = PACK_VER;
    for (i = 0; i < EQLAB_BANDS; i++)
    {
        out[4 + 3 * i] = (uint8_t)(b[i].gain + EQLAB_GAIN_MAX);
        out[5 + 3 * i] = b[i].freq & 3;
        out[6 + 3 * i] = eqlab_has_width(i) && b[i].wide;
    }
    out[EQLAB_PACKED - 1] = pack_check(out);
}

bool eqlab_unpack(const uint8_t *in, int len, struct dac_eq_band *b)
{
    struct dac_eq_band t[EQLAB_BANDS];
    int i;

    if (len != EQLAB_PACKED || in[0] != PACK_MAGIC0 || in[1] != PACK_MAGIC1
        || in[2] != PACK_MAGIC2 || in[3] != PACK_VER
        || in[EQLAB_PACKED - 1] != pack_check(in))
        return false;
    for (i = 0; i < EQLAB_BANDS; i++)
    {
        int g = in[4 + 3 * i], f = in[5 + 3 * i], w = in[6 + 3 * i];

        if (g > 2 * EQLAB_GAIN_MAX || f > 3 || w > 1
            || (w && !eqlab_has_width(i)))
            return false;
        t[i].gain = (int8_t)(g - EQLAB_GAIN_MAX);
        t[i].freq = (uint8_t)f;
        t[i].wide = (uint8_t)w;
    }
    for (i = 0; i < EQLAB_BANDS; i++)
        b[i] = t[i];
    return true;
}

/* #203: BS.1770 K-weighting power (both stages, 48 kHz) at the grid
 * points EQLAB_LF_MIN + 32 k (1/8 octave, 20 Hz .. 19.4 kHz), Q12;
 * computed offline from the standard's coefficients */
#define LOUD_PTS    80
static const unsigned short loud_w[LOUD_PTS] = {
      192,   251,   324,   413,   520,   646,   791,   954,  1134,  1328,
     1532,  1743,  1955,  2165,  2369,  2564,  2747,  2916,  3071,  3211,
     3336,  3447,  3545,  3630,  3705,  3769,  3825,  3872,  3914,  3949,
     3980,  4006,  4030,  4051,  4071,  4090,  4110,  4132,  4158,  4191,
     4232,  4288,  4362,  4462,  4596,  4775,  5010,  5311,  5683,  6125,
     6627,  7165,  7709,  8227,  8694,  9092,  9418,  9674,  9870, 10017,
    10125, 10203, 10259, 10299, 10327, 10347, 10361, 10370, 10377, 10382,
    10385, 10387, 10389, 10390, 10390, 10391, 10391, 10391, 10391, 10391,
};

/* 2^(i/16) in Q16, i = 0..16 */
static const unsigned int pow2_frac[17] = {
    65536, 68438, 71468, 74632, 77936, 81386, 84990, 88752, 92682,
    96785, 101070, 105545, 110218, 115098, 120194, 125515, 131072
};

/* 10^(db_q8 / 2560) in Q16 (a power ratio), db_q8 within +-48 dB */
static uint32_t db_q8_to_pow_q16(int db_q8)
{
    /* log2(10)/10 = 0.33219 = 85.04 / 256 */
    int l = db_q8 * 85, n, f;
    uint32_t m;

    l = l >= 0 ? (l + 128) >> 8 : -((-l + 128) >> 8);   /* log2, Q8 */
    n = l >> 8;                         /* floor, also for l < 0 */
    if (n > 14)
        n = 14;                         /* +42 dB: no curve reaches it */
    f = l & 255;
    m = pow2_frac[f >> 4] + (((pow2_frac[(f >> 4) + 1] - pow2_frac[f >> 4])
                              * (uint32_t)(f & 15)) >> 4);
    return n >= 0 ? m << n : m >> -n;
}

void eqlab_level(const struct dac_eq_band *b, struct eqlab_level *lv)
{
    uint64_t num = 0, den = 0;
    int k, l, mk;

    for (k = 0; k < LOUD_PTS; k++)
    {
        int d = eqlab_db_q8(b, EQLAB_LF_MIN + 32 * k, 0);
        num += (uint64_t)loud_w[k] * db_q8_to_pow_q16(d);
        den += (uint64_t)loud_w[k] << 16;
    }
    /* 10 log10(num / den) in tenth dB = 30.103 * log2(num / den) */
    l = eqlab_log2_q8((unsigned long)((num << 16) / den)) - 16 * 256;
    lv->loud_db10 = l >= 0 ? (l * 30103 + 128000) / 256000
                           : -((-l * 30103 + 128000) / 256000);
    lv->precut_db = eqlab_precut(b);
    mk = lv->precut_db * 10 - lv->loud_db10;
    if (mk < 0)
        mk = 0;
    if (mk > lv->precut_db * 10)
        mk = lv->precut_db * 10;
    lv->makeup_db10 = mk;
}

void eqlab_flat(struct dac_eq_band *b)
{
    int i;

    for (i = 0; i < EQLAB_BANDS; i++)
    {
        b[i].gain = 0;
        b[i].freq = EQLAB_FREQ_DEF;
        b[i].wide = 0;
    }
}

int eqlab_format(const struct dac_eq_band *b, char *buf, int len)
{
    int i, n;

    n = snprintf(buf, len, "eq1");
    for (i = 0; i < EQLAB_BANDS && n < len; i++)
        n += snprintf(buf + n, len - n, " %+d@%u%s", b[i].gain,
                      eqlab_hz[i][b[i].freq & 3],
                      eqlab_has_width(i) && b[i].wide ? "w" : "");
    return n < len ? n : len - 1;
}

/* a decimal number at *p, advancing it; false if none */
static bool parse_num(const char **p, int *v)
{
    const char *s = *p;
    int sign = 1, n = 0, digits = 0;

    if (*s == '+' || *s == '-')
        sign = *s++ == '-' ? -1 : 1;
    while (*s >= '0' && *s <= '9' && digits < 6)
    {
        n = n * 10 + (*s++ - '0');
        digits++;
    }
    if (!digits || (*s >= '0' && *s <= '9'))
        return false;
    *v = sign * n;
    *p = s;
    return true;
}

bool eqlab_parse(const char *s, struct dac_eq_band *b)
{
    struct dac_eq_band t[EQLAB_BANDS];
    int i, f;

    if (!*s)
    {
        eqlab_flat(b);
        return true;
    }
    if (s[0] != 'e' || s[1] != 'q' || s[2] != '1')
        return false;
    s += 3;
    for (i = 0; i < EQLAB_BANDS; i++)
    {
        int g, hz;

        if (*s++ != ' ' || (*s != '+' && *s != '-') || !parse_num(&s, &g)
            || g < -EQLAB_GAIN_MAX || g > EQLAB_GAIN_MAX || *s++ != '@'
            || !parse_num(&s, &hz))
            return false;
        for (f = 0; f < EQLAB_FREQS; f++)
            if (eqlab_hz[i][f] == hz)
                break;
        if (f == EQLAB_FREQS)
            return false;
        t[i].gain = (int8_t)g;
        t[i].freq = (uint8_t)f;
        t[i].wide = 0;
        if (*s == 'w')
        {
            if (!eqlab_has_width(i))
                return false;
            t[i].wide = 1;
            s++;
        }
    }
    if (*s)
        return false;
    for (i = 0; i < EQLAB_BANDS; i++)
        b[i] = t[i];
    return true;
}
