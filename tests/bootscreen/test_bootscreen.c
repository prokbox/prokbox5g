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
 * Host round-trip test for the boot screen (Phase 8a, docs/phase8a-brief.md
 * section 4): runs the real firmware/common/bootscreen.c with the committed
 * bootscreen_data.c into a host framebuffer, checks BOOTSCREEN_GEN against
 * the data, and writes the framebuffer as raw little-endian RGB565 for
 * compare.py.
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
#include <stdio.h>
#include <string.h>
#include "config.h"
#include "lcd.h"
#include "bootscreen.h"

fb_data host_fb[LCD_HEIGHT][LCD_WIDTH] __attribute__((aligned(4)));

static uint32_t fnv(uint32_t h, const unsigned short *w, size_t n)
{
    for (size_t i = 0; i < n; i++)
    {
        h = (h ^ (w[i] & 0xff)) * 0x01000193u;
        h = (h ^ (w[i] >> 8)) * 0x01000193u;
    }
    return h;
}

int main(int argc, char **argv)
{
    if (argc != 2)
    {
        fprintf(stderr, "usage: %s OUT.raw\n", argv[0]);
        return 2;
    }
    memset(host_fb, 0xa5, sizeof(host_fb));     /* every pixel must be drawn */
    bootscreen_draw();

    uint32_t h = fnv(0x811c9dc5u, &bootscreen_bg[0][0], LCD_HEIGHT * 2);
    h = fnv(h, bootscreen_block, BOOTSCREEN_W * BOOTSCREEN_H);
    if (h != (uint32_t)BOOTSCREEN_GEN)
    {
        printf("FAIL: BOOTSCREEN_GEN %08lx != data hash %08lx\n",
               (unsigned long)BOOTSCREEN_GEN, (unsigned long)h);
        return 1;
    }

    FILE *fp = fopen(argv[1], "wb");
    if (!fp)
        return 2;
    for (int y = 0; y < LCD_HEIGHT; y++)
        for (int x = 0; x < LCD_WIDTH; x++)
        {
            unsigned char b[2] = { host_fb[y][x] & 0xff, host_fb[y][x] >> 8 };
            fwrite(b, 1, 2, fp);
        }
    fclose(fp);
    printf("ok   bootscreen gen %08lx (%dx%d at %d,%d)\n",
           (unsigned long)h, BOOTSCREEN_W, BOOTSCREEN_H,
           BOOTSCREEN_X, BOOTSCREEN_Y);
    return 0;
}
