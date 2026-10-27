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
#include "r3d_int.h"

/* The frame: sky, transform, clip, set up into the ordering table, then
 * draw the table far to near.  Everything integer: view space in world
 * units, Q14 rotation, integer screen vertices (the PS1's wobble is
 * part of the look), 16.16 edge and attribute steps. */

struct r3d_mem r3d_m;
uint32_t (*r3d_now)(void);

static uint32_t nprims;
static int in_tess;                 /* emit: pieces are not counted */
static struct r3d_stats *stats;

/* A bound GCC cannot see as a constant: vc4-elf-gcc 6.2.1 loses the
 * source of min/max with a long immediate (vc2cc refuses that form) */
static inline int32_t opaque(int32_t v)
{
    __asm__("" : "+r"(v));
    return v;
}

static uint32_t now(void)
{
    return r3d_now ? r3d_now() : 0;
}

/* ---- sky ---- */

static void sky(const struct r3d_cam *c, uint16_t *fb)
{
    /* the horizon's row: a far point straight ahead at eye height */
    int32_t fx = c->m[6], fz = c->m[8];
    int32_t uy = c->m[3] * fx + c->m[5] * fz;     /* Q28 */
    int32_t zz = c->m[6] * fx + c->m[8] * fz;
    int32_t hz = R3D_H / 2, y;
    uint32_t *d = (uint32_t *)fb;

    if (zz > 0)
        hz = R3D_H / 2 - (int32_t)(((int64_t)uy * R3D_FOCAL) / zz);
    for (y = 0; y < R3D_H; y++)
    {
        uint32_t px, w;
        int32_t k = hz - y;                       /* rows above the horizon */
        int n;

        if (k <= 0)
            px = r3d_sky_low;
        else if (k >= 128)
            px = r3d_sky_top;
        else
        {
            /* blend fog -> top over 128 rows, per channel */
            uint32_t a = r3d_sky_fog, b = r3d_sky_top;
            uint32_t r = ((a >> 11) * (128 - k) + (b >> 11) * k) >> 7;
            uint32_t g = (((a >> 5) & 63) * (128 - k) + ((b >> 5) & 63) * k) >> 7;
            uint32_t bl = ((a & 31) * (128 - k) + (b & 31) * k) >> 7;
            px = r << 11 | g << 5 | bl;
        }
        w = px | px << 16;
        for (n = R3D_W / 8; n; n--)
        {
            d[0] = w; d[1] = w; d[2] = w; d[3] = w;
            d += 4;
        }
    }
}

/* ---- transform ---- */

static void transform(const struct r3d_cam *c, uint32_t i, uint32_t end)
{
    const struct r3d_vtx *v = (const struct r3d_vtx *)r3d_m.verts + i;
    struct r3d_xv *o = (struct r3d_xv *)r3d_m.xverts + i;
    const int32_t *m = c->m;

    for (; i < end; i++, v++, o++)
    {
        int32_t dx = v->x - c->x, dy = v->y - c->y, dz = v->z - c->z;
        int32_t x = (m[0] * dx + m[1] * dy + m[2] * dz) >> 14;
        int32_t y = (m[3] * dx + m[4] * dy + m[5] * dz) >> 14;
        int32_t z = (m[6] * dx + m[7] * dy + m[8] * dz) >> 14;
        int32_t g = z * R3D_GUARD;
        uint32_t out = 0;
        int32_t f;

        if (z < R3D_NEAR)
            out = R3D_OUT_NEAR;
        else
        {
            if (x > g)
                out |= R3D_OUT_XP;
            if (x < -g)
                out |= R3D_OUT_XN;
            if (y > g)
                out |= R3D_OUT_YP;
            if (y < -g)
                out |= R3D_OUT_YN;
            if (z > R3D_FOG_FAR)
                out |= R3D_OUT_FAR;
        }
        o->x = x;
        o->y = y;
        o->z = z;
        o->out = out;
        if (!(out & R3D_OUT_CLIP))
        {
            o->sx = R3D_W / 2 + (x * R3D_FOCAL) / z;
            o->sy = R3D_H / 2 - (y * R3D_FOCAL) / z;
        }
        f = z <= R3D_FOG_NEAR ? 31 : z >= R3D_FOG_FAR ? 0 :
            31 - ((z - R3D_FOG_NEAR) * 31) / (R3D_FOG_FAR - R3D_FOG_NEAR);
        o->fog = f;
    }
}

/* ---- the view test: a chunk's sphere against the six planes ---- */

/* The side planes through the eye: |x| F = (W/2) z and |y| F = (H/2) z.
 * A sphere is out when its centre is more than r beyond one: the
 * planes' normals have lengths sqrt(F^2 + 160^2) = 256 and
 * sqrt(F^2 + 120^2) = 233 in these units. */
#define SIDE_X_LEN 256
#define SIDE_Y_LEN 233

static int in_view(const struct r3d_cam *c, const struct r3d_chunk *ch)
{
    const int32_t *m = c->m;
    int32_t dx = ch->cx - c->x, dy = ch->cy - c->y, dz = ch->cz - c->z;
    int32_t x = (m[0] * dx + m[1] * dy + m[2] * dz) >> 14;
    int32_t y = (m[3] * dx + m[4] * dy + m[5] * dz) >> 14;
    int32_t z = (m[6] * dx + m[7] * dy + m[8] * dz) >> 14;
    int32_t r = ch->r, ex = (R3D_W / 2) * z + SIDE_X_LEN * r;
    int32_t ey = (R3D_H / 2) * z + SIDE_Y_LEN * r;

    if (z + r < R3D_NEAR || z - r > R3D_FOG_FAR)
        return 0;
    if (x * R3D_FOCAL > ex || -x * R3D_FOCAL > ex)
        return 0;
    if (y * R3D_FOCAL > ey || -y * R3D_FOCAL > ey)
        return 0;
    return 1;
}

/* ---- set-up ---- */

static void emit(const int32_t *x, const int32_t *y, const uint8_t *u,
                 const uint8_t *v, const uint8_t *s, uint32_t tex, int32_t zsum)
{
    struct r3d_prim *p;
    int32_t b;

    if (nprims >= R3D_MAX_PRIMS)
        return;
    /* off the screen on one side */
    if ((x[0] < 0 && x[1] < 0 && x[2] < 0) ||
        (x[0] >= R3D_W && x[1] >= R3D_W && x[2] >= R3D_W) ||
        (y[0] < 0 && y[1] < 0 && y[2] < 0) ||
        (y[0] >= R3D_H && y[1] >= R3D_H && y[2] >= R3D_H))
    {
        if (!in_tess)
            stats->culled++;
        return;
    }
    p = (struct r3d_prim *)r3d_m.prims + nprims++;
    p->x[0] = x[0]; p->x[1] = x[1]; p->x[2] = x[2];
    p->y[0] = y[0]; p->y[1] = y[1]; p->y[2] = y[2];
    p->u[0] = u[0]; p->u[1] = u[1]; p->u[2] = u[2];
    p->v[0] = v[0]; p->v[1] = v[1]; p->v[2] = v[2];
    p->s[0] = s[0]; p->s[1] = s[1]; p->s[2] = s[2];
    p->tex = tex;
    b = (zsum * 85) >> (8 + R3D_OT_SHIFT);       /* mean z / 2^shift */
    if (b < 0)
        b = 0;
    if (b >= R3D_OT)
        b = R3D_OT - 1;
    p->next = r3d_m.ot[b];
    r3d_m.ot[b] = nprims;
}

/* front faces wind counter-clockwise seen from outside, which is
 * clockwise on the screen (y down): a negative signed area */
static inline int32_t area(const int32_t *x, const int32_t *y)
{
    return (x[1] - x[0]) * (y[2] - y[0]) - (x[2] - x[0]) * (y[1] - y[0]);
}

/* A polygon vertex while clipping: view space and attributes */
struct cv
{
    int32_t x, y, z, u, v, s;
};

static int32_t plane_d(const struct cv *p, int k)
{
    int32_t g = p->z * R3D_GUARD;

    switch (k)
    {
    case 0:  return p->z - R3D_NEAR;
    case 1:  return g - p->x;
    case 2:  return g + p->x;
    case 3:  return g - p->y;
    default: return g + p->y;
    }
}

static int clip_plane(struct cv *in, int n, struct cv *out, int k)
{
    int i, m = 0;

    for (i = 0; i < n; i++)
    {
        const struct cv *a = &in[i], *b = &in[i + 1 == n ? 0 : i + 1];
        int32_t da = plane_d(a, k), db = plane_d(b, k);

        if (da >= 0)
            out[m++] = *a;
        if ((da >= 0) != (db >= 0))
        {
            /* t in Q12; the products stay inside 32 bits (|coord| under
             * 2^18 in the guard band's reach) */
            int32_t t = (int32_t)(((int64_t)da << 12) / (da - db));
            struct cv *o = &out[m++];

            o->x = a->x + (int32_t)(((int64_t)(b->x - a->x) * t) >> 12);
            o->y = a->y + (int32_t)(((int64_t)(b->y - a->y) * t) >> 12);
            o->z = a->z + (int32_t)(((int64_t)(b->z - a->z) * t) >> 12);
            o->u = a->u + (((b->u - a->u) * t) >> 12);
            o->v = a->v + (((b->v - a->v) * t) >> 12);
            o->s = a->s + (((b->s - a->s) * t) >> 12);
            if (k == 0 && o->z < R3D_NEAR)
                o->z = R3D_NEAR;
        }
    }
    return m;
}

/* Tessellation: edges near the eye whose far end is more than 1.5 times
 * as deep as the near one, and longer than TESS_PX on the screen, are
 * split at their midpoints (in view space), and the triangle is cut to
 * match, so the affine texture mapping's error stays small where it
 * shows most: the PS1's answer to the same warp.  The decision belongs
 * to the edge alone, so two triangles that share an edge always split
 * it alike and no crack opens between them. */
#define TESS_NEAR   500
#define TESS_PX     12
#define TESS_DEPTH  6

static int32_t proj_x(const struct cv *p)
{
    return R3D_W / 2 + (p->x * R3D_FOCAL) / p->z;
}

static int32_t proj_y(const struct cv *p)
{
    return R3D_H / 2 - (p->y * R3D_FOCAL) / p->z;
}

static int split_edge(const struct cv *p, const struct cv *q)
{
    int32_t zmin = p->z < q->z ? p->z : q->z, zmax = p->z ^ q->z ^ zmin;
    int32_t dx = proj_x(p) - proj_x(q), dy = proj_y(p) - proj_y(q);

    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;
    return zmin < TESS_NEAR && zmax * 2 > zmin * 3 && dx + dy > TESS_PX;
}

static void mid(struct cv *m, const struct cv *p, const struct cv *q)
{
    m->x = (p->x + q->x) / 2;
    m->y = (p->y + q->y) / 2;
    m->z = (p->z + q->z) / 2;
    m->u = (p->u + q->u) / 2;
    m->v = (p->v + q->v) / 2;
    m->s = (p->s + q->s) / 2;
}

static void tess(const struct cv *a, const struct cv *b, const struct cv *c,
                 uint32_t tex, int32_t bias, int depth)
{
    int32_t x[3], y[3];
    uint8_t u[3], v[3], sh[3];
    const struct cv *p[3];
    int i, sab = 0, sbc = 0, sca = 0;

    if (depth < TESS_DEPTH)
    {
        sab = split_edge(a, b);
        sbc = split_edge(b, c);
        sca = split_edge(c, a);
    }
    if (sab | sbc | sca)
    {
        struct cv ab, bc, ca;

        /* rotate so the split edges come first: ab, then bc, then ca */
        if (!sab && !(sbc && sca))
        {
            if (sbc)
                tess(b, c, a, tex, bias, depth);
            else
                tess(c, a, b, tex, bias, depth);
            return;
        }
        if (sab && !sbc && sca)
        {
            tess(c, a, b, tex, bias, depth);
            return;
        }
        if (!sab)       /* bc and ca */
        {
            tess(b, c, a, tex, bias, depth);
            return;
        }
        mid(&ab, a, b);
        if (!sbc)               /* ab only */
        {
            tess(a, &ab, c, tex, bias, depth + 1);
            tess(&ab, b, c, tex, bias, depth + 1);
            return;
        }
        mid(&bc, b, c);
        if (!sca)               /* ab and bc */
        {
            tess(&ab, b, &bc, tex, bias, depth + 1);
            tess(a, &ab, &bc, tex, bias, depth + 1);
            tess(a, &bc, c, tex, bias, depth + 1);
            return;
        }
        mid(&ca, c, a);         /* all three */
        tess(a, &ab, &ca, tex, bias, depth + 1);
        tess(&ab, b, &bc, tex, bias, depth + 1);
        tess(&ca, &bc, c, tex, bias, depth + 1);
        tess(&ab, &bc, &ca, tex, bias, depth + 1);
        return;
    }
    p[0] = a; p[1] = b; p[2] = c;
    for (i = 0; i < 3; i++)
    {
        x[i] = proj_x(p[i]);
        y[i] = proj_y(p[i]);
        u[i] = p[i]->u < 0 ? 0 : p[i]->u > 255 ? 255 : p[i]->u;
        v[i] = p[i]->v < 0 ? 0 : p[i]->v > 255 ? 255 : p[i]->v;
        sh[i] = p[i]->s < 0 ? 0 : p[i]->s > 31 ? 31 : p[i]->s;
    }
    if (area(x, y) >= 0)
        return;                 /* collapsed by rounding */
    emit(x, y, u, v, sh, tex, a->z + b->z + c->z + bias);
}

static int needs_tess(int32_t za, int32_t zb, int32_t zc)
{
    int32_t zmin = za < zb ? za : zb, zmax = za > zb ? za : zb;

    if (zc < zmin) zmin = zc;
    if (zc > zmax) zmax = zc;
    return zmin < TESS_NEAR && zmax * 2 > zmin * 3;
}

static void clipped(const struct r3d_xv *const *xv, const struct r3d_tri *t,
                    const uint8_t *s, uint32_t planes)
{
    struct cv a[9], b[9];
    int n = 3, k, i;
    int32_t x[3], y[3];
    uint8_t u[3], v[3], sh[3];

    for (i = 0; i < 3; i++)
    {
        a[i].x = xv[i]->x;
        a[i].y = xv[i]->y;
        a[i].z = xv[i]->z;
        a[i].u = t->uv[2 * i];
        a[i].v = t->uv[2 * i + 1];
        a[i].s = s[i];
    }
    for (k = 0; k < 5 && n >= 3; k++)
    {
        /* a near crossing makes every plane suspect: test them all */
        if (!(planes & (1u << k)) && !(planes & R3D_OUT_NEAR))
            continue;
        n = clip_plane(a, n, b, k);
        for (i = 0; i < n; i++)
            a[i] = b[i];
    }
    if (n < 3)
    {
        stats->culled++;
        return;
    }
    stats->clipped++;
    /* project, then fan from the first vertex */
    {
        int32_t px[9], py[9];
        uint8_t pu[9], pv[9], ps[9];
        int32_t zs = a[0].z;

        for (i = 0; i < n; i++)
        {
            px[i] = R3D_W / 2 + (a[i].x * R3D_FOCAL) / a[i].z;
            py[i] = R3D_H / 2 - (a[i].y * R3D_FOCAL) / a[i].z;
            pu[i] = a[i].u;
            pv[i] = a[i].v;
            ps[i] = a[i].s < 0 ? 0 : a[i].s > 31 ? 31 : a[i].s;
        }
        for (i = 1; i + 1 < n; i++)
        {
            x[0] = px[0]; x[1] = px[i]; x[2] = px[i + 1];
            y[0] = py[0]; y[1] = py[i]; y[2] = py[i + 1];
            if (area(x, y) >= 0)
                continue;
            if (needs_tess(a[0].z, a[i].z, a[i + 1].z))
            {
                in_tess = 1;
                tess(&a[0], &a[i], &a[i + 1], t->tex, t->bias, 0);
                in_tess = 0;
                continue;
            }
            u[0] = pu[0]; u[1] = pu[i]; u[2] = pu[i + 1];
            v[0] = pv[0]; v[1] = pv[i]; v[2] = pv[i + 1];
            sh[0] = ps[0]; sh[1] = ps[i]; sh[2] = ps[i + 1];
            emit(x, y, u, v, sh, t->tex, zs + a[i].z + a[i + 1].z + t->bias);
        }
    }
}

static void setup(uint32_t i, uint32_t end)
{
    const struct r3d_tri *t = (const struct r3d_tri *)r3d_m.tris + i;
    const struct r3d_xv *xvs = r3d_m.xverts;

    for (; i < end; i++, t++)
    {
        const struct r3d_xv *xv[3];
        uint32_t oa, ob, oc;
        int32_t x[3], y[3];
        uint8_t s[3];

        xv[0] = &xvs[t->a];
        xv[1] = &xvs[t->b];
        xv[2] = &xvs[t->c];
        oa = xv[0]->out;
        ob = xv[1]->out;
        oc = xv[2]->out;
        if (oa & ob & oc)
        {
            stats->culled++;            /* all beyond one plane, or far */
            continue;
        }
        s[0] = (t->light * (xv[0]->fog + 1)) >> 5;
        s[1] = (t->light * (xv[1]->fog + 1)) >> 5;
        s[2] = (t->light * (xv[2]->fog + 1)) >> 5;
        if ((oa | ob | oc) & R3D_OUT_CLIP)
        {
            clipped(xv, t, s, (oa | ob | oc) & R3D_OUT_CLIP);
            continue;
        }
        x[0] = xv[0]->sx; x[1] = xv[1]->sx; x[2] = xv[2]->sx;
        y[0] = xv[0]->sy; y[1] = xv[1]->sy; y[2] = xv[2]->sy;
        if (area(x, y) >= 0)
        {
            stats->culled++;
            continue;
        }
        if (needs_tess(xv[0]->z, xv[1]->z, xv[2]->z))
        {
            struct cv c[3];
            int k;

            for (k = 0; k < 3; k++)
            {
                c[k].x = xv[k]->x;
                c[k].y = xv[k]->y;
                c[k].z = xv[k]->z;
                c[k].u = t->uv[2 * k];
                c[k].v = t->uv[2 * k + 1];
                c[k].s = s[k];
            }
            in_tess = 1;
            tess(&c[0], &c[1], &c[2], t->tex, t->bias, 0);
            in_tess = 0;
            continue;
        }
        {
            uint8_t u[3], v[3];

            u[0] = t->uv[0]; v[0] = t->uv[1];
            u[1] = t->uv[2]; v[1] = t->uv[3];
            u[2] = t->uv[4]; v[2] = t->uv[5];
            emit(x, y, u, v, s, t->tex, xv[0]->z + xv[1]->z + xv[2]->z + t->bias);
        }
    }
}

/* ---- raster ---- */

/* steeper shading than a level a pixel: flat (a sliver).  It also keeps
 * each product of a shading step and a pixel distance inside 32 bits
 * across the guard band (16,000 pixels). */
#define SMAX (1 << 16)

static int32_t grad(int32_t d1, int32_t d2, int32_t e1, int32_t e2, int32_t a)
{
    /* (d1 e2 - d2 e1) / a in 16.16; a 32-bit divide when it fits */
    int32_t n = d1 * e2 - d2 * e1;

    if (n < 32768 && n > -32768)
        return n * 65536 / a;
    return (int32_t)(((int64_t)n << 16) / a);
}

static void raster(const struct r3d_prim *p, uint16_t *fb)
{
    int32_t x0, y0, x1, y1, x2, y2, a, y, ye;
    int32_t u0_, v0_, s0_, u1, v1, s1, u2, v2, s2;
    int32_t dudx, dudy, dvdx, dvdy, dsdx, dsdy, u0, v0, s0;
    int32_t xl, dl, xs, ds1, ds2;
    const uint8_t *tex = r3d_m.tex + p->tex * (R3D_TEX_SIDE * R3D_TEX_SIDE);
    const uint16_t *clut = r3d_m.clut + R3D_SHADE_GUARD * 256;
    uint32_t pixels = 0;

    /* the corners by value, sorted by y.  (Not by index: vc4-elf-gcc
     * 6.2.1 -O2 miscompiled the index sort's clamp of y2, render3d.md.) */
    x0 = p->x[0]; y0 = p->y[0]; u0_ = p->u[0]; v0_ = p->v[0]; s0_ = p->s[0];
    x1 = p->x[1]; y1 = p->y[1]; u1 = p->u[1]; v1 = p->v[1]; s1 = p->s[1];
    x2 = p->x[2]; y2 = p->y[2]; u2 = p->u[2]; v2 = p->v[2]; s2 = p->s[2];
#define SWAP(a, b) do { int32_t t_ = a; a = b; b = t_; } while (0)
#define CORNER_SWAP(i, j) do { SWAP(x##i, x##j); SWAP(y##i, y##j); \
        SWAP(u##i, u##j); SWAP(v##i, v##j); SWAP(s##i, s##j); } while (0)
#define u0 u0_
#define v0 v0_
#define s0 s0_
    if (y1 < y0)
        CORNER_SWAP(0, 1);
    if (y2 < y1)
        CORNER_SWAP(1, 2);
    if (y1 < y0)
        CORNER_SWAP(0, 1);
#undef u0
#undef v0
#undef s0
    if (y0 == y2 || y2 <= 0 || y0 >= R3D_H)
        return;
    ye = opaque(R3D_H);
    if (y2 < ye)
        ye = y2;
    a = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0);
    if (!a)
        return;
    /* attribute planes, from corner 0 */
    {
        int32_t du1 = u1 - u0_, du2 = u2 - u0_;
        int32_t dv1 = v1 - v0_, dv2 = v2 - v0_;
        int32_t dd1 = s1 - s0_, dd2 = s2 - s0_;
        int32_t ex1 = x1 - x0, ex2 = x2 - x0, ey1 = y1 - y0, ey2 = y2 - y0;

        dudx = grad(du1, du2, ey1, ey2, a);
        dvdx = grad(dv1, dv2, ey1, ey2, a);
        dsdx = grad(dd1, dd2, ey1, ey2, a);
        dudy = grad(du2, du1, ex2, ex1, a);
        dvdy = grad(dv2, dv1, ex2, ex1, a);
        dsdy = grad(dd2, dd1, ex2, ex1, a);
        u0 = u0_ * 65536;
        v0 = v0_ * 65536;
        s0 = s0_ * 65536;
        if (dsdx > SMAX || dsdx < -SMAX || dsdy > SMAX || dsdy < -SMAX)
        {
            /* a sliver: flat, at the mean */
            s0 = ((s0_ + s1 + s2) * 21845) & ~0xFFFF;
            dsdx = dsdy = 0;
        }
    }
    /* edges: the long one 0-2, the short ones 0-1 and 1-2 */
    dl = (x2 - x0) * 65536 / (y2 - y0);
    ds1 = y1 > y0 ? (x1 - x0) * 65536 / (y1 - y0) : 0;
    ds2 = y2 > y1 ? (x2 - x1) * 65536 / (y2 - y1) : 0;
    y = y0 < 0 ? 0 : y0;
    xl = x0 * 65536 + dl * (y - y0);
    for (; y < ye; y++, xl += dl)
    {
        int32_t xr, left, right, xe, n, dy = y - y0;
        uint32_t u, v;
        int32_t s;
        uint16_t *d;

        xs = y < y1 ? x0 * 65536 + ds1 * dy : x1 * 65536 + ds2 * (y - y1);
        /* a > 0: vertex 1 right of the long edge */
        if (a > 0)
            left = xl, right = xs;
        else
            left = xs, right = xl;
        xr = (left + 0xFFFF) >> 16;
        xe = (right + 0xFFFF) >> 16;
        if (xr < 0)
            xr = 0;
        if (xe > R3D_W)
            xe = R3D_W;
        n = xe - xr;
        if (n <= 0)
            continue;
        u = u0 + dudx * (xr - x0) + dudy * dy;
        v = v0 + dvdx * (xr - x0) + dvdy * dy;
        s = s0 + dsdx * (xr - x0) + dsdy * dy;
        {
            int32_t lo = opaque(-((R3D_SHADE_GUARD - 1) << 16));
            int32_t hi = opaque((R3D_SHADES + R3D_SHADE_GUARD - 2) << 16);

            if (s < lo)
                s = lo;
            if (s > hi)
                s = hi;
        }
        d = fb + y * R3D_W + xr;
        pixels += n;
        {
            /* The pixel loop.  u and v go from 16.16 to 6.26: the
             * texture's 64-texel wrap is then the 32-bit wrap, so a texel
             * index is shifts alone (no mask constant to reload each
             * pixel, which vc2cc must build in r15), with the same bits
             * as 16.16 and a mask of 63.  The shade row is likewise
             * (s >> 16) << 8, not a mask: 14 instructions a pixel, was 17. */
            uint32_t uu = u << 10, vv = v << 10;
            uint32_t du = (uint32_t)dudx << 10, dv = (uint32_t)dvdx << 10;
            int32_t dss = dsdx;

            do
            {
                uint32_t ti = (vv >> 26) << 6 | uu >> 26;

                *d++ = clut[((s >> 16) << 8) + tex[ti]];
                uu += du;
                vv += dv;
                s += dss;
            } while (--n);
        }
    }
    stats->pixels += pixels;
}

/* ---- the frame ---- */

void r3d_render(uint32_t t_ms, uint16_t *fb, struct r3d_stats *st)
{
    struct r3d_cam cam;
    uint32_t t0 = now(), t1, b, k;
    uint16_t *ot = r3d_m.ot;
    const struct r3d_prim *prims = r3d_m.prims;

    stats = st;
    st->tris = r3d_ntris;
    st->drawn = st->culled = st->clipped = st->pixels = 0;
    r3d_camera(t_ms, &cam);
    sky(&cam, fb);
    for (b = 0; b < R3D_OT; b++)
        ot[b] = 0;
    nprims = 0;
    st->chunks = st->chunks_culled = 0;
    for (k = 0; k < r3d_nchunks; k++)
    {
        const struct r3d_chunk *ch = &r3d_chunks[k];

        if (!in_view(&cam, ch))
        {
            st->chunks_culled++;
            st->culled += ch->t1 - ch->t0;
            continue;
        }
        st->chunks++;
        transform(&cam, ch->v0, ch->v1);
        setup(ch->t0, ch->t1);
    }
    t1 = now();
    for (b = R3D_OT; b-- > 0;)
    {
        uint32_t i = ot[b];

        while (i)
        {
            const struct r3d_prim *p = &prims[i - 1];

            raster(p, fb);
            i = p->next;
        }
    }
    st->drawn = nprims;
    st->setup_us = t1 - t0;
    st->raster_us = now() - t1;
}

uint32_t r3d_crc(const uint16_t *fb)
{
    const uint32_t *w = (const uint32_t *)fb;
    uint32_t h = 0x811C9DC5u, n;

    for (n = 0; n < R3D_W * R3D_H / 2; n++)
        h = (h ^ w[n]) * 0x01000193u;
    return h;
}

#ifdef R3D_SIM
/* the simulator test: one screen triangle alone; its pixel count */
uint32_t r3d_sim_raster(const struct r3d_prim *p, uint16_t *fb, struct r3d_stats *st)
{
    stats = st;
    st->pixels = 0;
    raster(p, fb);
    return st->pixels;
}
#endif
