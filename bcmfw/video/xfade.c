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
 * The drift offload's cross-fade (docs/research/bcm2722/offload.md):
 * one step of the 160 x 240 panel through xfade_blk, bit-exact to the
 * shell's xfade_rect, and the RGB565 -> planes unpacking it needs; the
 * 8b half step (the average of two windows, AVG2) and the staging copy.
 * Linked into the drift image (drift.c); xfade_test.py runs it in vc2sim.
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
#include "xfade.h"

#ifndef BCMFW_DRIFT_REP_STORE
#define BCMFW_DRIFT_REP_STORE 0     /* 1: the REP 16 store kernel */
#endif

void xfade_blk(uint16_t *out, uint16_t *state, const uint16_t *src, int w);
void xfade_blk_rep(uint16_t *out, uint16_t *state, const uint16_t *src, int w);
void half_blk(uint16_t *panel, const uint16_t *stg);
void copy_blk(uint16_t *panel, const uint16_t *stg);

/* A 160 x 240 window of RGB565 pixels ('pitch' pixels apart) into the
 * three 16-bit planes the kernel reads */
void xfade_unpack(uint16_t *planes, const uint16_t *px, int pitch)
{
    uint16_t *r = planes, *g = planes + XF_PLANE / 2, *b = planes + XF_PLANE;
    int x, y;

    for (y = 0; y < XF_H; y++, px += pitch)
    {
        for (x = 0; x < XF_W; x++)
        {
            unsigned p = px[x];

            r[x] = (uint16_t)(p >> 11);
            g[x] = (uint16_t)((p >> 5) & 63);
            b[x] = (uint16_t)(p & 31);
        }
        r += XF_W;
        g += XF_W;
        b += XF_W;
    }
}

/* One step toward src: weight 1..255 as nav_xfade_weight() gives it
 * (256, an exact copy, is the caller's window copy).  out: the panel's
 * top left in the LCD framebuffer. */
void xfade_panel(uint16_t *out, uint16_t *state, const uint16_t *src, int weight)
{
    int w32 = (weight + 4) >> 3;
    int x, y;

    if (w32 == 0)
        w32 = 1;
    for (y = 0; y < XF_H; y += 16)
    {
        for (x = 0; x < XF_W; x += 16)
        {
            uint16_t *o = (uint16_t *)((char *)out + y * XF_OUT_PITCH + 2 * x);
            int k = y * XF_W + x;

#if BCMFW_DRIFT_REP_STORE
            xfade_blk_rep(o, state + k, src + k, w32);
#else
            xfade_blk(o, state + k, src + k, w32);
#endif
        }
    }
}

/* A 160 x 240 window ('pitch' pixels apart, any alignment) into the
 * staging buffer (160 pixels a row, 32-byte aligned) */
void xfade_stage(uint16_t *stg, const uint16_t *px, int pitch)
{
    int x, y;

    for (y = 0; y < XF_H; y++, px += pitch, stg += XF_W)
        for (x = 0; x < XF_W; x++)
            stg[x] = px[x];
}

/* The 8b half step: the panel (out, the window on screen) averaged with
 * the staged next window, per channel (preview.c AVG2) */
void xfade_half(uint16_t *out, const uint16_t *stg)
{
    int x, y;

    for (y = 0; y < XF_H; y += 16)
        for (x = 0; x < XF_W; x += 16)
            half_blk((uint16_t *)((char *)out + y * XF_OUT_PITCH + 2 * x),
                     stg + y * XF_W + x);
}

/* The staged window onto the panel */
void xfade_copy(uint16_t *out, const uint16_t *stg)
{
    int x, y;

    for (y = 0; y < XF_H; y += 16)
        for (x = 0; x < XF_W; x += 16)
            copy_blk((uint16_t *)((char *)out + y * XF_OUT_PITCH + 2 * x),
                     stg + y * XF_W + x);
}
