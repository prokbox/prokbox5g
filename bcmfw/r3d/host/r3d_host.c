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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../r3d.h"

/* The renderer built for the Mac: the same source as the chip's image,
 * so previews and videos render in seconds.  r3d_test.py checks that its
 * check frames carry the simulator's checksums.
 *   r3d_host crc                  each scene's check-frame checksums
 *   r3d_host video FPS [scenes]   raw RGB24 frames, 320x240, on stdout */

static uint8_t tex[8 * 64 * 64];
static uint16_t clut[48 * 256];
static uint64_t verts[2048];
static uint8_t tris[4096 * 16], xverts[2048 * 20], prims[6144 * 24];
static uint16_t ot[512], fb[320 * 240];

static void rgb(FILE *f)
{
    static uint8_t row[320 * 240 * 3];
    int i;

    for (i = 0; i < 320 * 240; i++)
    {
        uint16_t v = fb[i];

        row[3 * i] = (v >> 11) * 255 / 31;
        row[3 * i + 1] = ((v >> 5) & 63) * 255 / 63;
        row[3 * i + 2] = (v & 31) * 255 / 31;
    }
    fwrite(row, 1, sizeof(row), f);
}

int main(int argc, char **argv)
{
    struct r3d_mem m = { tex, clut, verts, tris, xverts, ot, prims };
    struct r3d_stats st;
    int sc, k;

    r3d_init(&m);
    if (argc >= 2 && !strcmp(argv[1], "crc"))
    {
        static const uint32_t ms[2] = { 0, 5000 };

        for (sc = 0; sc < R3D_SCENES; sc++)
        {
            r3d_scene(sc);
            for (k = 0; k < 2; k++)
            {
                r3d_render(ms[k], fb, &st);
                printf("%d %u %08x %u\n", sc, ms[k], r3d_crc(fb), st.drawn);
            }
        }
        return 0;
    }
    if (argc >= 3 && !strcmp(argv[1], "video"))
    {
        int fps = atoi(argv[2]);
        const char *scenes = argc >= 4 ? argv[3] : "01234";

        for (; *scenes; scenes++)
        {
            r3d_scene(*scenes - '0');
            for (k = 0; k < 10 * fps; k++)
            {
                r3d_render((uint32_t)(k * 1000 / fps), fb, &st);
                rgb(stdout);
            }
        }
        return 0;
    }
    fprintf(stderr, "usage: r3d_host crc | video FPS [scenes]\n");
    return 2;
}
