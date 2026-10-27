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

#ifndef BCMFW_R3D_INT_H
#define BCMFW_R3D_INT_H

/* r3d.c and r3d_scene.c only: the scene's data and the frame's camera */

#include "r3d.h"

#define R3D_NEAR        24          /* the near plane, world units */
#define R3D_GUARD       40          /* guard band: |X|, |Y| <= 40 Z */
#define R3D_FOCAL       200         /* pixels; 77 degrees across */
#define R3D_FOG_NEAR    400
#define R3D_FOG_FAR     3200

struct r3d_vtx                      /* a scene vertex, world units */
{
    int16_t x, y, z, pad;
};

struct r3d_tri                      /* a scene triangle */
{
    uint16_t a, b, c;
    uint8_t uv[6];                  /* u, v per corner, texels (wrap 64) */
    uint8_t tex;
    uint8_t light;                  /* 0..31, baked at build */
    uint16_t bias;                  /* added to its sort depth (3 z): the
                                       ground draws behind what stands
                                       on it */
};

struct r3d_xv                       /* a vertex in view space */
{
    int32_t x, y, z;
    int16_t sx, sy;                 /* projected, when out == 0 */
    uint8_t fog;                    /* 0..31 */
    uint8_t out;                    /* R3D_OUT_* */
    uint16_t pad;
};

#define R3D_OUT_NEAR    0x01
#define R3D_OUT_XP      0x02
#define R3D_OUT_XN      0x04
#define R3D_OUT_YP      0x08
#define R3D_OUT_YN      0x10
#define R3D_OUT_CLIP    0x1F
#define R3D_OUT_FAR     0x20

struct r3d_prim                     /* a screen triangle */
{
    int16_t x[3], y[3];
    uint8_t u[3], v[3], s[3];
    uint8_t tex;
    uint16_t next;                  /* 1-based, 0 = end of the bucket */
};

/* A run of vertices and the triangles that use only them, with a
 * bounding sphere: an object, or a block of terrain */
#define R3D_MAX_CHUNKS  160

struct r3d_chunk
{
    uint16_t v0, v1, t0, t1;
    int16_t cx, cy, cz;
    uint16_t r;
};

/* The camera: position, and the view basis rows (Q14) */
struct r3d_cam
{
    int32_t x, y, z;
    int32_t m[9];
};

extern struct r3d_mem r3d_m;
extern uint32_t r3d_nverts, r3d_ntris, r3d_nchunks;
extern struct r3d_chunk r3d_chunks[R3D_MAX_CHUNKS];
extern int r3d_scene_id;

/* Q14 sine/cosine, angle in 1024ths of a turn */
int32_t r3d_sin(int32_t a);
int32_t r3d_cos(int32_t a);
uint32_t r3d_isqrt(uint32_t v);
/* The camera at t_ms on the current scene's path (r3d_scene.c) */
void r3d_camera(uint32_t t_ms, struct r3d_cam *c);
/* A camera at (x, y, z) looking at (tx, ty, tz) */
void r3d_look(struct r3d_cam *c, int32_t x, int32_t y, int32_t z,
              int32_t tx, int32_t ty, int32_t tz);
/* The sky's colours: zenith, horizon (= the fog), below the horizon */
extern uint16_t r3d_sky_top, r3d_sky_fog, r3d_sky_low;

#endif /* BCMFW_R3D_INT_H */
