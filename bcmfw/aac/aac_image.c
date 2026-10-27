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
 * bcmfw/aac in the video image: the far data's placement and the bcs/bcc
 * probe (aac_image.h).  Built with bcmfw's flags; the decoder itself is
 * built with aac_config.h's (build.py).  Loops count down to zero or up
 * to a count: no unsigned compares, so no bcs/bcc of our own.
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
#include "bcmfw.h"
#include "aac_image.h"

#if BCMFW_AAC

/* vc2cc's far sections (Program.FAR): .xdata's run and load addresses,
 * .sdbss's run address */
extern uint32_t __xdata_start[], __xdata_end[], __xdata_load[];
extern uint32_t __sdbss_start[], __sdbss_end[];

static uint32_t placed, placed_fnv;

static uint32_t fnv_words(const uint32_t *p, uint32_t n)
{
    uint32_t h = 2166136261u;

    for (; n != 0; n--)
        h = (h ^ *p++) * 16777619u;
    return h;
}

static uint32_t xdata_words(void)
{
    return ((uint32_t)__xdata_end - (uint32_t)__xdata_start) >> 2;
}

void aac_image_place(void)
{
    uint32_t n = xdata_words(), k;
    const uint32_t *src = (const uint32_t *)(BCMFW_AAC_PARK_END - 4 * n);
    uint32_t *dst = __xdata_start;

    if (placed)
        return;
    for (k = 0; k != n; k++)
        dst[k] = src[k];
    placed_fnv = fnv_words(src, n);
    n = ((uint32_t)__sdbss_end - (uint32_t)__sdbss_start) >> 2;
    dst = __sdbss_start;
    for (k = 0; k != n; k++)
        dst[k] = 0;
    placed = 1;
}

int aac_image_ready(void)
{
    return placed && fnv_words(__xdata_start, xdata_words()) == placed_fnv;
}

uint32_t aac_image_info(uint32_t *load, uint32_t *bytes, uint32_t *now)
{
    *load = (uint32_t)__xdata_load;
    *bytes = xdata_words() << 2;
    *now = placed ? fnv_words(__xdata_start, xdata_words()) : 0;
    return placed_fnv;
}

/* (a, b): equal, either side of 0 and of 0x80000000, all ones, and the
 * pointer-like pairs of the decoder's loops (fft-ffmpeg.c's pass(),
 * huffman.c's bounds) */
static const uint32_t probe_cases[AAC_PROBE_CASES][2] =
{
    { 0, 0 }, { 0, 1 }, { 1, 0 }, { 0xFFFFFFFF, 0 },
    { 0, 0xFFFFFFFF }, { 0xFFFFFFFF, 0xFFFFFFFF },
    { 0x7FFFFFFF, 0x80000000 }, { 0x80000000, 0x7FFFFFFF },
    { 5, 5 }, { 0x12345678, 0x12345679 }, { 0x12345679, 0x12345678 },
    { 0x80000000, 0x80000000 }, { 0xFFFFFFFF, 1 }, { 1, 0xFFFFFFFF },
    { 0x8000, 0x10000 }, { 0x00200004, 0x00200000 },
};

uint32_t aac_brprobe(const uint32_t (*cases)[2], uint32_t n);

uint32_t aac_image_probe(void)
{
    return aac_brprobe(probe_cases, AAC_PROBE_CASES);
}

#endif /* BCMFW_AAC */
