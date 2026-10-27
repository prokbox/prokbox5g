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

#include <stdint.h>
#include "hostproto.h"
#include "jref.h"
#include "jviz.h"

/* The visualizer's jobs (proposed, sim only; #126, jobs.h J_OP_SPECTRUM
 * and J_OP_BARS, docs/research/bcm2722/visualizer.md): one definition
 * for every runner.  jref_exec calls them in a JREF_VIZ build (the host
 * model, the BCMFW_VIZ image); the display-only image's overlay
 * (video/viz_ovl.c) calls them from its exec, against the resident jref
 * (the same struct: the overlay is built with the image's flags). */

/* jref.c surf()'s twin, which the display-only image can't spare the
 * bytes to export to its overlay: J_SURF_*, or a slot of this epoch in
 * one of the states allowed */
static __attribute__((noinline)) struct jref_surf *surf(struct jref *j, uint32_t id,
                                                        uint32_t states)
{
    struct jref_slot *s;

    if (id == J_SURF_FRONT)
        return &j->front;
    if (id == J_SURF_BACK)
        return &j->back;
    if (id >= JREF_SLOTS)
        return 0;
    s = &j->slot[id];
    if (!(states & (1u << s->state)) || s->epoch != j->epoch)
        return 0;
    return &s->surf;
}

uint32_t jref_spectrum(struct jref *j, const uint32_t *a, uint32_t n, uint32_t *rep,
                       uint32_t *nr, int (*hook)(uint32_t *p, const int16_t *pcm,
                                                 uint32_t n))
{
    struct jref_surf *src = J_SURF_IS_SLOT(a[0]) ? surf(j, a[0], JREF_READ_OK) : 0;
    uint32_t ns = J_X(a[1]), nb = J_Y(a[1]), b, p[JVIZ_M_MAX];
    uint16_t e[JVIZ_BANDS_MAX + 1], l2[JVIZ_BANDS_MAX];

    if (!src)
        return J_E_SLOT;
    if ((ns != 256 && ns != 512) || !nb || nb > JVIZ_BANDS_MAX || n < J_SPEC_ARGS(nb))
        return HP_E_INVAL;
    for (b = 0; b <= nb; b++)
    {
        e[b] = (a[2 + b / 2] >> (16 * (b & 1))) & 0xFFFF;
        if (e[b] > ns / 2 || (b && e[b] < e[b - 1]))
            return HP_E_INVAL;
    }
    if (src->pitch * src->h < ns)
        return J_E_RANGE;
#ifdef JVIZ_OVL
    int16_t tb[JVIZ_TAB256_HW + 16];

    if (ns == 256)              /* the overlay derives 256's tables */
        jviz_derive256((int16_t *)(((uintptr_t)tb + 31) & ~(uintptr_t)31));
#endif
    if (!hook || !hook(p, (const int16_t *)src->px, ns))
        jviz_power_ref(p, (const int16_t *)src->px, ns);
    jviz_bands(l2, p, ns / 2, e, nb);
    for (b = 0; b < nb; b += 2)
        rep[1 + b / 2] = l2[b] | (b + 1 < nb ? (uint32_t)l2[b + 1] << 16 : 0);
    *nr = 1 + (nb + 1) / 2;
    return HP_OK;
}

uint32_t jref_bars(struct jref *j, const uint32_t *a, uint32_t n, jviz_bars_fn hook)
{
    struct jref_surf *dst = surf(j, a[0], JREF_WRITE_OK);
    uint32_t cnt = a[3] & 0xFF, words = (cnt + 3) / 4, i;
    uint32_t x = J_X(a[1]), y = J_Y(a[1]), w = J_X(a[2]), h = J_Y(a[2]);
    uint8_t ht[JVIZ_BANDS_MAX], pk[JVIZ_BANDS_MAX];
    struct jviz_bars b;
    jviz_fill_fn fill = 0;

    if (!dst)
        return J_E_SLOT;
    if (!cnt || cnt > JVIZ_BANDS_MAX || n < J_BARS_ARGS(cnt))
        return HP_E_INVAL;
    if (!w || !h || x + w > dst->w || y + h > dst->h)   /* jref.c rect_ok */
        return J_E_RANGE;
    for (i = 0; i < cnt; i++)
    {
        ht[i] = a[7 + i / 4] >> (8 * (i & 3));
        pk[i] = a[7 + words + i / 4] >> (8 * (i & 3));
    }
    b.n = cnt;
    b.barw = (a[3] >> 8) & 0xFF;
    b.gap = (a[3] >> 16) & 0xFF;
    b.cap_h = a[3] >> 24;
    b.bot = a[4] & 0xFFFF;
    b.top = a[4] >> 16;
    b.cap = a[5] & 0xFFFF;
    b.bg = a[5] >> 16;
    b.flags = a[6];
    b.height = ht;
    b.peak = pk;
#ifdef JREF_HOOKS
    fill = j->fill;
#endif
    if (hook && hook(dst->px + y * dst->pitch + x, dst->pitch, x, w, h, &b))
        return HP_OK;
    return jviz_bars_draw(dst->px + y * dst->pitch + x, dst->pitch, w, h, &b, fill) ?
           J_E_RANGE : HP_OK;
}

/* POLYLINE, SEGMENTS and PALROW (jobs.h; jviz.h): the destination and
 * its rect (jref.c's rect_ok), and the source slot src (READY, at least
 * min halfwords) when src_at; 0 and *st on any error */
static uint16_t *op_rect(struct jref *j, const uint32_t *a, uint32_t *w, uint32_t *h,
                         uint32_t *pitch, uint32_t src_at, uint32_t min,
                         struct jref_surf **src, uint32_t *st)
{
    struct jref_surf *dst = surf(j, a[0], JREF_WRITE_OK);
    uint32_t x = J_X(a[1]), y = J_Y(a[1]);

    *w = J_X(a[2]);
    *h = J_Y(a[2]);
    *st = J_E_SLOT;
    if (src_at)
    {
        *src = J_SURF_IS_SLOT(a[src_at]) ? surf(j, a[src_at], JREF_READ_OK) : 0;
        if (!*src)
            return 0;
    }
    if (!dst)
        return 0;
    *st = J_E_RANGE;
    if (!*w || !*h || x + *w > dst->w || y + *h > dst->h ||
        (src_at && (*src)->pitch * (*src)->h < min))
        return 0;
    *pitch = dst->pitch;
    return dst->px + y * dst->pitch + x;
}

/* n bytes, four to a word from a, the low first */
static void bytes(uint8_t *d, const uint32_t *a, uint32_t n)
{
    uint32_t i;

    for (i = 0; i < n; i++)
        d[i] = a[i / 4] >> (8 * (i & 3));
}

uint32_t jref_polyline(struct jref *j, const uint32_t *a, uint32_t n)
{
    struct jref_surf *src;
    uint32_t w, h, pitch, st;
    uint16_t *px;
    struct jviz_poly p;

    (void)n;                    /* J_POLY_ARGS: the callers check it */
    p.n = a[5] & 0xFFFF;
    p.width = a[7] >> 16;
    if (p.n < 2 || p.n > JVIZ_POLY_N_MAX || !p.width || p.width > 3)
        return HP_E_INVAL;
    px = op_rect(j, a, &w, &h, &pitch, 3, 0, &src, &st);
    if (!px)
        return st;
    p.src = src->px;
    p.flags = a[5] >> 16;
    p.len = src->pitch * src->h * (p.flags & JVIZ_POLY_U8 ? 2 : 1);
    p.off = a[4] & 0xFFFF;
    p.step = a[4] >> 16;
    p.gain = a[6] & 0xFFFF;
    p.half = a[6] >> 16;
    p.colour = a[7];
    return jviz_poly_draw(px, pitch, w, h, &p) ? J_E_RANGE : HP_OK;
}

uint32_t jref_segments(struct jref *j, const uint32_t *a, uint32_t n)
{
    uint32_t cnt = a[4] & 0xFF, w, h, pitch, st;
    uint8_t len[JVIZ_SEG_MAX], pk[JVIZ_SEG_MAX];
    uint16_t *px;
    struct jviz_segs s;

    if (!cnt || cnt > JVIZ_SEG_MAX || n < J_SEGS_ARGS(cnt))
        return HP_E_INVAL;
    px = op_rect(j, a, &w, &h, &pitch, 0, 0, 0, &st);
    if (!px)
        return st;
    s.n = cnt;
    s.cx = (int16_t)a[3];
    s.cy = (int16_t)(a[3] >> 16);
    s.a0 = (a[4] >> 8) & 0xFF;
    s.astep = (a[4] >> 16) & 0xFF;
    s.width = (a[4] >> 24) & 0xF;
    s.flags = a[4] >> 28;
    s.r0 = a[5] & 0xFF;
    s.lmax = (a[5] >> 8) & 0xFF;
    s.rring = (a[5] >> 16) & 0xFF;
    s.capd = (a[5] >> 24) & 0xF;
    s.ringd = a[5] >> 28;
    s.c0 = a[6];
    s.c1 = a[6] >> 16;
    s.cap = a[7];
    s.ring = a[7] >> 16;
    bytes(len, a + 8, cnt);
    bytes(pk, a + 8 + (cnt + 3) / 4, cnt);
    s.len = len;
    s.peak = pk;
    return jviz_segs_draw(px, pitch, w, h, &s) ? HP_E_INVAL : HP_OK;
}

uint32_t jref_palrow(struct jref *j, const uint32_t *a, uint32_t n)
{
    struct jref_surf *lut;
    uint32_t w, h, pitch, st;
    uint8_t lv[JVIZ_PAL_MAX];
    uint16_t *px;
    struct jviz_pal p;

    p.nb = a[4];
    if (!p.nb || p.nb > JVIZ_PAL_MAX || n < J_PAL_ARGS(p.nb))
        return HP_E_INVAL;
    px = op_rect(j, a, &w, &h, &pitch, 3, 256, &lut, &st);
    if (!px)
        return st;
    bytes(lv, a + 5, p.nb);
    p.lut = lut->px;
    p.lv = lv;
    return jviz_pal_draw(px, pitch, w, h, &p) ? J_E_RANGE : HP_OK;
}
