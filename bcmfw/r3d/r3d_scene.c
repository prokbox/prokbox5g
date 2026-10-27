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

/* The test scenes, their textures and their camera paths: all made here
 * from integer formulas, so the image carries no art and the simulator
 * and the chip build the same bytes.  Four scenes of about 200, 450,
 * 1,000 and 2,200 triangles drawn a frame, each with a 10 s camera loop,
 * and a fifth: the village again, walked through at eye height. */

uint32_t r3d_nverts, r3d_ntris, r3d_nchunks;
struct r3d_chunk r3d_chunks[R3D_MAX_CHUNKS];
int r3d_scene_id;
uint16_t r3d_sky_top, r3d_sky_fog, r3d_sky_low;

static int16_t sine[257];           /* a quarter turn, Q14 */

/* ---- maths ---- */

int32_t r3d_sin(int32_t a)
{
    a &= 1023;
    if (a < 256)
        return sine[a];
    if (a < 512)
        return sine[512 - a];
    if (a < 768)
        return -sine[a - 512];
    return -sine[1024 - a];
}

int32_t r3d_cos(int32_t a)
{
    return r3d_sin(a + 256);
}

uint32_t r3d_isqrt(uint32_t v)
{
    uint32_t r = 0, b = 1u << 30;

    while (b > v)
        b >>= 2;
    while (b)
    {
        if (v >= r + b)
        {
            v -= r + b;
            r = (r >> 1) + b;
        }
        else
            r >>= 1;
        b >>= 2;
    }
    return r;
}

static void sine_init(void)
{
    /* Taylor series in Q28 to the x^9 term: under half a Q14 step */
    const int64_t half_pi = 421657428;          /* pi / 2 in Q28 */
    int i;

    for (i = 0; i <= 256; i++)
    {
        int64_t x = half_pi * i / 256, x2 = (x * x) >> 28;
        int64_t term = x, sum = x;
        int k;

        for (k = 1; k <= 4; k++)
        {
            term = -((term * x2) >> 28) / ((2 * k) * (2 * k + 1));
            sum += term;
        }
        sine[i] = (int16_t)((sum + (1 << 13)) >> 14);
    }
}

/* Scale a vector into Q14 */
static void norm(int32_t *v)
{
    int32_t a = v[0], b = v[1], c = v[2];
    uint32_t len;

    while (a > 30000 || a < -30000 || b > 30000 || b < -30000 ||
           c > 30000 || c < -30000)
    {
        a >>= 1; b >>= 1; c >>= 1;
    }
    while (a < 4096 && a > -4096 && b < 4096 && b > -4096 &&
           c < 4096 && c > -4096 && (a || b || c))
    {
        a <<= 1; b <<= 1; c <<= 1;
    }
    len = r3d_isqrt((uint32_t)(a * a) + (uint32_t)(b * b) + (uint32_t)(c * c));
    if (!len)
    {
        v[0] = v[2] = 0;
        v[1] = 16384;
        return;
    }
    v[0] = a * 16384 / (int32_t)len;
    v[1] = b * 16384 / (int32_t)len;
    v[2] = c * 16384 / (int32_t)len;
}

void r3d_look(struct r3d_cam *c, int32_t x, int32_t y, int32_t z,
              int32_t tx, int32_t ty, int32_t tz)
{
    int32_t f[3], r[3];

    c->x = x;
    c->y = y;
    c->z = z;
    f[0] = tx - x;
    f[1] = ty - y;
    f[2] = tz - z;
    norm(f);
    /* right = f x up (horizontal); in a right-handed world, looking down
     * +z with y up, the screen's right is -x */
    r[0] = -f[2];
    r[1] = 0;
    r[2] = f[0];
    norm(r);
    c->m[0] = r[0]; c->m[1] = r[1]; c->m[2] = r[2];
    /* up = r x f */
    c->m[3] = (r[1] * f[2] - r[2] * f[1]) >> 14;
    c->m[4] = (r[2] * f[0] - r[0] * f[2]) >> 14;
    c->m[5] = (r[0] * f[1] - r[1] * f[0]) >> 14;
    c->m[6] = f[0]; c->m[7] = f[1]; c->m[8] = f[2];
}

/* ---- textures and colours ---- */

static uint32_t hash(uint32_t x, uint32_t y, uint32_t s)
{
    uint32_t h = x * 0x27D4EB2Du ^ y * 0x165667B1u ^ s * 0x9E3779B9u;

    h ^= h >> 15;
    h *= 0x85EBCA6Bu;
    h ^= h >> 13;
    return h;
}

/* Tileable value noise over a 64-texel square, 0..255, cells of c */
static uint32_t noise(uint32_t x, uint32_t y, uint32_t c, uint32_t s)
{
    uint32_t n = R3D_TEX_SIDE / c;
    uint32_t gx = x / c, gy = y / c, fx = x % c, fy = y % c;
    uint32_t a = hash(gx % n, gy % n, s) & 255;
    uint32_t b = hash((gx + 1) % n, gy % n, s) & 255;
    uint32_t d = hash(gx % n, (gy + 1) % n, s) & 255;
    uint32_t e = hash((gx + 1) % n, (gy + 1) % n, s) & 255;
    uint32_t top = a * (c - fx) + b * fx, bot = d * (c - fx) + e * fx;

    return (top * (c - fy) + bot * fy) / (c * c);
}

enum { T_GRASS, T_STONE, T_BRICK, T_ROOF, T_CRATE, T_ROCK, T_BARK, T_LEAF };

static const uint8_t ramp_rgb[R3D_TEX_N][3] =
{
    { 70, 135, 50 },    /* grass */
    { 150, 148, 138 },  /* flagstones */
    { 165, 75, 52 },    /* brick */
    { 150, 55, 45 },    /* roof tiles */
    { 160, 112, 62 },   /* crate planks */
    { 120, 110, 98 },   /* rock */
    { 92, 62, 38 },     /* bark */
    { 42, 112, 48 },    /* leaves */
};

static uint32_t clamp31(int32_t v)
{
    return v < 0 ? 0 : v > 31 ? 31 : v;
}

/* Texel brightness 0..31 of texture t at (x, y) */
static uint32_t texel(int t, uint32_t x, uint32_t y)
{
    uint32_t n1 = noise(x, y, 4, t + 1), n2 = noise(x, y, 16, t + 9);
    uint32_t h = hash(x, y, t + 17) & 7;

    switch (t)
    {
    case T_GRASS:
        return clamp31(10 + (n2 >> 4) + (n1 >> 5) + h - 4);
    case T_STONE:
    {
        /* 16x16 flagstones, mortar 1 texel, each stone its own shade */
        uint32_t edge = (x & 15) == 0 || (y & 15) == 0;
        uint32_t tone = hash(x >> 4, y >> 4, 5) & 7;

        return edge ? 5 : clamp31(12 + tone + (n1 >> 5) + (h >> 1) - 2);
    }
    case T_BRICK:
    {
        uint32_t row = y >> 3, bx = (x + (row & 1) * 8) & 15;
        uint32_t edge = (y & 7) == 0 || bx == 0;

        return edge ? 22 : clamp31(10 + (hash((x + (row & 1) * 8) >> 4, row, 3) & 7) +
                                   (n1 >> 6) + (h >> 1));
    }
    case T_ROOF:
    {
        /* rows of tiles, darker toward each row's top edge */
        uint32_t ry = y & 7, col = (x + ((y >> 3) & 1) * 4) & 7;

        return clamp31(6 + ry * 2 + (col == 0 ? -4 : 0) + (n1 >> 6));
    }
    case T_CRATE:
    {
        /* a frame, vertical planks and one brace */
        uint32_t frame = x < 5 || x > 58 || y < 5 || y > 58;
        int32_t dxy = (int32_t)x - (int32_t)y;
        uint32_t brace = dxy > -5 && dxy < 5;
        uint32_t seam = (x & 15) == 0;

        if (frame || brace)
            return clamp31(20 + (n1 >> 5) + (h >> 1) - ((x == 5 || y == 5) ? 8 : 0));
        return clamp31((seam ? 6 : 13) + (n2 >> 5) + (h >> 1));
    }
    case T_ROCK:
        return clamp31(6 + (n2 >> 4) + (n1 >> 4) + (h >> 1));
    case T_BARK:
        return clamp31(8 + (noise(x, y >> 3, 4, 31) >> 4) + (h >> 1) - ((x & 7) == 0 ? 5 : 0));
    default:            /* leaves */
        return clamp31(4 + (n1 >> 3) + (hash(x >> 1, y >> 1, 41) & 7));
    }
}

static uint32_t rgb565(uint32_t r, uint32_t g, uint32_t b)
{
    return (r >> 3) << 11 | (g >> 2) << 5 | b >> 3;
}

static const uint8_t fog_rgb[3] = { 150, 112, 122 };     /* dusk haze */

static void colours(void)
{
    uint32_t t, i, row;

    r3d_sky_top = rgb565(22, 30, 72);
    r3d_sky_fog = rgb565(fog_rgb[0], fog_rgb[1], fog_rgb[2]);
    r3d_sky_low = rgb565(70, 52, 60);
    for (row = 0; row < R3D_CLUT_ROWS; row++)
    {
        int32_t l = row < R3D_SHADE_GUARD ? 0 : row - R3D_SHADE_GUARD;

        if (l > 31)
            l = 31;
        for (t = 0; t < R3D_TEX_N; t++)
            for (i = 0; i < 32; i++)
            {
                uint32_t c[3], k;

                for (k = 0; k < 3; k++)
                {
                    uint32_t v = ramp_rgb[t][k] * (8 + i) / 28;

                    if (v > 255)
                        v = 255;
                    c[k] = (fog_rgb[k] * (31 - l) + v * l) / 31;
                }
                r3d_m.clut[row * 256 + t * 32 + i] = rgb565(c[0], c[1], c[2]);
            }
    }
}

void r3d_init(const struct r3d_mem *m)
{
    uint32_t t, x, y;

    r3d_m = *m;
    sine_init();
    for (t = 0; t < R3D_TEX_N; t++)
        for (y = 0; y < R3D_TEX_SIDE; y++)
            for (x = 0; x < R3D_TEX_SIDE; x++)
                m->tex[(t * R3D_TEX_SIDE + y) * R3D_TEX_SIDE + x] =
                    t * 32 + texel(t, x, y);
    colours();
    r3d_nverts = r3d_ntris = 0;
    r3d_scene_id = -1;
}

/* ---- scene building ---- */

static struct r3d_vtx *V;
static struct r3d_tri *T;

static uint32_t vert(int32_t x, int32_t y, int32_t z)
{
    if (r3d_nverts >= R3D_MAX_VERTS)
        return R3D_MAX_VERTS - 1;
    V[r3d_nverts].x = x;
    V[r3d_nverts].y = y;
    V[r3d_nverts].z = z;
    V[r3d_nverts].pad = 0;
    return r3d_nverts++;
}

static void cross(uint32_t a, uint32_t b, uint32_t c, int32_t *n)
{
    int32_t ux = V[b].x - V[a].x, uy = V[b].y - V[a].y, uz = V[b].z - V[a].z;
    int32_t vx = V[c].x - V[a].x, vy = V[c].y - V[a].y, vz = V[c].z - V[a].z;

    n[0] = uy * vz - uz * vy;
    n[1] = uz * vx - ux * vz;
    n[2] = ux * vy - uy * vx;
}

/* A triangle, wound to face (rx, ry, rz) */
static void tri(uint32_t a, uint32_t b, uint32_t c, const uint8_t *uv,
                int tex, int32_t rx, int32_t ry, int32_t rz)
{
    struct r3d_tri *t;
    int32_t n[3];

    if (r3d_ntris >= R3D_MAX_TRIS)
        return;
    t = &T[r3d_ntris++];
    cross(a, b, c, n);
    if ((int64_t)n[0] * rx + (int64_t)n[1] * ry + (int64_t)n[2] * rz >= 0)
    {
        t->a = a; t->b = b; t->c = c;
        t->uv[0] = uv[0]; t->uv[1] = uv[1];
        t->uv[2] = uv[2]; t->uv[3] = uv[3];
        t->uv[4] = uv[4]; t->uv[5] = uv[5];
    }
    else
    {
        t->a = a; t->b = c; t->c = b;
        t->uv[0] = uv[0]; t->uv[1] = uv[1];
        t->uv[2] = uv[4]; t->uv[3] = uv[5];
        t->uv[4] = uv[2]; t->uv[5] = uv[3];
    }
    t->tex = tex;
    t->light = 31;
    t->bias = 0;
}

/* Close the current chunk: the vertices and triangles added since the
 * last one, with a bounding sphere for the frame's view test */
static uint32_t chunk_v, chunk_t;

static void chunk(void)
{
    struct r3d_chunk *k;
    int32_t lo[3] = { 32767, 32767, 32767 }, hi[3] = { -32768, -32768, -32768 };
    uint32_t i, r = 0;

    if (r3d_ntris == chunk_t || r3d_nchunks >= R3D_MAX_CHUNKS)
        return;
    for (i = chunk_v; i < r3d_nverts; i++)
    {
        const int16_t *p = &V[i].x;
        int k3;

        for (k3 = 0; k3 < 3; k3++)
        {
            if (p[k3] < lo[k3]) lo[k3] = p[k3];
            if (p[k3] > hi[k3]) hi[k3] = p[k3];
        }
    }
    k = &r3d_chunks[r3d_nchunks++];
    k->v0 = chunk_v;
    k->v1 = r3d_nverts;
    k->t0 = chunk_t;
    k->t1 = r3d_ntris;
    k->cx = (lo[0] + hi[0]) / 2;
    k->cy = (lo[1] + hi[1]) / 2;
    k->cz = (lo[2] + hi[2]) / 2;
    for (i = chunk_v; i < r3d_nverts; i++)
    {
        int32_t dx = V[i].x - k->cx, dy = V[i].y - k->cy, dz = V[i].z - k->cz;
        uint32_t d = r3d_isqrt((uint32_t)(dx * dx + dy * dy + dz * dz)) + 1;

        if (d > r)
            r = d;
    }
    k->r = r;
    chunk_v = r3d_nverts;
    chunk_t = r3d_ntris;
}

static uint8_t clamp_uv(int32_t v)
{
    return v < 0 ? 0 : v > 255 ? 255 : v;
}

/* A quad a-b-c-d around its edge, U x V texels, facing (rx, ry, rz) */
static void quad(uint32_t a, uint32_t b, uint32_t c, uint32_t d, int tex,
                 int32_t u, int32_t v, int32_t rx, int32_t ry, int32_t rz)
{
    uint8_t uv1[6], uv2[6];
    uint8_t U = clamp_uv(u), W = clamp_uv(v);

    uv1[0] = 0; uv1[1] = 0; uv1[2] = U; uv1[3] = 0; uv1[4] = U; uv1[5] = W;
    uv2[0] = 0; uv2[1] = 0; uv2[2] = U; uv2[3] = W; uv2[4] = 0; uv2[5] = W;
    tri(a, b, c, uv1, tex, rx, ry, rz);
    tri(a, c, d, uv2, tex, rx, ry, rz);
}

/* An axis-aligned box without its bottom; crate = one texture per face */
static void box(int32_t x0, int32_t y0, int32_t z0, int32_t x1, int32_t y1,
                int32_t z1, int tex, int crate)
{
    uint32_t b0 = vert(x0, y0, z0), b1 = vert(x1, y0, z0);
    uint32_t b2 = vert(x1, y0, z1), b3 = vert(x0, y0, z1);
    uint32_t t0 = vert(x0, y1, z0), t1 = vert(x1, y1, z0);
    uint32_t t2 = vert(x1, y1, z1), t3 = vert(x0, y1, z1);
    int32_t w = x1 - x0, h = y1 - y0, d = z1 - z0;
    int32_t uw = crate ? 63 : w, uh = crate ? 63 : h, ud = crate ? 63 : d;

    quad(b0, b1, t1, t0, tex, uw, uh, 0, 0, -1);     /* -z */
    quad(b2, b3, t3, t2, tex, uw, uh, 0, 0, 1);      /* +z */
    quad(b1, b2, t2, t1, tex, ud, uh, 1, 0, 0);      /* +x */
    quad(b3, b0, t0, t3, tex, ud, uh, -1, 0, 0);     /* -x */
    quad(t0, t1, t2, t3, tex, uw, ud, 0, 1, 0);      /* top */
    chunk();
}

/* An n-sided prism around (cx, cz), from y0 to y1; cap: a top */
static void prism(int32_t cx, int32_t cz, int32_t y0, int32_t y1, int32_t r,
                  int n, int tex, int cap)
{
    uint32_t base = r3d_nverts, top;
    int i;

    for (i = 0; i < n; i++)
    {
        int32_t a = i * 1024 / n;

        vert(cx + (r * r3d_cos(a) >> 14), y0, cz + (r * r3d_sin(a) >> 14));
        vert(cx + (r * r3d_cos(a) >> 14), y1, cz + (r * r3d_sin(a) >> 14));
    }
    for (i = 0; i < n; i++)
    {
        int j = (i + 1) % n;
        int32_t a = (2 * i + 1) * 512 / n;

        quad(base + 2 * i, base + 2 * j, base + 2 * j + 1, base + 2 * i + 1, tex,
             2 * r * 3 / n + 8, y1 - y0, r3d_cos(a), 0, r3d_sin(a));
    }
    if (!cap)
    {
        chunk();
        return;
    }
    top = vert(cx, y1, cz);
    for (i = 0; i < n; i++)
    {
        static const uint8_t uv[6] = { 32, 32, 0, 0, 63, 0 };

        tri(top, base + 2 * i + 1, base + 2 * ((i + 1) % n) + 1, uv, tex, 0, 1, 0);
    }
    chunk();
}

static void cone(int32_t cx, int32_t cz, int32_t y0, int32_t y1, int32_t r,
                 int n, int tex)
{
    uint32_t base = r3d_nverts, apex;
    int i;

    for (i = 0; i < n; i++)
    {
        int32_t a = i * 1024 / n;

        vert(cx + (r * r3d_cos(a) >> 14), y0, cz + (r * r3d_sin(a) >> 14));
    }
    apex = vert(cx, y1, cz);
    for (i = 0; i < n; i++)
    {
        static const uint8_t uv[6] = { 0, 63, 63, 63, 32, 0 };
        int32_t a = (2 * i + 1) * 512 / n;

        tri(base + i, base + (i + 1) % n, apex, uv, tex,
            r3d_cos(a), (r << 14) / (y1 - y0), r3d_sin(a));
    }
    chunk();
}

static void tree(int32_t x, int32_t y, int32_t z, int32_t s)
{
    prism(x, z, y, y + s, s / 8 + 3, 4, T_BARK, 0);
    cone(x, z, y + s * 2 / 5, y + s * 2, s / 2, 6, T_LEAF);
}

/* A house: brick walls, a gabled roof along x */
static void house(int32_t x, int32_t z, int32_t w, int32_t d, int32_t h)
{
    int32_t x0 = x - w / 2, x1 = x + w / 2, z0 = z - d / 2, z1 = z + d / 2;
    int32_t e = 10, rh = d / 2;
    uint32_t b0 = vert(x0, 0, z0), b1 = vert(x1, 0, z0);
    uint32_t b2 = vert(x1, 0, z1), b3 = vert(x0, 0, z1);
    uint32_t t0 = vert(x0, h, z0), t1 = vert(x1, h, z0);
    uint32_t t2 = vert(x1, h, z1), t3 = vert(x0, h, z1);
    uint32_t r0, r1, e0, e1, e2, e3;
    static const uint8_t gable[6] = { 0, 63, 63, 63, 32, 20 };

    quad(b0, b1, t1, t0, T_BRICK, w, h, 0, 0, -1);
    quad(b2, b3, t3, t2, T_BRICK, w, h, 0, 0, 1);
    quad(b1, b2, t2, t1, T_BRICK, d, h, 1, 0, 0);
    quad(b3, b0, t0, t3, T_BRICK, d, h, -1, 0, 0);
    r0 = vert(x0 - e, h + rh, z);
    r1 = vert(x1 + e, h + rh, z);
    e0 = vert(x0 - e, h - 6, z0 - e);
    e1 = vert(x1 + e, h - 6, z0 - e);
    e2 = vert(x1 + e, h - 6, z1 + e);
    e3 = vert(x0 - e, h - 6, z1 + e);
    quad(e0, e1, r1, r0, T_ROOF, w, rh * 3 / 2, 0, 1, -1);
    quad(e2, e3, r0, r1, T_ROOF, w, rh * 3 / 2, 0, 1, 1);
    tri(t0, t3, vert(x0, h + rh, z), gable, T_BRICK, -1, 0, 0);
    tri(t1, t2, vert(x1, h + rh, z), gable, T_BRICK, 1, 0, 0);
    chunk();
}

/* A height field of n x n cells of side c centred on the origin */
typedef int32_t (*height_fn)(int32_t i, int32_t j);

/* In blocks of R3D_BLOCK x R3D_BLOCK cells, each with its own vertices
 * and its own chunk, so a block out of view costs one sphere test */
#define R3D_BLOCK 8

/* bias: the ground's sort bias (flat ground: 0xFFFF, behind everything;
 * hills: about a cell, so what stands on a slope stays in front) */
static void terrain(int n, int32_t c, height_fn hf, uint32_t bias)
{
    int bi, bj, i, j;
    uint32_t t0 = r3d_ntris;

    for (bj = 0; bj < n; bj += R3D_BLOCK)
        for (bi = 0; bi < n; bi += R3D_BLOCK)
        {
            int w = n - bi < R3D_BLOCK ? n - bi : R3D_BLOCK;
            int h = n - bj < R3D_BLOCK ? n - bj : R3D_BLOCK;
            uint32_t base = r3d_nverts;

            for (j = bj; j <= bj + h; j++)
                for (i = bi; i <= bi + w; i++)
                    vert((i - n / 2) * c, hf(i, j), (j - n / 2) * c);
            for (j = 0; j < h; j++)
                for (i = 0; i < w; i++)
                {
                    uint32_t a = base + j * (w + 1) + i, b = a + 1;
                    uint32_t d = a + w + 1, e = d + 1;
                    int32_t dh = V[a].y - V[e].y, dh2 = V[b].y - V[d].y;
                    int tex;

                    if (dh < 0)
                        dh = -dh;
                    if (dh2 < 0)
                        dh2 = -dh2;
                    tex = dh + dh2 > c ? T_ROCK : T_GRASS;
                    quad(a, d, e, b, tex, 63, 63, 0, 1, 0);
                }
            chunk();
        }
    for (; t0 < r3d_ntris; t0++)
        T[t0].bias = bias;
}

static int32_t ground_y(int n, int32_t c, height_fn hf, int32_t x, int32_t z)
{
    int32_t i = x / c + n / 2, j = z / c + n / 2;

    if (i < 0) i = 0;
    if (j < 0) j = 0;
    if (i > n) i = n;
    if (j > n) j = n;
    return hf(i, j);
}

/* Bake the light: ambient plus a low sun from the west-south-west */
static void light(void)
{
    int32_t sun[3] = { -9000, 9000, -5000 };
    uint32_t i;

    norm(sun);
    for (i = 0; i < r3d_ntris; i++)
    {
        struct r3d_tri *t = &T[i];
        int32_t n[3], d;

        cross(t->a, t->b, t->c, n);
        norm(n);
        d = (n[0] * sun[0] + n[1] * sun[1] + n[2] * sun[2]) >> 14;
        t->light = 11 + (d > 0 ? d * 20 / 16384 : 0);
    }
}

/* ---- the four scenes ---- */

static int32_t h_flat(int32_t i, int32_t j)
{
    (void)i; (void)j;
    return 0;
}

static int32_t h_valley(int32_t i, int32_t j)
{
    return (90 * r3d_sin(i * 56) >> 14) + (70 * r3d_cos(j * 44 + 100) >> 14) +
           (int32_t)(hash(i, j, 7) & 15) + 60;
}

static int32_t h_ruins(int32_t i, int32_t j)
{
    int32_t d = j - 17;     /* a valley along x */

    return (d * d * 3) / 2 + (40 * r3d_sin(i * 72 + j * 20) >> 14) +
           (int32_t)(hash(i, j, 11) & 15);
}

static void courtyard(void)
{
    static const int16_t crates[6][3] = {
        { -150, 0, -110 }, { -110, 0, -150 }, { 130, 0, 120 },
        { -60, 0, 160 }, { 170, 0, -60 }, { -128, 50, -128 },
    };
    int i;

    terrain(8, 64, h_flat, 0xFFFF);
    for (i = 0; i < 64 * 2; i++)
        T[i].tex = T_STONE;
    for (i = 0; i < 6; i++)
    {
        int32_t x = crates[i][0], y = crates[i][1], z = crates[i][2];

        box(x - 25, y, z - 25, x + 25, y + 50, z + 25, T_CRATE, 1);
    }
    prism(-90, 60, 0, 180, 22, 8, T_STONE, 1);
    prism(90, -70, 0, 150, 22, 8, T_STONE, 1);
    box(-20, 0, -20, 20, 30, 20, T_STONE, 0);
    cone(0, 0, 30, 140, 22, 4, T_BRICK);
}

static void village(void)
{
    static const int16_t houses[6][4] = {
        { -300, -250, 160, 110 }, { 40, -320, 130, 100 }, { 300, -200, 150, 120 },
        { -320, 180, 140, 100 }, { 0, 300, 170, 110 }, { 320, 260, 120, 100 },
    };
    int i, n;

    terrain(10, 96, h_flat, 0xFFFF);
    for (i = 0; i < 6; i++)
        house(houses[i][0], houses[i][1], houses[i][2], houses[i][3], 90);
    for (i = 0, n = 0; i < 64 && n < 24; i++)
    {
        /* clear of the walk's ring (radius 200, scene 4) and the houses */
        int32_t a = i * 43 + 40, r = 262 + (hash(i, 1, 3) & 127);
        int32_t x = r * r3d_cos(a) >> 14, z = r * r3d_sin(a) >> 14, k;

        for (k = 0; k < 6; k++)
            if (x > houses[k][0] - houses[k][2] / 2 - 45 &&
                x < houses[k][0] + houses[k][2] / 2 + 45 &&
                z > houses[k][1] - houses[k][3] / 2 - 45 &&
                z < houses[k][1] + houses[k][3] / 2 + 45)
                break;
        if (k < 6)
            continue;
        tree(x, 0, z, 70 + (hash(i, 2, 3) & 31));
        n++;
    }
    prism(0, 0, 0, 30, 34, 8, T_STONE, 0);              /* the well */
    for (i = 0; i < 6; i++)
    {
        int32_t x = -100 + i * 40, z = 85 + (i & 1) * 14;

        box(x - 18, 0, z - 18, x + 18, 36, z + 18, T_CRATE, 1);
    }
}

static void valley(void)
{
    int i;

    terrain(24, 80, h_valley, 3 * 80);
    for (i = 0; i < 14; i++)
    {
        int32_t x = ((int32_t)(hash(i, 3, 5) % 1600)) - 800;
        int32_t z = ((int32_t)(hash(i, 4, 5) % 1600)) - 800;

        tree(x, ground_y(24, 80, h_valley, x, z) - 4, z, 90 + (hash(i, 6, 5) & 31));
    }
}

static void ruins(void)
{
    int i;

    terrain(34, 64, h_ruins, 3 * 64);
    for (i = 0; i < 16; i++)
    {
        int32_t x = -1000 + i * 130, z = (i & 1) ? -120 : 120;
        int32_t y = ground_y(34, 64, h_ruins, x, z) - 4;
        int32_t h = 120 + (hash(i, 8, 9) & 63);

        prism(x, z, y, y + h, 26, 6, T_STONE, 1);
        if (!(i % 3))
            box(x - 30, y + h, z - 30, x + 30, y + h + 18, z + 30, T_STONE, 0);
    }
    for (i = 0; i < 12; i++)
    {
        int32_t x = ((int32_t)(hash(i, 3, 13) % 2000)) - 1000;
        int32_t z = ((int32_t)(hash(i, 4, 13) % 500)) + 380;

        if (i & 1)
            z = -z;
        tree(x, ground_y(34, 64, h_ruins, x, z) - 4, z, 100 + (hash(i, 6, 13) & 31));
    }
}

uint32_t r3d_scene(int id)
{
    V = r3d_m.verts;
    T = r3d_m.tris;
    r3d_nverts = r3d_ntris = r3d_nchunks = 0;
    chunk_v = chunk_t = 0;
    r3d_scene_id = id;
    switch (id)
    {
    case 0:  courtyard(); break;
    case 1:
    case 4:  village(); break;
    case 2:  valley(); break;
    case 3:  ruins(); break;
    default: break;
    }
    chunk();                    /* anything left over */
    light();
    return r3d_ntris;
}

/* ---- camera paths: one 10 s loop each ---- */

void r3d_camera(uint32_t t_ms, struct r3d_cam *c)
{
    int32_t a = (int32_t)((t_ms % 10000) * 1024 / 10000);

    switch (r3d_scene_id)
    {
    case 0:     /* an orbit of the courtyard */
        r3d_look(c, 420 * r3d_sin(a) >> 14, 150, 420 * r3d_cos(a) >> 14, 0, 50, 0);
        break;
    case 1:     /* a walk round the green, looking ahead and in */
        r3d_look(c, 620 * r3d_sin(a) >> 14, 230, 620 * r3d_cos(a) >> 14,
                 60 * r3d_sin(a + 160) >> 14, 20, 60 * r3d_cos(a + 160) >> 14);
        break;
    case 2:     /* a high circle over the valley */
        r3d_look(c, 1150 * r3d_sin(a) >> 14, 700, 1150 * r3d_cos(a) >> 14,
                 0, 0, 0);
        break;
    case 4:     /* on foot round the well: eye height, a bob each step */
    {
        /* 110 units a second on a ring of 200: 78/1024 of a turn a
         * second; two steps a second, each a half-turn of |sin| */
        int32_t w = (int32_t)(t_ms % 13128) * 78 / 1000;
        int32_t step = (int32_t)(t_ms % 1000) * 1024 / 1000;
        int32_t bob = 5 * r3d_sin(step & 511) >> 14;
        int32_t sway = 3 * r3d_sin(step >> 1) >> 14;
        int32_t x = 200 * r3d_sin(w) >> 14, z = 200 * r3d_cos(w) >> 14;
        int32_t fx = r3d_cos(w) >> 6, fz = -r3d_sin(w) >> 6;     /* ahead */

        r3d_look(c, x + (sway * fz >> 8), 44 + bob, z - (sway * fx >> 8),
                 x + fx - (x >> 3), 40 + bob / 2, z + fz - (z >> 3));
        break;
    }
    default:    /* down the ruined valley and back */
    {
        int32_t x = 900 * r3d_sin(a) >> 14, dx = r3d_cos(a);

        r3d_look(c, x / 2, 760, 1250 + (60 * r3d_sin(2 * a) >> 14),
                 x / 3 + (dx >= 0 ? 60 : -60), 0, -100);
        break;
    }
    }
}
