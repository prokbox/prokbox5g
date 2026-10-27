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

#ifndef BCMFW_R3D_H
#define BCMFW_R3D_H

#include <stdint.h>

/* The 3D render test (BCMFW_R3D, docs/research/bcm2722/render3d.md): a
 * PS1-style software renderer on the core.  Integer screen vertices,
 * 8-bit palette textures shaded through a colour table, affine texture
 * mapping, a painter's ordering table instead of a depth buffer.  No
 * hardware access here, so the simulator test calls it directly. */

#define R3D_W           320
#define R3D_H           240
#define R3D_TEX_SIDE    64          /* textures 64x64 texels, 8-bit */
#define R3D_TEX_N       8
#define R3D_SHADES      32          /* light levels 0..31 */
#define R3D_SHADE_GUARD 8           /* clamped rows each side of them */
#define R3D_CLUT_ROWS   (R3D_SHADES + 2 * R3D_SHADE_GUARD)
#define R3D_OT          512         /* ordering-table buckets */
#define R3D_OT_SHIFT    3           /* view z >> this = bucket */
#define R3D_MAX_VERTS   2048
#define R3D_MAX_TRIS    4096
#define R3D_MAX_PRIMS   6144

#define R3D_SCENES      5

/* Memory the renderer works in (the image: fixed SRAM addresses,
 * config.h BCMFW_R3D_*; the simulator test: its own) */
struct r3d_mem
{
    uint8_t *tex;           /* R3D_TEX_N x 64 x 64 texel indices */
    uint16_t *clut;         /* R3D_CLUT_ROWS x 256 RGB565 */
    void *verts;            /* scene vertices */
    void *tris;             /* scene triangles */
    void *xverts;           /* transformed vertices */
    uint16_t *ot;           /* R3D_OT bucket heads */
    void *prims;            /* screen triangles */
};

/* Per-frame counts, for the log */
struct r3d_stats
{
    uint32_t tris;          /* the scene's triangles */
    uint32_t drawn;         /* screen triangles rasterised */
    uint32_t culled;        /* back faces and off-screen */
    uint32_t clipped;       /* triangles that crossed a clip plane */
    uint32_t pixels;        /* pixels written (span lengths) */
    uint32_t chunks;        /* chunks in view */
    uint32_t chunks_culled; /* chunks skipped whole (outside the view) */
    uint32_t setup_us;      /* sky, transform and setup (r3d_now set) */
    uint32_t raster_us;     /* the ordering table drawn */
};

/* The microsecond clock, or NULL (the simulator) */
extern uint32_t (*r3d_now)(void);

void r3d_init(const struct r3d_mem *m);
/* Build scene id (0..R3D_SCENES-1); its triangle count */
uint32_t r3d_scene(int id);
/* One frame of the current scene at t_ms on its camera path into fb
 * (320x240 RGB565) */
void r3d_render(uint32_t t_ms, uint16_t *fb, struct r3d_stats *st);
/* A checksum of a frame (FNV-1a over its words) */
uint32_t r3d_crc(const uint16_t *fb);

#endif /* BCMFW_R3D_H */
