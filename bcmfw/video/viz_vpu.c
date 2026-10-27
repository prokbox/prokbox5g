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

/* The visualizer's FFT and bars on the vector unit (proposed, sim only;
 * #126, docs/research/bcm2722/visualizer.md): jref's spectrum hook
 * (jref.h, JREF_VIZ) over viz_kernels.S's jv_fft256 and jv_fft512
 * (gen_viz.py), bit for bit jviz.c's reference; jref's bars hook over
 * jv_bars16, pixel for pixel jviz_bars_draw.  The image installs each
 * only if its start-up check agrees with the C (jobs.h J_VPU_FFT,
 * J_VPU_BARS): the kernels' forms are unproven on silicon, and the C
 * gives the same levels and pixels, only slower. */
#include <stdint.h>
#include "config.h"
#include "jviz.h"
#include "viz_vpu.h"

void jv_fft256(uint16_t *raw, const int16_t *pcm, const struct jviz_tabs *t);
void jv_fft512(uint16_t *raw, const int16_t *pcm, const struct jviz_tabs *t);

/* The kernel's raw output and the start-up check's samples live on the
 * stack (16 KB; the deepest path, the FFT's check, takes about 6 KB), not
 * in .bss: the overlay's region keeps the 2 KB for the line ops
 * (visualizer.md section 13).  A 32-byte aligned buffer of n halfwords
 * inside one of n + 16 */
#define ALIGN32(b)  ((void *)(((uint32_t)(b) + 31) & ~31u))

int viz_spectrum(uint32_t *p, const int16_t *pcm, uint32_t n)
{
    uint16_t buf[JVIZ_RAW_HW + 16], *raw = ALIGN32(buf);

    if ((uint32_t)pcm & 31)
        return 0;                   /* a slot always is; else the C */
    if (n == 256)
        jv_fft256(raw, pcm, jviz_tabs(256));
    else
        jv_fft512(raw, pcm, jviz_tabs(512));
    jviz_raw_power(p, raw, n);
    return 1;
}

/* the kernel's raw powers for pcm over the tables t against the C's, every
 * bin (the rows past n / 32 are the kernel's scratch) */
static int agrees(const int16_t *pcm, uint32_t n, const struct jviz_tabs *t, int fail)
{
    uint16_t ref[JVIZ_RAW_HW], buf[JVIZ_RAW_HW + 16], *raw = ALIGN32(buf);
    uint32_t k;

    if (n == 256)
        jv_fft256(raw, pcm, t);
    else
        jv_fft512(raw, pcm, t);
    jviz_raw_ref_t(ref, pcm, n, t);
    for (k = 0; k < n / 2; k++)         /* the low rows, the high */
        if (raw[k] != (uint16_t)(ref[k] + fail) || raw[256 + k] != ref[256 + k])
            return 0;
    return 1;
}

/* The start-up check, at each size:
 *  - a full-scale square wave (clipping) and pseudo-random noise, the
 *    size's own tables;
 *  - the worst case for the accumulator: every sample 32767 and every
 *    table word -32768 (a window and twiddles past |W| <= 1).  Each sample
 *    windows to -16,384, step 1 sums row 0 to (-32768, -32768), and step
 *    2's two products there are 2^30 each: their sum, 2^31, is where C
 *    wraps and silicon's accumulator saturates (quirks #31, #32), so a
 *    kernel that sums them in one chain fails here under every rule that
 *    fits silicon (gen_viz.py chain(); viz_test.py runs each) */
int viz_vpu_init(int fail)
{
    int16_t pbuf[JVIZ_N_MAX + 16], *pcm = ALIGN32(pbuf);
    /* the worst case's table; in the overlay, first 256's derived tables
       (jviz.h jviz_derive256) */
#ifdef JVIZ_OVL
    int16_t tab[JVIZ_TAB256_HW + 16], *t = ALIGN32(tab);
#else
    int16_t tab[JVIZ_N_MAX + 16], *t = ALIGN32(tab);
#endif
    struct jviz_tabs worst;
    uint32_t n, s, i, x = 0x12345678;

    worst.win = worst.tw = worst.ws = t;
    for (n = JVIZ_N_MIN; n <= JVIZ_N_MAX; n *= 2)
        for (s = 0; s < 3; s++)
        {
            for (i = 0; i < n; i++)
            {
                x = x * 1103515245u + 12345;
                pcm[i] = s == 1 ? (int16_t)(x >> 16) :
                         s == 2 || (i / 8 + i) & 4 ? 32767 : -32768;
            }
#ifdef JVIZ_OVL
            if (n == 256 && s == 0)
                jviz_derive256(t);
#endif
            if (s == 2)
                for (i = 0; i < JVIZ_N_MAX; i++)
                    t[i] = -32768;
            worst.rows = n / 32;
            if (!agrees(pcm, n, s == 2 ? &worst : jviz_tabs(n), fail))
                return 0;
        }
    return 1;
}

/* ---- BARS on the vector unit (jv_bars16, gen_viz.py) ------------------ *
 * The frame as 16 x 16 blocks of the surface: 16-row groups top down (the
 * last moved up to end on the rect's last row, so it overlaps the one
 * before: every pixel's colour depends only on where it is, so drawing a
 * row twice gives it the same value), and in each group the 16-pixel
 * column blocks from the one holding the rect's first pixel.  Each pixel
 * is the cap's colour if its row is in its column's cap rows, else the
 * gradient's colour of its row if the row is at or below its column's
 * first bar row, else bg (J_BARS_BG, inside the rect), else unchanged
 * (the lanes outside the rect, and the rest without J_BARS_BG): exactly
 * jviz_bars_draw's, whose bars never share a pixel and whose caps are
 * drawn last.  The C does the geometry once a frame (per column: its
 * first bar row and cap rows; per block: their bounds and the background
 * mask), the kernel the pixels. */

/* jv_bars16's job: gen_viz.py reads these words at these offsets */
struct jv_bars_job
{
    uint32_t pitch;             /* in bytes */
    uint32_t ncb;               /* column blocks */
    const uint16_t *lanes;      /* each lane's first bar row; its cap's first
                                   and last rows VIZ_BARS_CB_MAX * 32 and
                                   twice that many bytes on */
    const uint32_t *desc;       /* per block: the least first bar row, the
                                   groups (bits) holding a bar's top, those
                                   holding cap rows, the background's lane
                                   mask (| 0x10000 for every lane) */
    uint32_t bg, cap;
    int32_t grad[6];            /* bot's red, top's less bot's, green, blue */
};

void jv_bars16(const struct jv_bars_job *job, uint16_t *d, uint32_t rr,
               const uint16_t *rows, uint32_t gbit);

#define LANES       (16 * VIZ_BARS_CB_MAX)
#define NONE        0x4000      /* a first row past any rect's rows */

/* n halfwords of v from p, two to a word (p and n even) */
static __attribute__((noinline)) void set16(uint16_t *p, uint32_t n, uint32_t v)
{
    uint32_t *w = (uint32_t *)p, *e = w + n / 2;

    v |= v << 16;
    while (w < e)
        *w++ = v;
}

/* n lanes from p: the first and the last v1, the rest v */
static __attribute__((noinline)) void lanes(uint16_t *p, uint32_t n, uint32_t v, uint32_t v1)
{
    uint32_t u;

    for (u = 1; u + 1 < n; u++)
        p[u] = v;
    p[0] = v1;
    p[n - 1] = v1;
}

/* The groups (bits) whose rows hold row v and row v - 1, of h rows: the
 * regular groups (rows 16 g on) and, if h isn't whole groups, the last
 * (rows h - 16 on) */
static uint32_t top_groups(uint32_t v, uint32_t h)
{
    uint32_t g = 0;

    if (!v || v >= h)
        return 0;
    if ((v & 15) && (v >> 4) < (h >> 4))
        g = 1u << (v >> 4);
    if ((h & 15) && v > h - 16)
        g |= 1u << (h >> 4);
    return g;
}

/* the groups holding any of rows v0 .. v1 */
static __attribute__((noinline)) uint32_t span_groups(uint32_t v0, uint32_t v1, uint32_t h)
{
    uint32_t g = 0, k;

    for (k = v0 >> 4; k <= v1 >> 4 && k < (h >> 4); k++)
        g |= 1u << k;
    if ((h & 15) && v1 >= h - 16)
        g |= 1u << (h >> 4);
    return g;
}

int viz_bars(uint16_t *px, uint32_t pitch, uint32_t x, uint32_t w, uint32_t h,
             const struct jviz_bars *b)
{
    uint16_t buf[3 * LANES + 32 + 16], *bst, *cs, *ce, *rows;
    uint32_t desc[4 * VIZ_BARS_CB_MAX], *dc;
    uint32_t lo = ((uint32_t)px >> 1) & 15, ncb, total, a, i, c, m, hh, s0, s1, top, ch;
    uint32_t tg, cg, inv, rr, k, g, r, fl;
    struct jv_bars_job job;

    if ((pitch & 15) || ((uint32_t)(px - x) & 31) || h < 16 || h > VIZ_BARS_ROWS_MAX ||
        !b->n || !b->barw || b->n > JVIZ_BANDS_MAX)
        return 0;
    total = b->n * b->barw + (b->n - 1) * b->gap;
    ncb = (lo + w + 15) >> 4;
    if (total > w || ncb > VIZ_BARS_CB_MAX)
        return 0;                   /* the C's (or its error) */
    bst = (uint16_t *)(((uint32_t)buf + 31) & ~31u);
    cs = bst + LANES;
    ce = cs + LANES;
    rows = ce + LANES;
    set16(bst, 16 * ncb, NONE);
    if (b->flags & JVIZ_BARS_CAP)
        set16(cs, 16 * ncb, NONE);  /* else never read: no block has caps */
    for (i = 0, dc = desc; i < ncb; i++, dc += 4)
    {
        m = 0;
        if (b->flags & JVIZ_BARS_BG)
        {
            m = 0xFFFF;
            if (i == 0)
                m &= 0xFFFF << lo;
            if (i == ncb - 1)
                m &= 0xFFFF >> (16 * ncb - lo - w);
            if (m == 0xFFFF)
                m |= 0x10000;
        }
        dc[0] = NONE;
        dc[1] = 0;
        dc[2] = 0;
        dc[3] = m;
    }
    for (i = 0, a = lo + (w - total) / 2; i < b->n; i++, a += b->barw + b->gap)
    {
        hh = b->height[i] < h ? b->height[i] : h;
        s0 = h - hh;
        s1 = s0 + ((b->flags & JVIZ_BARS_ROUND) && hh >= 2 && b->barw >= 3);
        lanes(bst + a, b->barw, s0, s1);
        tg = top_groups(s0, h) | top_groups(s1, h);
        top = b->peak[i] < h ? h - b->peak[i] : 0;
        ch = b->cap_h < h - top ? b->cap_h : h - top;
        cg = 0;
        if ((b->flags & JVIZ_BARS_CAP) && b->peak[i] && ch)
        {
            lanes(cs + a, b->barw, top, top);
            lanes(ce + a, b->barw, top + ch - 1, top + ch - 1);
            cg = span_groups(top, top + ch - 1, h);
        }
        for (c = a >> 4; c <= (a + b->barw - 1) >> 4; c++)
        {
            dc = desc + 4 * c;
            if (s0 < dc[0])
                dc[0] = s0;
            dc[1] |= tg;
            dc[2] |= cg;
        }
    }
    job.pitch = 2 * pitch;
    job.ncb = ncb;
    job.lanes = bst;
    job.desc = desc;
    job.bg = b->bg;
    job.cap = b->cap;
    job.grad[0] = b->bot >> 11;
    job.grad[1] = (int32_t)(b->top >> 11) - (int32_t)(b->bot >> 11);
    job.grad[2] = b->bot >> 5 & 63;
    job.grad[3] = (int32_t)(b->top >> 5 & 63) - (int32_t)(b->bot >> 5 & 63);
    job.grad[4] = b->bot & 31;
    job.grad[5] = (int32_t)(b->top & 31) - (int32_t)(b->bot & 31);
    /* per group: f of its rows (row t from the bottom: jviz_bars_draw's
       t * 256 / h) and their indices in the rect.  JVIZ_BARS_FLIP by
       addressing alone: the group's rows rr .. rr + 15 are the surface's
       rows h - 16 - rr .. h - 1 - rr, so the kernel gets that block (rows
       top down, 32-byte aligned, the same positive stride) with its row k
       given the rect's row rr + 15 - k (f and index); the kernel itself
       is unchanged, its masks per row and its group bounds over the same
       set of rows */
    inv = (256u << 16) / h;
    fl = (b->flags & JVIZ_BARS_FLIP) != 0;
    for (rr = 0, g = 1; rr < h; rr += 16, g <<= 1)
    {
        if (rr + 16 > h)
            rr = h - 16;
        for (k = 0; k < 16; k++)
        {
            r = fl ? rr + 15 - k : rr + k;
            rows[k] = ((h - 1 - r) * inv) >> 16;
            rows[16 + k] = r;
        }
        jv_bars16(&job, px - lo + (fl ? h - 16 - rr : rr) * pitch, rr, rows, g);
    }
    return 1;
}

/* The start-up check: five frames on a 32 x 18 surface, the kernel's
 * against jviz_bars_draw's, every pixel of the surface: a rect at x 3 (two
 * partial blocks) with every flag, the same without the background (the
 * lanes kept), and the whole width (whole blocks, no load); then the
 * first two flipped (JVIZ_BARS_FLIP: the rows addressed bottom up); two
 * groups that overlap; heights 0, 1 and full, peaks above the rect, at
 * its top and over a bar of 0, rounded tops, a two-row cap */
int viz_bars_init(int fail)
{
    static const uint8_t ht[4] = { 0, 18, 7, 1 }, pk[4] = { 3, 18, 20, 1 };
    static const uint8_t geo[5][3] = {
        { 3, 27, JVIZ_BARS_BG | JVIZ_BARS_CAP | JVIZ_BARS_ROUND },
        { 3, 27, JVIZ_BARS_CAP | JVIZ_BARS_ROUND },
        { 0, 32, JVIZ_BARS_BG | JVIZ_BARS_ROUND },
        { 3, 27, JVIZ_BARS_BG | JVIZ_BARS_CAP | JVIZ_BARS_ROUND | JVIZ_BARS_FLIP },
        { 3, 27, JVIZ_BARS_CAP | JVIZ_BARS_ROUND | JVIZ_BARS_FLIP } };
    uint16_t buf[2 * 32 * 18 + 16], *v, *c;
    struct jviz_bars b = { 4, 5, 2, 2, 0, 0x0841, 0xF81F, 0xFFE0, 0x1234, ht, pk };
    uint32_t g, i, x = 0x9E3779B9;

    v = (uint16_t *)(((uint32_t)buf + 31) & ~31u);
    c = v + 32 * 18;
    for (g = 0; g < 5; g++)
    {
        for (i = 0; i < 32 * 18; i++)
        {
            x = x * 1103515245u + 12345;
            v[i] = c[i] = x >> 16;
        }
        b.flags = geo[g][2];
        if (!viz_bars(v + geo[g][0], 32, geo[g][0], geo[g][1], 18, &b))
            return 0;
        jviz_bars_draw(c + geo[g][0], 32, geo[g][1], 18, &b, 0);
        for (i = 0; i < 32 * 18; i++)
            if (v[i] != c[i])
                return 0;
    }
    return !fail;
}

/* ---- POLYLINE, SEGMENTS and PALROW (jviz.h; plain C) ------------------ *
 * No kernel to check against the C: the C is what runs.  The check is of
 * the build: each op's fixed scene (jviz_draw_check) against the sum the
 * Mac's build of the same C gives (viz_test), so a miscompile or a core
 * that differs takes the op off (the PP draws it, logged) instead of
 * drawing wrong pixels */
uint32_t viz_draw_init(int fail)
{
    static const uint32_t want[3] = { JVIZ_CHK_POLY, JVIZ_CHK_SEGS, JVIZ_CHK_PAL };
    uint16_t s[JVIZ_CHK_W * JVIZ_CHK_H];
    uint32_t op, ok = 0;

    for (op = 0; op < 3; op++)
        if (jviz_draw_check(s, op) == want[op] + (uint32_t)fail)
            ok |= 1u << op;
    return ok;
}
