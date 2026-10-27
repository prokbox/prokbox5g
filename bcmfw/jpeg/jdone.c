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

/* One output of jdec from memory, the whole resized picture: the entry
 * the vc2sim test calls and jdtest.c's first mode (jpeg-spike.md S2/S3).
 * JDEC_OK or a JDEC_E_* code. */
#include "jdec.h"
#ifdef __vc4__
#include "jdec_vpu.h"   /* the colour stage and the 8x8 IDCT on the vector
                           unit, as the display-only image runs them */
#ifndef JDEC_VPU_IDCT
#define JDEC_VPU_IDCT 1     /* 0: the IDCT in C (config.h BCMFW_JDEC_VPU_IDCT) */
#endif
/* once: the IDCT's check runs at the first decode, as at the image's start */
static int hooked;
#define JDEC_HOOKS() (hooked ? (void)0 : (void)(hooked = 1, \
                                                jdec_vpu_init(JDEC_VPU_IDCT, 0)))
#else
#define JDEC_HOOKS() ((void)0)
#endif

int jdec_one(const uint8_t *jpg, uint32_t len, uint16_t *out, uint32_t dw,
             uint32_t dh, void *work, uint32_t work_bytes)
{
    struct jdec_src src;
    struct jdec_mem m;
    struct jdec_out o;

    JDEC_HOOKS();
    jdec_mem_src(&src, &m, jpg, len);
    o.dw = o.cw = dw;
    o.dh = o.ch = dh;
    o.cx = o.cy = 0;
    o.px = out;
    o.pitch = dw;
    return jdec_run(&src, &o, 1, work, work_bytes, 0);
}

/* n outputs of jdec from memory in one call (decode-once), each the whole
 * resized picture wh[k] = dw | dh << 16, packed one after another at out:
 * the entry the vc2sim test calls for the art set */
int jdec_many(const uint8_t *jpg, uint32_t len, uint16_t *out,
              const uint32_t *wh, int n, void *work, uint32_t work_bytes)
{
    struct jdec_src src;
    struct jdec_mem m;
    struct jdec_out o[JDEC_MAX_OUT];
    int k;

    if (n < 1 || n > JDEC_MAX_OUT)
        return JDEC_E_RANGE;
    JDEC_HOOKS();
    jdec_mem_src(&src, &m, jpg, len);
    for (k = 0; k < n; k++)
    {
        o[k].dw = o[k].cw = wh[k] & 0xFFFF;
        o[k].dh = o[k].ch = wh[k] >> 16;
        o[k].cx = o[k].cy = 0;
        o[k].px = out;
        o[k].pitch = o[k].dw;
        out += (uint32_t)o[k].dw * o[k].dh;
    }
    return jdec_run(&src, o, n, work, work_bytes, 0);
}
