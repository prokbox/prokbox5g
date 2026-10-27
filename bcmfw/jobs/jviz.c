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

/* The visualizer's reference: the spectrum and the bars (jviz.h).  Plain
 * C, no allocation, no floats: it builds for the Mac, the PP and the BCM
 * (vc2cc) alike.  The FFT below is the vector kernel's arithmetic step by
 * step (bcmfw/video/gen_viz.py): every value a vector lane would hold is
 * wrapped to 16 bits where the lane would wrap it, and every summed
 * product is summed in 32 bits and floored once, as the accumulator does
 * (isa.md, "The 32-bit accumulator"). */
#include <stdint.h>
#include "jviz.h"
#ifdef JVIZ_OVL
/* The overlay's build (bcmfw/build.py ovl_flags): its region is 20 KB, so
 * the 256-sample tables (1.5 KB) are not loaded but derived, exactly,
 * from the 512-sample ones into the caller's buffer (jviz_derive256): the
 * window's evens at 2j and odds at 2j + 1 (w_256(t) = w_512(2t)), the
 * twiddles' every other row (W_128^(l k2) = W_256^(l 2k2), W_256^k =
 * W_512^(2k)); viz_test checks the identity against jviz_tab.h */
#pragma GCC diagnostic ignored "-Wunused-const-variable"
#endif
#include "jviz_tab.h"

#ifdef JVIZ_OVL
static struct jviz_tabs tabs256 = { 0, 0, 0, 8 };

void jviz_derive256(int16_t *buf)
{
    uint32_t j;

    for (j = 0; j < 256; j++)
    {
        buf[j] = jviz_win512[j < 128 ? 2 * j : 2 * j - 255];
        buf[256 + j] = jviz_tw512[32 * (j >> 4) + (j & 15)];
        buf[512 + j] = jviz_ws512[32 * (j >> 4) + (j & 15)];
    }
    tabs256.win = buf;
    tabs256.tw = buf + 256;
    tabs256.ws = buf + 512;
}
#else
static const struct jviz_tabs tabs256 = { jviz_win256, jviz_tw256, jviz_ws256, 8 };
#endif
#ifndef JVIZ_N256_ONLY
static const struct jviz_tabs tabs512 = { jviz_win512, jviz_tw512, jviz_ws512, 16 };

const struct jviz_tabs *jviz_tabs(uint32_t n)
{
    return n == 256 ? &tabs256 : n == 512 ? &tabs512 : 0;
}

/* the size's tables, never 0 (n not 256: 512's), so no null path */
static const struct jviz_tabs *tabs(uint32_t n)
{
    return n == 256 ? &tabs256 : &tabs512;
}
#else
/* The PP's build (apps/ipodui/viz_jviz.c): its CPU path runs 256 samples
 * (viz.c N_CPU), so the 512-sample tables (3 KB) stay out:
 * jviz_power_ref() of 512 samples gives no bins (the reference entry
 * points, jviz_fft_ref and jviz_raw_ref, are the tests' and the BCM's) */
const struct jviz_tabs *jviz_tabs(uint32_t n)
{
    return n == 256 ? &tabs256 : 0;
}

static const struct jviz_tabs *tabs(uint32_t n)
{
    (void)n;
    return &tabs256;
}
#endif

/* a lane's 16 bits */
static int32_t w16(uint32_t v)
{
    return (int16_t)(uint16_t)v;
}

/* the accumulator's high half: the 32-bit sum, floored by 2^16 */
static int32_t hi16(uint32_t acc)
{
    return (int16_t)(uint16_t)(acc >> 16);
}

static uint32_t mul(int32_t a, int32_t b)
{
    return (uint32_t)(a * b);
}

/* bit reversal of i over bits bits */
static uint32_t brev(uint32_t i, uint32_t bits)
{
    uint32_t r = 0;

    while (bits--)
    {
        r = r << 1 | (i & 1);
        i >>= 1;
    }
    return r;
}

/* b = d * W_16^e / 2: d >> 1 for e 0, d * -i >> 1 for e 4, else the
 * exact sums floored (the kernel: vmul x15 and vmulhd acc pairs) */
static void twiddle(int16_t *br, int16_t *bi, int32_t dr, int32_t di, uint32_t e)
{
    int32_t wr, wi;

    if (e == 0)
    {
        *br = dr >> 1;
        *bi = di >> 1;
    }
    else if (e == 4)
    {
        *br = di >> 1;
        *bi = w16(0u - (uint32_t)dr) >> 1;
    }
    else
    {
        wr = jviz_w16[2 * e];
        wi = jviz_w16[2 * e + 1];
        *br = hi16(mul(dr, wr) + mul(di, -wi));
        *bi = hi16(mul(dr, wi) + mul(di, wr));
    }
}

/* Radix-2 decimation in frequency over L points re[k s], im[k s], every
 * stage but the last (h = 1), each halving */
static void stages(int16_t *re, int16_t *im, uint32_t s, uint32_t L)
{
    uint32_t h, g, j, a, b;
    int32_t dr, di;

    for (h = L / 2; h >= 2; h >>= 1)
        for (g = 0; g < L; g += 2 * h)
            for (j = 0; j < h; j++)
            {
                a = (g + j) * s;
                b = (g + j + h) * s;
                dr = w16((uint32_t)(re[a] - re[b]));
                di = w16((uint32_t)(im[a] - im[b]));
                re[a] = w16((uint32_t)(re[a] + re[b])) >> 1;
                im[a] = w16((uint32_t)(im[a] + im[b])) >> 1;
                twiddle(&re[b], &im[b], dr, di, j * (16 / (2 * h)));
            }
}

struct blk
{
    int16_t r[16][16], i[16][16];       /* [row][lane] */
};

struct work
{
    struct blk z, y;
};

/* X[k] / m into the work's y (row k2, lane k1, k = k2 + rows k1), over
 * the tables t (the size's own, or any: the start-up check's) */
static void fft(struct work *w, const int16_t *pcm, uint32_t n,
                const struct jviz_tabs *t)
{
    uint32_t rows = t->rows, m = n / 2, bits = rows == 16 ? 4 : 3, r, l, i, k2, k1, p;
    struct blk *z = &w->z, *y = &w->y;
    int32_t ar, ai, br, bi, s, d, tt, u, wr, wi;

    /* the window: x = pcm * h >> 16 (vmulhd) */
    for (r = 0; r < rows; r++)
        for (l = 0; l < 16; l++)
        {
            z->r[r][l] = (pcm[16 * r + l] * t->win[16 * r + l]) >> 16;
            z->i[r][l] = (pcm[m + 16 * r + l] * t->win[m + 16 * r + l]) >> 16;
        }
    /* step 1: down the rows, every lane at once; the last stage (h = 1)
       unscaled, to y in natural order, its halving left to the twiddle */
    for (l = 0; l < 16; l++)
        stages(&z->r[0][l], &z->i[0][l], 16, rows);
    for (i = 0; i < rows; i += 2)
        for (l = 0; l < 16; l++)
        {
            ar = z->r[i][l];
            ai = z->i[i][l];
            br = z->r[i + 1][l];
            bi = z->i[i + 1][l];
            y->r[brev(i, bits)][l] = w16((uint32_t)(ar + br));
            y->i[brev(i, bits)][l] = w16((uint32_t)(ai + bi));
            y->r[brev(i + 1, bits)][l] = w16((uint32_t)(ar - br));
            y->i[brev(i + 1, bits)][l] = w16((uint32_t)(ai - bi));
        }
    /* step 2: z = y * W_m^(n1 k2), floored by 2^16 (the last halving) */
    for (r = 0; r < rows; r++)
        for (l = 0; l < 16; l++)
        {
            wr = t->tw[16 * r + l];
            wi = t->tw[16 * (rows + r) + l];
            z->r[r][l] = hi16(mul(y->r[r][l], wr) + mul(y->i[r][l], w16(0u - (uint32_t)wi)));
            z->i[r][l] = hi16(mul(y->r[r][l], wi) + mul(y->i[r][l], wr));
        }
    /* step 3: along the lanes, every row; the last stage halving, to y in
       natural order */
    for (r = 0; r < rows; r++)
    {
        stages(z->r[r], z->i[r], 1, 16);
        for (i = 0; i < 16; i += 2)
        {
            ar = z->r[r][i];
            ai = z->i[r][i];
            br = z->r[r][i + 1];
            bi = z->i[r][i + 1];
            y->r[r][brev(i, 4)] = w16((uint32_t)(ar + br)) >> 1;
            y->i[r][brev(i, 4)] = w16((uint32_t)(ai + bi)) >> 1;
            y->r[r][brev(i + 1, 4)] = w16((uint32_t)(ar - br)) >> 1;
            y->i[r][brev(i + 1, 4)] = w16((uint32_t)(ai - bi)) >> 1;
        }
    }
    /* the split step into z: X = (S 2^15 + Wr T + Wi D) / 2^16 + i (U 2^15
       + Wi T - Wr D) / 2^16, Z's partner Z[m - k] at row rows - k2, lane
       15 - k1 (row 0: lane -k1) */
    for (k2 = 0; k2 < rows; k2++)
        for (k1 = 0; k1 < 16; k1++)
        {
            p = k2 ? (rows - k2) : 0;
            i = k2 ? 15 - k1 : (16 - k1) & 15;
            s = w16((uint32_t)(y->r[k2][k1] + y->r[p][i]));
            d = w16((uint32_t)(y->r[k2][k1] - y->r[p][i]));
            tt = w16((uint32_t)(y->i[k2][k1] + y->i[p][i]));
            u = w16((uint32_t)(y->i[k2][k1] - y->i[p][i]));
            wr = t->ws[16 * k2 + k1];
            wi = t->ws[16 * (rows + k2) + k1];
            z->r[k2][k1] = hi16(((uint32_t)s << 15) + mul(tt, wr) + mul(d, wi));
            z->i[k2][k1] = hi16(((uint32_t)u << 15) + mul(tt, wi) +
                                mul(d, w16(0u - (uint32_t)wr)));
        }
    (void)m;
}

#ifndef JVIZ_OVL                /* the tests' entry points (jviz_fft_ref,
                                   jviz_raw_ref): not in the overlay */
void jviz_fft_ref(int16_t *xr, int16_t *xi, const int16_t *pcm, uint32_t n)
{
    struct work w;
    uint32_t sh = n == 256 ? 3 : 4, k;

    fft(&w, pcm, n, tabs(n));
    for (k = 0; k < n / 2; k++)
    {
        xr[k] = w.z.r[k & ((1u << sh) - 1)][k >> sh];
        xi[k] = w.z.i[k & ((1u << sh) - 1)][k >> sh];
    }
}
#endif

void jviz_raw_ref_t(uint16_t *raw, const int16_t *pcm, uint32_t n,
                    const struct jviz_tabs *t)
{
    struct work w;
    uint32_t rows = t->rows, r, l, p;
    int32_t a, b;

    fft(&w, pcm, n, t);
    for (r = 0; r < 16; r++)
        for (l = 0; l < 16; l++)
        {
            p = 0;
            if (r < rows)
            {
                a = w.z.r[r][l];
                b = w.z.i[r][l];
                p = mul(a, a) + mul(b, b);
            }
            raw[16 * r + l] = p & 0xFFFF;
            raw[256 + 16 * r + l] = p >> 16;
        }
}

#ifndef JVIZ_OVL
void jviz_raw_ref(uint16_t *raw, const int16_t *pcm, uint32_t n)
{
    jviz_raw_ref_t(raw, pcm, n, tabs(n));
}
#endif

void jviz_power_ref(uint32_t *p, const int16_t *pcm, uint32_t n)
{
    struct work w;
    uint32_t sh = n == 256 ? 3 : 4, k;
    int32_t a, b;

#ifdef JVIZ_N256_ONLY
    if (n != 256)
    {
        for (k = 0; k < n / 2; k++)     /* a size this build lacks */
            p[k] = 0;
        return;
    }
#endif
    fft(&w, pcm, n, tabs(n));
    for (k = 0; k < n / 2; k++)
    {
        a = w.z.r[k & ((1u << sh) - 1)][k >> sh];
        b = w.z.i[k & ((1u << sh) - 1)][k >> sh];
        p[k] = mul(a, a) + mul(b, b);
    }
}

void jviz_raw_power(uint32_t *p, const uint16_t *raw, uint32_t n)
{
    uint32_t sh = n == 256 ? 3 : 4, k, at;

    for (k = 0; k < n / 2; k++)
    {
        at = 16 * (k & ((1u << sh) - 1)) + (k >> sh);
        p[k] = raw[at] | (uint32_t)raw[256 + at] << 16;
    }
}

uint32_t jviz_l2(uint32_t p)
{
    uint32_t e = 31, f;

    if (p < 2)
        return 0;
    while (!(p >> e))
        e--;
    f = e >= 4 ? p >> (e - 4) : p << (4 - e);
    return 16 * e + jviz_log16[f & 15];
}

void jviz_bands(uint16_t *l2, const uint32_t *p, uint32_t m, const uint16_t *e,
                uint32_t nb)
{
    uint32_t b, k, hi, top;

    for (b = 0; b < nb; b++)
    {
        top = 0;
        hi = e[b + 1] < m ? e[b + 1] : m;
        for (k = e[b]; k < hi; k++)
            if (p[k] > top)
                top = p[k];
        l2[b] = jviz_l2(top);
    }
}

/* ---- the bars ---------------------------------------------------------- */

/* one channel of the gradient: a + (b - a) * f / 256, floored */
static uint32_t lerp(uint32_t a, uint32_t b, uint32_t f)
{
    return (uint32_t)((int32_t)a + (((int32_t)b - (int32_t)a) * (int32_t)f >> 8));
}

uint32_t jviz_mix565(uint32_t c0, uint32_t c1, uint32_t f)
{
    return lerp(c0 >> 11, c1 >> 11, f) << 11 | lerp(c0 >> 5 & 63, c1 >> 5 & 63, f) << 5 |
           lerp(c0 & 31, c1 & 31, f);
}

#ifdef __GNUC__
typedef uint32_t __attribute__((may_alias)) jviz_u32;
#else
typedef uint32_t jviz_u32;
#endif

/* n pixels of c from d: two to a word where it can, eight words a turn */
static void span(uint16_t *d, uint32_t n, uint32_t c)
{
    uint32_t cc = c | c << 16;
    jviz_u32 *w;

    if (n && ((uintptr_t)d & 2))
    {
        *d++ = c;
        n--;
    }
    for (w = (jviz_u32 *)d; n >= 16; n -= 16, w += 8)
    {
        w[0] = cc; w[1] = cc; w[2] = cc; w[3] = cc;
        w[4] = cc; w[5] = cc; w[6] = cc; w[7] = cc;
    }
    for (; n >= 2; n -= 2)
        *w++ = cc;
    if (n)
        *(uint16_t *)w = c;
}

/* rows h0 .. h1 - 1 of the w-wide column at d in colour c: the fill hook
 * for what it takes, the rest here */
static void rect(uint16_t *d, uint32_t pitch, uint32_t w, uint32_t rows, uint32_t c,
                 jviz_fill_fn fill)
{
    uint32_t sn = fill && rows ? fill(d, pitch, w, rows, c) : 0, lo, hi, y;

    lo = sn ? sn & 0xFFFF : w;
    hi = sn ? sn >> 16 : w;
    for (y = 0; y < rows; y++, d += pitch)
    {
        span(d, lo, c);
        span(d + hi, w - hi, c);
    }
}

int jviz_bars_draw(uint16_t *px, uint32_t pitch, uint32_t w, uint32_t h,
                   const struct jviz_bars *b, jviz_fill_fn fill)
{
    uint32_t total, x0, i, t, inv, hh, x, top, ch, rows;
    uint16_t col[256], *p, *r0 = px;
    int32_t step = (int32_t)pitch;

    if (!b->n || !b->barw || b->n > JVIZ_BANDS_MAX || !h)
        return -1;
    total = b->n * b->barw + (b->n - 1) * b->gap;
    if (total > w)
        return -1;
    x0 = (w - total) / 2;
    if (b->flags & JVIZ_BARS_BG)
        rect(px, pitch, w, h, b->bg, fill);
    /* JVIZ_BARS_FLIP: the same rows, addressed bottom up (no extra pass) */
    if (b->flags & JVIZ_BARS_FLIP)
    {
        r0 = px + (h - 1) * pitch;
        step = -step;
    }
    /* the gradient's colour of row t from the bottom (bars are at most
       255 rows high): f = t * 256 / h as t * inv >> 16, one division */
    inv = (256u << 16) / h;
    rows = h < 256 ? h : 256;
    for (t = 0; t < rows; t++)
        col[t] = jviz_mix565(b->bot, b->top, (t * inv) >> 16);
    /* bar by bar, bottom up (the bars never share a pixel) */
    for (i = 0, x = x0; i < b->n; i++, x += b->barw + b->gap)
    {
        hh = b->height[i] < h ? b->height[i] : h;
        p = r0 + (int32_t)(h - 1) * step + x;
        for (t = 0; t < hh; t++, p -= step)
        {
            if (t == hh - 1 && (b->flags & JVIZ_BARS_ROUND) && hh >= 2 && b->barw >= 3)
                span(p + 1, b->barw - 2, col[t]);
            else
                span(p, b->barw, col[t]);
        }
    }
    if (b->flags & JVIZ_BARS_CAP)
        for (i = 0, x = x0; i < b->n; i++, x += b->barw + b->gap)
        {
            if (!b->peak[i])
                continue;
            top = b->peak[i] < h ? h - b->peak[i] : 0;
            ch = b->cap_h < h - top ? b->cap_h : h - top;
            for (t = 0, p = r0 + (int32_t)top * step + x; t < ch; t++, p += step)
                span(p, b->barw, b->cap);
        }
    return 0;
}

/* ---- Scope, Radial and Spectrogram (jviz.h) ---------------------------- */

/* rows lo .. hi of the w x h rect's column c, clipped, in colour */
static void vspan(uint16_t *px, uint32_t pitch, uint32_t h, int32_t lo, int32_t hi,
                  uint32_t colour)
{
    uint16_t *p;

    if (lo < 0)
        lo = 0;
    if (hi >= (int32_t)h)
        hi = (int32_t)h - 1;
    for (p = px + lo * pitch; lo <= hi; lo++, p += pitch)
        *p = colour;
}

/* the point's value, as a row of the rect (clamped) */
static int32_t poly_row(const struct jviz_poly *p, uint32_t e, uint32_t h)
{
    int32_t y;

    if (p->half)
        e = (e & 1) ? p->half + (e >> 1) : e >> 1;
    if (p->flags & JVIZ_POLY_U8)
        y = ((const uint8_t *)p->src)[e];
    else
        y = (int32_t)(h >> 1) - ((((const int16_t *)p->src)[e] * (int32_t)p->gain) >> 16);
    return y < 0 ? 0 : y >= (int32_t)h ? (int32_t)h - 1 : y;
}

/* column c's rows take in a .. b */
static void cover(int16_t *lo, int16_t *hi, uint32_t c, int32_t a, int32_t b)
{
    int32_t t;

    if (a > b)
    {
        t = a;
        a = b;
        b = t;
    }
    if (a < lo[c])
        lo[c] = a;
    if (b > hi[c])
        hi[c] = b;
}

int jviz_poly_draw(uint16_t *px, uint32_t pitch, uint32_t w, uint32_t h,
                   const struct jviz_poly *p)
{
    int16_t lo[JVIZ_POLY_W_MAX], hi[JVIZ_POLY_W_MAX];
    uint32_t i, c, x0, x1, q, r, acc, W = p->width;
    int32_t y0, y1, dy, l, u;

    if (p->n < 2 || p->n > JVIZ_POLY_N_MAX || W < 1 || W > 3 || !w ||
        w > JVIZ_POLY_W_MAX || !h || p->gain > 0xFFFF || p->off > 0xFFFF ||
        p->step > 0xFFFF || p->half > 0xFFFF)
        return -1;
    /* the last element read lies inside src (split: past any odd one's) */
    i = p->off + (p->n - 1) * p->step;
    if ((p->half ? p->half + (i >> 1) : i) >= p->len)
        return -1;
    for (c = 0; c < w; c++)
    {
        lo[c] = 0x7FFF;
        hi[c] = -1;
    }
    /* column of point i: floor(i (w - 1) / (n - 1)), stepped */
    q = (w - 1) / (p->n - 1);
    r = (w - 1) % (p->n - 1);
    x0 = acc = 0;
    y0 = poly_row(p, p->off, h);
    for (i = 1; i < p->n; i++, x0 = x1, y0 = y1)
    {
        x1 = x0 + q;
        acc += r;
        if (acc >= p->n - 1)
        {
            acc -= p->n - 1;
            x1++;
        }
        y1 = poly_row(p, p->off + i * p->step, h);
        if (x1 <= x0 + 1)       /* the common case: no division */
            cover(lo, hi, x0, y0, y1);
        else
            for (c = x0, dy = y1 - y0; c < x1; c++)
                cover(lo, hi, c, y0 + dy * (int32_t)(c - x0) / (int32_t)(x1 - x0),
                      y0 + dy * (int32_t)(c + 1 - x0) / (int32_t)(x1 - x0));
    }
    cover(lo, hi, w - 1, y0, y0);
    /* the brush: column c takes columns c - W / 2 .. c + (W - 1) / 2 */
    for (c = 0; c < w; c++)
    {
        l = lo[c];
        u = hi[c];
        for (i = c >= W / 2 ? c - W / 2 : 0; i <= c + (W - 1) / 2 && i < w; i++)
        {
            if (lo[i] < l)
                l = lo[i];
            if (hi[i] > u)
                u = hi[i];
        }
        vspan(px + c, pitch, h, l - (int32_t)(W - 1) / 2, u + (int32_t)W / 2, p->colour);
    }
    return 0;
}

/* Q14 sin over a quarter wave, 0 .. 90 degrees in 7.5 degree steps */
static const int16_t sin48q[13] =
{
    0, 2139, 4240, 6270, 8192, 9974, 11585, 12998, 14189, 15137, 15826, 16244, 16384
};

int32_t jviz_sin48(uint32_t k)
{
    uint32_t r;
    int32_t v;

    k %= 48;
    r = k % 24;
    v = sin48q[r <= 12 ? r : 24 - r];
    return k < 24 ? v : -v;
}

/* a rect (jviz_segs_draw's dots) */
struct vrect
{
    uint16_t *px;
    int32_t pitch, w, h;
};

/* the d x d square (or rounded) at (x, y) of the rect: offsets as the
 * polyline's brush */
static __attribute__((noinline)) void dot(const struct vrect *v, int32_t x, int32_t y,
                                          int32_t d, uint32_t round, uint32_t colour)
{
    int32_t j, a, b, in;
    uint16_t *p;

    x -= (d - 1) / 2;
    y -= (d - 1) / 2;
    p = v->px + y * v->pitch;
    if (!round && x >= 0 && y >= 0 && x + d <= v->w && y + d <= v->h)
    {
        for (p += x, j = 0; j < d; j++, p += v->pitch)  /* inside: no clip */
            for (a = 0; a < d; a++)
                p[a] = colour;
        return;
    }
    for (j = 0; j < d; j++, p += v->pitch)
    {
        if (y + j < 0 || y + j >= v->h)
            continue;
        in = round && d >= 3 && (j == 0 || j == d - 1);
        a = x + in;
        b = x + d - 1 - in;
        if (a < 0)
            a = 0;
        if (b >= v->w)
            b = v->w - 1;
        for (; a <= b; a++)
            p[a] = colour;
    }
}

/* the point at radius r on angle index k, from (cx, cy) */
static int32_t polar(int32_t c, uint32_t r, int32_t q14)
{
    return c + (((int32_t)r * q14 + 8192) >> 14);
}

int jviz_segs_draw(uint16_t *px, uint32_t pitch, uint32_t w, uint32_t h,
                   const struct jviz_segs *s)
{
    uint16_t col[256];
    uint32_t i, k, t, L, inv, r, d, rd, c, pk;
    int32_t sn, cs;
    struct vrect v = { px, (int32_t)pitch, (int32_t)w, (int32_t)h };

    if (!s->n || s->n > JVIZ_SEG_MAX || s->a0 > 47 || s->astep > 47 || s->width < 1 ||
        s->width > 3 || !s->lmax || s->lmax > 255 || s->capd > 15 || s->ringd > 15)
        return -1;
    inv = (256u << 16) / s->lmax;
    for (t = 0; t < s->lmax; t++)
        col[t] = jviz_mix565(s->c0, s->c1, (t * inv) >> 16);
    for (k = 0; k < 3; k++)                 /* the ring, the segments, the caps */
        for (i = 0; i < s->n; i++)
        {
            sn = jviz_sin48(s->a0 + i * s->astep);
            cs = -jviz_sin48(s->a0 + i * s->astep + 12);
            pk = s->peak[i] < s->lmax ? s->peak[i] : s->lmax;
            rd = s->flags & JVIZ_SEG_ROUND;
            L = 1;
            if (k == 0)                     /* the ring's dot */
            {
                r = s->rring;
                d = s->ringd;
                c = s->ring;
                if (!(s->flags & JVIZ_SEG_RING))
                    L = 0;
            }
            else if (k == 1)                /* the segment */
            {
                r = s->r0;
                d = s->width;
                c = 0;
                rd = 0;
                L = s->len[i] < s->lmax ? s->len[i] : s->lmax;
            }
            else                            /* the cap */
            {
                r = s->r0 + pk;
                d = s->capd;
                c = s->cap;
                if (!(s->flags & JVIZ_SEG_CAP) || !pk)
                    L = 0;
            }
            for (t = 0; t < L; t++)
                dot(&v, polar(s->cx, r + t, sn), polar(s->cy, r + t, cs), d, rd,
                    k == 1 ? col[t] : c);
        }
    return 0;
}

int jviz_pal_draw(uint16_t *px, uint32_t pitch, uint32_t w, uint32_t h,
                  const struct jviz_pal *p)
{
    uint32_t b, y, xa, xb;

    if (!p->nb || p->nb > JVIZ_PAL_MAX || p->nb > w)
        return -1;
    for (b = 0, xa = 0; b < p->nb; b++, xa = xb)
    {
        xb = (b + 1) * w / p->nb;
        for (y = 0; y < h; y++)
            span(px + y * pitch + xa, xb - xa, p->lut[p->lv[b]]);
    }
    return 0;
}

#ifndef JVIZ_N256_ONLY
/* jviz.h jviz_draw_check: a fixed scene per op (clipping at every edge,
 * every width, both sources, split order, n under and over the width,
 * rounded dots, the ring and the caps), its pixels' FNV-1a.  The scenes
 * are data (src: 0 the samples, 1 the bytes), the surface's offset first */
struct chk_poly
{
    uint16_t at, w, h, src;
    struct jviz_poly p;
};

struct chk_segs
{
    uint16_t at, w, h;
    struct jviz_segs g;
};

static const struct chk_poly chk_poly[3] =
{
    { 2 * JVIZ_CHK_W + 3, 33, 25, 0, { 0, 64, 33, 1, 1, 32, 700, 3, 0, 0x07FF } },
    /* more points than columns */
    { 2 * JVIZ_CHK_W + 3, 33, 25, 0, { 0, 64, 50, 0, 1, 0, 300, 1, 0, 0xF800 } },
    /* bytes, fewer points than columns */
    { JVIZ_CHK_W, JVIZ_CHK_W, JVIZ_CHK_H - 1, 1,
      { 0, 48, 7, 3, 5, 0, 0, 2, JVIZ_POLY_U8, 0xFFE0 } },
};

static const struct chk_segs chk_segs[2] =
{
    { JVIZ_CHK_W + 1, JVIZ_CHK_W - 2, JVIZ_CHK_H - 2,
      { 16, 5, 3, 3, JVIZ_SEG_CAP | JVIZ_SEG_RING | JVIZ_SEG_ROUND, 3, 20, 2, 3, 1,
        19, 14, 0x0841, 0xF81F, 0xFFFF, 0x4208, 0, 0 } },
    { 0, JVIZ_CHK_W, JVIZ_CHK_H,
      { 48, 0, 1, 1, JVIZ_SEG_CAP, 0, 9, 2, 2, 1, 35, 3, 0x0841, 0xF81F, 0xFFFF, 0x4208,
        0, 0 } },
};

/* a scene's struct, word by word (a struct copy would link memcpy) */
static void copyw(void *d, const void *s, uint32_t bytes)
{
    jviz_u32 *dw = d;
    const jviz_u32 *sw = s;

    for (bytes /= 4; bytes; bytes--)
        *dw++ = *sw++;
}

uint32_t jviz_draw_check(uint16_t *s, uint32_t op)
{
    int16_t pcm[64];
    uint8_t b8[48], b8b[48];
    uint16_t lut[256];
    uint32_t i, x = 0x2545F491, h = 0x811C9DC5;
    struct jviz_poly p;
    struct jviz_segs g;
    struct jviz_pal pl;

    for (i = 0; i < 256; i++)
    {
        x = x * 1103515245u + 12345;
        lut[i] = x >> 16;
        if (i < 64)
            pcm[i] = (int16_t)(x >> 8);
        if (i < 48)
        {
            b8[i] = x >> 3;
            b8b[i] = x >> 11;
        }
    }
    for (i = 0; i < JVIZ_CHK_W * JVIZ_CHK_H; i++)
        s[i] = 0x18E3;
    if (op == 0)
        for (i = 0; i < 3; i++)
        {
            copyw(&p, &chk_poly[i].p, sizeof(p));
            p.src = chk_poly[i].src ? (const void *)b8 : (const void *)pcm;
            jviz_poly_draw(s + chk_poly[i].at, JVIZ_CHK_W, chk_poly[i].w, chk_poly[i].h, &p);
        }
    else if (op == 1)
    {
        for (i = 0; i < 48; i++)
            b8[i] &= 31;
        for (i = 0; i < 2; i++)
        {
            copyw(&g, &chk_segs[i].g, sizeof(g));
            g.len = b8;
            g.peak = b8b;
            jviz_segs_draw(s + chk_segs[i].at, JVIZ_CHK_W, chk_segs[i].w, chk_segs[i].h, &g);
        }
    }
    else
    {
        pl.lut = lut;
        pl.lv = b8;
        for (i = 0; i < 2; i++)
        {
            pl.nb = i ? 37 : 10;
            jviz_pal_draw(s + (3 + 6 * i) * JVIZ_CHK_W + 1, JVIZ_CHK_W, 37, 4 - 3 * i, &pl);
        }
    }
    for (i = 0; i < JVIZ_CHK_W * JVIZ_CHK_H; i++)
        h = ((h ^ (s[i] & 0xFF)) * 16777619u ^ (s[i] >> 8)) * 16777619u;
    return h;
}
#endif
