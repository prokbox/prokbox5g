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
#include "jmix.h"

/* ---- helpers ---------------------------------------------------------- */

/* COMMIT's sum (jobs.h j_f16w): the w x h pixels row by row, the
 * padding of a wider pitch left out */
void jref_f16w(const struct jref_surf *s, uint32_t *a, uint32_t *b)
{
    uint32_t y;

    *a = *b = 0;
    for (y = 0; y < s->h; y++)
        j_f16w(a, b, s->px + y * s->pitch, s->w);
}

void jref_setup(struct jref *j, const struct jref_surf *front,
                const struct jref_surf *back, const struct jref_surf *panel)
{
    uint32_t i;
    uint8_t *p = (uint8_t *)j;

    for (i = 0; i < sizeof(*j); i++)
        p[i] = 0;
    j->front = *front;
    j->back = *back;
    j->panel = *panel;
    j->epoch = 1;
    j->mode = J_MODE_UI;
}

void jref_pool(struct jref *j, uint32_t cls, void *mem, uint32_t bytes)
{
    if (cls < JREF_CLASSES)
    {
        j->pool[cls].base = mem;
        j->pool[cls].size = bytes;
        j->pool[cls].used = 0;
    }
}

void jref_pool_addr(struct jref *j, uint32_t cls, uint32_t bus_base)
{
    if (cls < JREF_CLASSES)
        j->pool[cls].bus = bus_base;
}

/* A surface id to a surface: the frames, or a slot of this epoch in one
 * of the states allowed (bit per J_ST_*) */
static struct jref_surf *surf(struct jref *j, uint32_t id, uint32_t states)
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

#define READ_OK   JREF_READ_OK
#define WRITE_OK  JREF_WRITE_OK

static int rect_ok(const struct jref_surf *s, uint32_t xy, uint32_t wh)
{
    uint32_t x = J_X(xy), y = J_Y(xy), w = J_X(wh), h = J_Y(wh);

    return w && h && x + w <= s->w && y + h <= s->h;
}

static uint16_t *at(const struct jref_surf *s, uint32_t x, uint32_t y)
{
    return s->px + y * s->pitch + x;
}

/* Two pixels as one word (copy_rest) */
#ifdef __GNUC__
typedef uint32_t __attribute__((may_alias)) jref_u32;
#else
typedef uint32_t jref_u32;
#endif

/* The columns of [0, w) a hook's span (jref.h: lo | hi << 16) left to C,
 * [0, lo) then [hi, w), in order: FOR_REST(x, span, w) { ... } */
#define FOR_REST(x, sp, w) \
    for (uint32_t k_ = 0, e_ = ((x) = 0, (sp) ? (sp) & 0xFFFF : (w)); k_ < 2; \
         k_++, (x) = (sp) ? (sp) >> 16 : (w), e_ = (w)) \
        for (; (x) < e_; (x)++)

/* n rows (row i at d + i * dp, s + i * sp) of the columns a hook's span
 * left, copied in order; key as the copy hook's.  The two ranges, [0, lo)
 * and [hi, w), one after the other: the same as row by row, since the
 * rows of one call never overlap across columns (jref.h, copy) */
static void copy_rest(uint16_t *d, int32_t dp, const uint16_t *s, int32_t sp,
                      uint32_t w, int32_t n, uint32_t span, uint32_t key)
{
    uint32_t k, x, x0, e, lo = span ? span & 0xFFFF : w, hi = span ? span >> 16 : w;
    int32_t i;

    for (k = 0; k < 2; k++)
    {
        x0 = k ? hi : 0;
        e = k ? w : lo;
        if (x0 >= e)
            continue;
        for (i = 0; i < n; i++)
        {
            uint16_t *dr = d + i * dp;
            const uint16_t *sr = s + i * sp;

            x = x0;
            if (key)
            {
                for (; x < e; x++)
                    if (sr[x] != key >> 16)
                        dr[x] = sr[x];
                continue;
            }
            if (!(((uintptr_t)(dr + x) ^ (uintptr_t)(sr + x)) & 2))
            {
                /* the same phase: two pixels a word.  The same as pixel
                   by pixel, overlaps in one row included (BLIT's d < s, an
                   animation drawn from its own surface): reading s[x + 1]
                   with s[x] differs only if d is exactly one pixel past
                   s, and the same phase rules that out */
                jref_u32 *dw;
                const jref_u32 *sw;
                uint32_t m;

                if ((uintptr_t)(dr + x) & 2)
                    dr[x] = sr[x], x++;
                dw = (jref_u32 *)(dr + x);
                sw = (const jref_u32 *)(sr + x);
                m = (e - x) >> 1;
                x += 2 * m;
                for (; m; m--)
                    *dw++ = *sw++;
            }
            for (; x < e; x++)
                dr[x] = sr[x];
        }
    }
}

/* copy_run's modes */
#define RUN_HOOK    1       /* through the copy hook */
#define RUN_GROUPS  2       /* in 16-row groups (every row of one read first),
                               and the rows after the last in one call;
                               without it, a row at a time */
#define RUN_AGAIN   4       /* the last group over rows already copied, when
                               h is not a multiple of 16: their sources are
                               never overwritten, so they copy the same */

#ifdef JREF_HOOKS
/* h rows of w pixels from s to d, row by row in order (a negative pitch:
 * upward), as if pixel by pixel; mode RUN_* */
static void copy_run(struct jref *j, uint16_t *d, int32_t dp, const uint16_t *s,
                     int32_t sp, uint32_t w, int32_t h, uint32_t key, int mode)
{
    int32_t y, n, b;
    uint32_t how, span;

    for (y = 0; y < h; y += n, d += n * dp, s += n * sp)
    {
        n = h - y;              /* new rows this step */
        b = 0;
        if (!(mode & RUN_GROUPS))
            n = 1;              /* each row whole before the next: rows
                                   that overlap may differ in x */
        how = n;                /* the hook's rows; b rows back */
        if (!(mode & RUN_GROUPS))
            ;
        else if (n >= 16)
            how = 16 | JREF_GROUP, n = 16;
        else if ((mode & RUN_AGAIN) && h >= 16)
            how = 16 | JREF_GROUP, b = 16 - n;
        span = (mode & RUN_HOOK) && j->copy ?
               j->copy(d - b * dp, dp, s - b * sp, sp, w, how, key) : 0;
        copy_rest(d, dp, s, sp, w, n, span, key);
    }
}
#else
/* No hooks: every mode copies the rows in order, which copy_rest does
 * for all of them in one call (the groups and the last group again only
 * shape the hook's calls) */
#define copy_run(j, d, dp, s, sp, w, h, key, mode) \
    ((void)(j), copy_rest(d, dp, s, sp, w, h, 0, key))
#endif

/* ---- slots ------------------------------------------------------------ */

/* First fit in the class's pool, around its live slots */
static uint32_t pool_fit(struct jref *j, uint32_t cls, uint32_t bytes, uint32_t *off)
{
    struct jref_pool *p = &j->pool[cls];
    uint32_t o = 0, i, moved;

    do
    {
        moved = 0;
        for (i = 0; i < JREF_SLOTS; i++)
        {
            struct jref_slot *s = &j->slot[i];
            uint32_t so, se;

            if (s->state == J_ST_EMPTY || s->cls != cls || s->epoch != j->epoch)
                continue;
            so = (uint32_t)((uint8_t *)s->surf.px - p->base);
            se = so + ((s->bytes + 31) & ~31u);
            if (o < se && so < o + bytes)
            {
                o = se;
                moved = 1;
            }
        }
    } while (moved);
    if (o + bytes > p->size)
        return 0;
    *off = o;
    return 1;
}

static uint32_t op_alloc(struct jref *j, const uint32_t *a, uint32_t *rep, uint32_t *n)
{
    uint32_t cls = a[0], w = J_X(a[1]), h = J_Y(a[1]), fmt = a[2], i, off, bytes, pitch;
    struct jref_slot *s = 0;

    if (cls == 0 || cls >= JREF_CLASSES || !w || !h)
        return HP_E_INVAL;
    if (fmt != J_FMT_RGB565)
        return J_E_FORMAT;
    if (j->mode == J_MODE_VIDEO)
        return J_E_MODE;            /* a video session has the arena */
    if (!j->pool[cls].size)
        return (cls == J_CLS_GAME) != (j->mode == J_MODE_GAME) ? J_E_MODE : J_E_NOSLOT;
    for (i = 0; i < JREF_SLOTS; i++)
    {
        struct jref_slot *t = &j->slot[i];

        if (t->state == J_ST_READY && t->epoch == j->epoch && t->cls == cls &&
            t->key_lo == a[3] && t->key_hi == a[4] && t->w == w && t->h == h)
        {
            rep[1] = i;
            rep[2] = j->pool[cls].bus + (uint32_t)((uint8_t *)t->surf.px - j->pool[cls].base);
            rep[3] = t->surf.pitch * 2;
            rep[4] = J_ALLOC_HIT;
            *n = 5;
            return HP_OK;
        }
    }
    for (i = 0; i < JREF_SLOTS && !s; i++)
        if (j->slot[i].state == J_ST_EMPTY || j->slot[i].epoch != j->epoch)
            s = &j->slot[i];
    pitch = J_SLOT_PITCH(cls, w);
    bytes = pitch * h * 2;
    if (!s || !pool_fit(j, cls, (bytes + 31) & ~31u, &off))
        return J_E_NOSLOT;
    i = (uint32_t)(s - j->slot);
    s->key_lo = a[3];
    s->key_hi = a[4];
    s->bytes = bytes;
    s->epoch = j->epoch;
    s->sum = 0;
    s->w = w;
    s->h = h;
    s->fmt = fmt;
    s->cls = cls;
    s->state = J_ST_ALLOCATED;
    s->surf.px = (uint16_t *)(j->pool[cls].base + off);
    s->surf.w = w;
    s->surf.h = h;
    s->surf.pitch = pitch;
    rep[1] = i;
    rep[2] = j->pool[cls].bus + off;
    rep[3] = pitch * 2;
    rep[4] = 0;
    *n = 5;
    return HP_OK;
}

uint16_t *jref_slot_px(struct jref *j, uint32_t slot)
{
    struct jref_surf *s = slot < JREF_SLOTS ? surf(j, slot, WRITE_OK) : 0;

    return s ? s->px : 0;
}

/* ---- drawing ---------------------------------------------------------- */

static uint32_t op_blit(struct jref *j, const uint32_t *a)
{
    struct jref_surf *src = surf(j, a[0], READ_OK), *dst = surf(j, a[3], WRITE_OK);
    uint32_t w = J_X(a[2]), h = J_Y(a[2]), y, x, key;
    int32_t sy = J_Y(a[1]), dy = J_Y(a[4]), sp, dp;
    const uint16_t *s;
    uint16_t *d;

    if (!src || !dst)
        return J_E_SLOT;
    if (!rect_ok(src, a[1], a[2]) || !rect_ok(dst, a[4], a[2]))
        return J_E_RANGE;
    key = a[5] & J_BLIT_KEY ? (a[5] & 0xFFFF0000u) | J_BLIT_KEY : 0;
    s = at(src, J_X(a[1]), sy);
    d = at(dst, J_X(a[4]), dy);
    sp = src->pitch;
    dp = dst->pitch;
    if (src != dst || dy - sy >= (int32_t)h || sy - dy >= (int32_t)h ||
        J_X(a[4]) - J_X(a[1]) + w - 1 >= 2 * w - 1)
    {
        copy_run(j, d, dp, s, sp, w, h, key, RUN_HOOK | RUN_GROUPS | RUN_AGAIN);
        return HP_OK;
    }
    /* overlapping rects of one surface: rows in the safe order (rows of
       different y never share pixels); within one row, as memmove */
    if (dy > sy)
        copy_run(j, d + (h - 1) * dp, -dp, s + (h - 1) * sp, -sp, w, h, key, RUN_HOOK);
    else if (dy < sy)
        copy_run(j, d, dp, s, sp, w, h, key, RUN_HOOK);
    else if (d > s)
        for (y = 0; y < h; y++, d += dp, s += sp)
        {
            /* from the end (a pop slide's in-place move).  Without a key,
               in the same phase (d 2 pixels or more past s), two pixels a
               word, as pixel by pixel: s[x - 2] is never d[x - 1] */
            x = w;
            if (key)
            {
                while (x-- > 0)
                    if (s[x] != key >> 16)
                        d[x] = s[x];
                continue;
            }
            if (!(((uintptr_t)d ^ (uintptr_t)s) & 2))
            {
                if ((uintptr_t)(d + x) & 2)
                    x--, d[x] = s[x];
                for (; x >= 2; x -= 2)
                    *(jref_u32 *)(d + x - 2) = *(const jref_u32 *)(s + x - 2);
            }
            while (x-- > 0)
                d[x] = s[x];
        }
    else if (d < s)
        copy_rest(d, dp, s, sp, w, h, 0, key);
    return HP_OK;
}

static uint32_t op_fill(struct jref *j, const uint32_t *a)
{
    struct jref_surf *dst = surf(j, a[0], WRITE_OK);
    uint32_t y, x, w = J_X(a[2]), h = J_Y(a[2]), c = a[3] & 0xFFFF, span = 0;
    uint16_t *d;

    if (!dst)
        return J_E_SLOT;
    if (!rect_ok(dst, a[1], a[2]))
        return J_E_RANGE;
    d = at(dst, J_X(a[1]), J_Y(a[1]));
#ifdef JREF_HOOKS
    if (j->fill)
        span = j->fill(d, dst->pitch, w, h, c);
#endif
    for (y = 0; y < h; y++, d += dst->pitch)
        FOR_REST(x, span, w)
            d[x] = c;
    return HP_OK;
}

/* FADE and AVG: dst, dxy, wh, a, axy, b, bxy [, weight] */
static uint32_t op_mix(struct jref *j, const uint32_t *a, uint32_t n, int fade)
{
    uint32_t ncols = 0;

    struct jref_surf *dst = surf(j, a[0], WRITE_OK), *sa = surf(j, a[3], READ_OK | WRITE_OK);
    struct jref_surf *sb = surf(j, a[5], READ_OK | WRITE_OK);
    uint32_t weight = fade ? a[7] : 0, y, x, w = J_X(a[2]), h = J_Y(a[2]), g;
    const uint16_t *pa, *pb;
    uint16_t *d;
    int vec;

    if (!dst || !sa || !sb)
        return J_E_SLOT;
    if (fade && (weight < 1 || weight > 256))
        return HP_E_INVAL;
    if (fade && n > 8 && (a[8] & J_FADE_SHADE_B))
    {
        ncols = a[8] & 0xFF;
        if (ncols > J_SHADE_MAX || ncols > J_X(a[2]) || n < 9 + (ncols + 3) / 4)
            return HP_E_INVAL;
    }
    if (!rect_ok(dst, a[1], a[2]) || !rect_ok(sa, a[4], a[2]) || !rect_ok(sb, a[6], a[2]))
        return J_E_RANGE;
    /* the vector blocks read 16 rows before writing them: only where that
       is the same as row by row (the destination is another surface, or
       the same rect of a source: the in-place step); b's shaded columns
       too (the hook shades them first) */
#ifdef JREF_HOOKS
    vec = j->mix16 && (dst != sa || a[1] == a[4]) && (dst != sb || a[1] == a[6]);
#else
    vec = 0;
#endif
    d = at(dst, J_X(a[1]), J_Y(a[1]));
    pa = at(sa, J_X(a[4]), J_Y(a[4]));
    pb = at(sb, J_X(a[6]), J_Y(a[6]));
    for (y = 0; y < h; y += g)
    {
        uint32_t r, span = 0;

        g = vec && h - y >= 16 ? 16 : 1;
#ifdef JREF_HOOKS
        if (g == 16)
            span = j->mix16(d, dst->pitch, pa, sa->pitch, pb, sb->pitch, w, weight,
                            ncols, y ? 0 : a + 9);
#endif
        if (span == w << 16)
        {
            d += g * dst->pitch;        /* the hook did every column */
            pa += g * sa->pitch;
            pb += g * sb->pitch;
            continue;
        }
        for (r = 0; r < g; r++, d += dst->pitch, pa += sa->pitch, pb += sb->pitch)
            FOR_REST(x, span, w)
            {
                uint32_t bv = pb[x];

                if (x < ncols)
                    bv = jmix_shade_px(bv, (a[9 + x / 4] >> (8 * (x & 3))) & 0xFF);
                d[x] = fade ? jmix_fade_px(pa[x], bv, weight) : jmix_avg_px(pa[x], bv);
            }
    }
    return HP_OK;
}

static uint32_t op_shade(struct jref *j, const uint32_t *a, uint32_t n)
{
    struct jref_surf *s = surf(j, a[0], WRITE_OK);
    uint32_t w = J_X(a[2]), h = J_Y(a[2]), y, x, r, g;
    uint16_t *d;

    if (!s)
        return J_E_SLOT;
    if (w > J_SHADE_MAX || n < 3 + (w + 3) / 4)
        return HP_E_INVAL;
    if (!rect_ok(s, a[1], a[2]))
        return J_E_RANGE;
    d = at(s, J_X(a[1]), J_Y(a[1]));
    for (y = 0; y < h; y += g)
    {
        uint32_t span = 0;

#ifdef JREF_HOOKS
        g = j->shade16 && h - y >= 16 ? 16 : 1;
        if (g == 16)
            span = j->shade16(d, s->pitch, w, y ? 0 : a + 3);
#else
        g = 1;
#endif
        if (span == w << 16)
            d += g * s->pitch;          /* the hook did every column */
        else
            for (r = 0; r < g; r++, d += s->pitch)
                FOR_REST(x, span, w)
                    d[x] = jmix_shade_px(d[x], (a[3 + x / 4] >> (8 * (x & 3))) & 0xFF);
    }
    return HP_OK;
}

static uint32_t op_scroll(struct jref *j, const uint32_t *a)
{
    struct jref_surf *s = surf(j, a[0], WRITE_OK);
    int32_t dy = (int16_t)(a[3] & 0xFFFF), h = J_Y(a[2]), n, r0, p;

    if (!s)
        return J_E_SLOT;
    if (!rect_ok(s, a[1], a[2]))
        return J_E_RANGE;
    if (dy == 0 || dy >= h || -dy >= h)
        return HP_OK;           /* nothing moves inside the rect */
    /* the rows that get a source: rows going down (dy > 0) are copied from
       the bottom up, rows going up from the top down, in groups of 16
       (every row of one read first); past |dy| >= 16 the last group may
       start over rows already moved, as their sources stay */
    n = h - (dy > 0 ? dy : -dy);
    r0 = J_Y(a[1]) + (dy > 0 ? h - 1 : 0);
    p = dy > 0 ? -(int32_t)s->pitch : (int32_t)s->pitch;
    copy_run(j, at(s, J_X(a[1]), r0), p, at(s, J_X(a[1]), r0 - dy), p, J_X(a[2]), n, 0,
             RUN_HOOK | RUN_GROUPS | (dy >= 16 || dy <= -16 ? RUN_AGAIN : 0));
    return HP_OK;
}

#ifdef JREF_DECODE
/* DECODE: the source slot's first len bytes, n outputs (jobs.h, jdec.h) */
static uint32_t op_decode(struct jref *j, const uint32_t *a, uint32_t n,
                          uint32_t *rep, uint32_t *nr)
{
    struct jref_surf *src = surf(j, a[0], READ_OK), *d;
    struct jdec_out out[J_DEC_MAX];
    struct jdec_info info;
    uint32_t k, cnt = a[2];
    int e;

    if (!src || a[0] >= JREF_SLOTS)
        return J_E_SLOT;
    if (cnt < 1 || cnt > J_DEC_MAX || n < J_DEC_ARGS(cnt))
        return HP_E_INVAL;
    if (a[1] < 4 || a[1] > 2 * src->pitch * src->h)
        return J_E_RANGE;
    for (k = 0; k < cnt; k++)
    {
        const uint32_t *o = a + 3 + 5 * k;

        d = surf(j, o[3], WRITE_OK);
        if (!d)
            return J_E_SLOT;
        if (d == src || !rect_ok(d, o[4], o[2]))
            return J_E_RANGE;
        out[k].dw = J_X(o[0]);
        out[k].dh = J_Y(o[0]);
        out[k].cx = J_X(o[1]);
        out[k].cy = J_Y(o[1]);
        out[k].cw = J_X(o[2]);
        out[k].ch = J_Y(o[2]);
        out[k].px = at(d, J_X(o[4]), J_Y(o[4]));
        out[k].pitch = d->pitch;
    }
    e = j->decode((const uint8_t *)src->px, a[1], out, (int)cnt, &info);
    switch (e)
    {
    case JDEC_OK:
        rep[1] = info.sw | (uint32_t)info.sh << 16;
        *nr = 2;
        return HP_OK;
    case JDEC_E_UNSUPPORTED: return J_E_DEC_UNSUPPORTED;
    case JDEC_E_TRUNCATED:   return J_E_DEC_TRUNCATED;
    case JDEC_E_RANGE:       return J_E_RANGE;
    case JDEC_E_NOMEM:       return HP_E_NOMEM;
    default:                 return J_E_DEC_CORRUPT;
    }
}
#endif

static uint32_t op_present(struct jref *j, const uint32_t *a, uint32_t *rep, uint32_t *n)
{
    struct jref_surf *s = surf(j, a[0], READ_OK);
    uint32_t y, x;

    if (!s)
        return J_E_SLOT;
    if (!rect_ok(s, a[1], a[2]) || !rect_ok(&j->panel, a[1], a[2]))
        return J_E_RANGE;
    if (j->present)
    {
        j->present(s, a[1], a[2], a[3]);
        j->presents++;
        rep[1] = 0;
        *n = 2;
        return HP_OK;
    }
    for (y = 0; y < J_Y(a[2]); y++)
    {
        const uint16_t *f = at(s, J_X(a[1]), J_Y(a[1]) + y);
        uint16_t *d = at(&j->panel, J_X(a[1]), J_Y(a[1]) + y);

        for (x = 0; x < J_X(a[2]); x++)
            d[x] = f[x];
    }
    j->presents++;
    rep[1] = 0;                 /* the model waits for nothing */
    *n = 2;
    return HP_OK;
}

/* ---- animations ----------------------------------------------------- */

/* Progress 0..65536 at t of ms, linear (PAN): t * 65536 / ms rounded
 * down, by long division (t < ms throughout, so nothing overflows): no
 * 64-bit divide, whose library routine was 1.2 KB of the BCM's image */
static uint32_t progress(uint32_t t, uint32_t ms)
{
    uint32_t q = 0, i;

    if (!ms || t >= ms)
        return 65536;
    for (i = 0; i < 16; i++)
    {
        q <<= 1;
        if (t >= ms - t)
        {
            t -= ms - t;        /* 2t - ms */
            q |= 1;
        }
        else
            t <<= 1;
    }
    return q;
}

static uint32_t anim_check(struct jref *j, const uint32_t *a, uint32_t n)
{
    if (a[0] == J_ANIM_SLIDE)
    {
        struct jref_surf *f = n >= 8 ? surf(j, a[1], READ_OK) : 0;
        struct jref_surf *t = n >= 8 ? surf(j, a[2], READ_OK) : 0;
        uint32_t count = n >= 8 ? a[7] : 0, k, ext, o, prev = 0;

        if (n < 8 || count < 1 || count > J_SLIDE_MAX || n < 8 + (count + 1) / 2 ||
            !a[6])
            return HP_E_INVAL;
        if (!f || !t)
            return J_E_SLOT;
        if (!rect_ok(f, a[3], a[4]) || !rect_ok(t, a[3], a[4]) ||
            !rect_ok(&j->panel, a[3], a[4]) || a[5] > 3)
            return J_E_RANGE;
        ext = a[5] < 2 ? J_X(a[4]) : J_Y(a[4]);
        /* the offsets fit the extent and never decrease (rev 0.4) */
        for (k = 0; k < count; k++, prev = o)
            if ((o = (a[8 + k / 2] >> (16 * (k & 1))) & 0xFFFF) > ext || o < prev)
                return J_E_RANGE;
        return HP_OK;
    }
    if (a[0] == J_ANIM_PAN)
    {
        struct jref_surf *s = n >= 8 ? surf(j, a[1], READ_OK) : 0;
        uint32_t step = n >= 8 && (a[7] & J_PAN_STEP);

        /* the stepped form (jobs.h J_PAN_STEP): its three words too, a
           step of some px, one step at least, the rest window in the
           slot, a step of 16 bits of ms (so a lap fits 32 bits) */
        if (n < (step ? J_PAN_ARGS : 8))
            return HP_E_INVAL;
        if (!s)
            return J_E_SLOT;
        if (!rect_ok(s, a[2], a[4]) || !rect_ok(s, a[3], a[4]) ||
            !rect_ok(&j->panel, a[5], a[4]) || !a[6] ||
            (step && (!(a[8] & 0xFFFF) || (a[6] >> 16) ||
                      J_X(a[3]) <= J_X(a[2]) + (a[8] & 0xFFFF) || !rect_ok(s, a[9], a[4]))))
            return J_E_RANGE;
        return HP_OK;
    }
    return HP_E_INVAL;
}

uint32_t jref_anim_frame(struct jref *j, const uint32_t *rec, uint32_t t_ms)
{
    const uint32_t *a = rec + 1;
    uint32_t b[6], running = J_ANIM_FRAME_US;

    if (anim_check(j, a, HP_REC_LEN(rec[0])) != HP_OK)
        return 0;
    /* every part is a BLIT into the front frame (rev 0.4): from another
     * surface in 16-row groups, from the frame itself in BLIT's overlap
     * order, as if its rect were read whole first; an empty part draws
     * nothing (BLIT's J_E_RANGE) */
    b[3] = J_SURF_FRONT;
    b[5] = 0;
    if (a[0] == J_ANIM_SLIDE)
    {
        /* the old picture moving out, the new one following it in, along
         * the axis (sh: x, or y): the old part first, n of the extent at
         * od from 'from' at os, then the new, off of it at nd from 'to'
         * at ns.  From the front frame itself (rev 0.4), the old picture
         * already sits moved by the offset drawn last: it moves on by the
         * difference, so frame k equals the slide out of the frame as the
         * ANIM found it */
        uint32_t dir = a[5], frame = t_ms / a[6], count = a[7];
        uint32_t k = frame < count ? frame : count - 1;
        uint32_t off = (a[8 + k / 2] >> (16 * (k & 1))) & 0xFFFF;
        uint32_t sh = dir & 2 ? 16 : 0, n = ((a[4] >> sh) & 0xFFFF) - off;
        uint32_t keep = a[4] & ~(0xFFFFu << sh);
        uint32_t last = a[1] == J_SURF_FRONT ? j->anim_off : 0;
        uint32_t od = 0, os = off - last, nd = n, ns = 0;

        if (dir & 1)                        /* right, down: the new picture first */
        {
            od = off;
            os = last;
            nd = 0;
            ns = n;
        }
        b[0] = a[1];
        b[1] = a[3] + (os << sh);
        b[2] = keep | n << sh;
        b[4] = a[3] + (od << sh);
        op_blit(j, b);
        b[0] = a[2];
        b[1] = a[3] + (ns << sh);
        b[2] = keep | off << sh;
        b[4] = a[3] + (nd << sh);
        op_blit(j, b);
        j->anim_off = off;
        if (frame + 1 >= count)
            running = 0;
    }
    else
    {
        /* PAN: the window from (x0, y0) to (x1, y1) over the period;
         * loop 0 once, 1 again from the start, 2 back and forth.  Stepped
         * (J_PAN_STEP): laps of a pause on the rest window, then steps of
         * px every period ms (jobs.h) */
        uint32_t period = a[6], loop = a[7], t = t_ms, p;
        int32_t dx = (int32_t)J_X(a[3]) - (int32_t)J_X(a[2]);
        int32_t dy = (int32_t)J_Y(a[3]) - (int32_t)J_Y(a[2]);
        uint32_t wx, wy;

        if (loop & J_PAN_STEP)
        {
            uint32_t px = a[8] & 0xFFFF, pause = a[8] >> 16;
            uint32_t lap = pause + (uint32_t)(dx - 1) / px * period;

            t += a[10];
            running = period * 1000;
            if (loop & 0xFF)
                t %= lap;
            else if (t >= lap)
                running = 0;            /* one lap: it ends at rest */
            wx = J_X(a[9]);
            wy = J_Y(a[9]);
            if (t >= pause && running)
            {
                wx = J_X(a[2]) + ((t - pause) / period + 1) * px;
                wy = J_Y(a[2]);
            }
        }
        else
        {
            if (loop == 1)
                t %= period;
            else if (loop == 2)
            {
                t %= 2 * period;
                if (t > period)
                    t = 2 * period - t;
            }
            else if (t >= period)
                t = period, running = 0;
            p = progress(t, period);
            wx = (uint32_t)((int32_t)J_X(a[2]) + (int32_t)(((int64_t)dx * p + 32768) >> 16));
            wy = (uint32_t)((int32_t)J_Y(a[2]) + (int32_t)(((int64_t)dy * p + 32768) >> 16));
        }
        b[0] = a[1];
        b[1] = J_XY(wx, wy);
        b[2] = a[4];
        b[4] = a[5];
        op_blit(j, b);
    }
    return running;
}

/* ---- dispatch ---------------------------------------------------------- */

/* payload words each job needs (0: not in rev 0.1) */
static const uint8_t need[0x20] =
{
    [J_OP_FENCE - 0x20] = 1, [J_OP_ALLOC - 0x20] = 5, [J_OP_COMMIT - 0x20] = 3,
    [J_OP_FREE - 0x20] = 1, [J_OP_BLIT - 0x20] = 6, [J_OP_FILL - 0x20] = 4,
    [J_OP_FADE - 0x20] = 8, [J_OP_AVG - 0x20] = 7, [J_OP_SCROLL - 0x20] = 4,
    [J_OP_PRESENT - 0x20] = 4, [J_OP_ANIM - 0x20] = 1, [J_OP_ANIM_STOP - 0x20] = 1,
    [J_OP_STATS - 0x20] = 0xFF, [J_OP_RESET - 0x20] = 0xFF, [J_OP_SHADE - 0x20] = 3,
#ifdef JREF_DECODE
    [J_OP_DECODE - 0x20] = J_DEC_ARGS(1),
#endif
#ifdef JREF_VIZ
    [J_OP_SPECTRUM - 0x20] = J_SPEC_ARGS(1), [J_OP_BARS - 0x20] = J_BARS_ARGS(1),
    [J_OP_POLYLINE - 0x20] = J_POLY_ARGS, [J_OP_SEGMENTS - 0x20] = J_SEGS_ARGS(1),
    [J_OP_PALROW - 0x20] = J_PAL_ARGS(1),
#endif
};

uint32_t jref_exec(struct jref *j, const uint32_t *rec, uint32_t *reply,
                   uint32_t *nreply)
{
    uint32_t op = HP_REC_OP(rec[0]), n = HP_REC_LEN(rec[0]), tag = HP_REC_TAG(rec[0]);
    const uint32_t *a = rec + 1;
    uint32_t st = HP_OK, nr = 1, k;

    if (op < 0x20 || op > 0x3F
#ifndef JREF_EXT
        || !need[op - 0x20]
#endif
#ifdef JREF_DECODE
        || (op == J_OP_DECODE && !j->decode)
#endif
        )
        st = HP_E_UNKNOWN;
#ifdef JREF_EXT
    else if (!need[op - 0x20])  /* not this build's: the overlay's, or none */
        st = jref_ext(j, op, a, n, reply, &nr);
#endif
    else if (need[op - 0x20] != 0xFF && n < need[op - 0x20])
        st = HP_E_INVAL;
    else
        switch (op)
        {
        case J_OP_FENCE:
            j->fence = a[0] & 0xFFFF;
            reply[1] = j->fence;
            nr = 2;
            break;
        case J_OP_ALLOC:
            st = op_alloc(j, a, reply, &nr);
            break;
        case J_OP_COMMIT:
        {
            struct jref_surf *s = a[0] < JREF_SLOTS ? surf(j, a[0], WRITE_OK) : 0;
            uint32_t sa, sb;

            if (!s)
            {
                st = J_E_SLOT;
                break;
            }
            jref_f16w(s, &sa, &sb);
            if (!(n > 3 && (a[3] & J_COMMIT_NOCHECK)) && (a[1] != sa || a[2] != sb))
            {
                st = J_E_SUM;
                break;
            }
            j->slot[a[0]].sum = sb;
            j->slot[a[0]].state = J_ST_READY;
            reply[1] = sa;
            reply[2] = sb;
            nr = 3;
            break;
        }
        case J_OP_FREE:
            if (a[0] >= JREF_SLOTS || j->slot[a[0]].state == J_ST_EMPTY)
                st = J_E_SLOT;
            else
                j->slot[a[0]].state = J_ST_EMPTY;
            break;
        case J_OP_BLIT:    st = op_blit(j, a); break;
        case J_OP_FILL:    st = op_fill(j, a); break;
        case J_OP_FADE:    st = op_mix(j, a, n, 1); break;
        case J_OP_AVG:     st = op_mix(j, a, n, 0); break;
        case J_OP_SHADE:   st = op_shade(j, a, n); break;
        case J_OP_SCROLL:  st = op_scroll(j, a); break;
#ifdef JREF_DECODE
        case J_OP_DECODE:  st = op_decode(j, a, n, reply, &nr); break;
#endif
#ifdef JREF_VIZ
        case J_OP_SPECTRUM: st = jref_spectrum(j, a, n, reply, &nr, j->spectrum); break;
        case J_OP_BARS:    st = jref_bars(j, a, n, j->bars); break;
        case J_OP_POLYLINE: st = jref_polyline(j, a, n); break;
        case J_OP_SEGMENTS: st = jref_segments(j, a, n); break;
        case J_OP_PALROW:  st = jref_palrow(j, a, n); break;
#endif
        case J_OP_PRESENT: st = op_present(j, a, reply, &nr); break;
        case J_OP_ANIM:
            st = anim_check(j, a, n);
            j->anim_off = 0;
            break;
        case J_OP_ANIM_STOP:
            reply[1] = 0;
            nr = 2;
            break;
        case J_OP_STATS:
        {
            uint32_t free = 0;

            for (k = 0; k < JREF_CLASSES; k++)
                free += j->pool[k].size;        /* rev 0.1: sizes, not use */
            reply[1] = j->done;
            reply[2] = j->errors;
            reply[3] = 0;
            reply[4] = free;
            reply[5] = 0;
            reply[6] = j->epoch;
            nr = 7;
            break;
        }
        case J_OP_RESET:
            for (k = 0; k < JREF_SLOTS; k++)
                j->slot[k].state = J_ST_EMPTY;
            j->epoch++;
            break;
        default:
            st = HP_E_UNKNOWN;
            break;
        }
    reply[0] = st;
    j->done++;                  /* every job run, as J_DONE counts them */
    if (st != HP_OK)
    {
        j->errors++;
        j->last_err = op | st << 8 | tag << 16;
        nr = 1;
    }
    *nreply = (st != HP_OK || op == J_OP_FENCE || (tag & J_TAG_REPLY)) ? nr : 0;
    return st;
}

/* ---- the ring ------------------------------------------------------------ */

uint32_t jring_put(struct jring *r, uint32_t op, uint32_t tag,
                   const uint32_t *payload, uint32_t n)
{
    uint32_t used = (r->wr - r->rd) & 0xFFFF, m = r->size - 1, i;

    if (n > HP_MAX_PAYLOAD)
        return HP_E_TOOBIG;
    if (used + n + 1 > r->size)
        return HP_E_BUSY;
    r->w[r->wr & m] = HP_REC(op, n, tag);
    for (i = 0; i < n; i++)
        r->w[(r->wr + 1 + i) & m] = payload[i];
    r->wr = (r->wr + n + 1) & 0xFFFF;
    return HP_OK;
}

uint32_t jring_get(struct jring *r, uint32_t *rec)
{
    uint32_t m = r->size - 1, n, i;

    if (r->rd == r->wr)
        return 0;
    rec[0] = r->w[r->rd & m];
    n = HP_REC_LEN(rec[0]);
    for (i = 0; i < n; i++)
        rec[1 + i] = r->w[(r->rd + 1 + i) & m];
    r->rd = (r->rd + n + 1) & 0xFFFF;
    return n + 1;
}
