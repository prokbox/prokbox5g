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

/* The JPEG spike's reference (jpeg-spike.md step S1): the shell's own
 * decoder and resizer (apps/recorder/jpeg_load.c, resize.c, bmp.c, as the
 * simulator compiles them), called the way the art builder calls them
 * (artcache.c img_decode: clip_jpeg_fd, FORMAT_NATIVE | FORMAT_DITHER |
 * FORMAT_RESIZE), at a size given on the command line.  Writes the
 * RGB565 pixels (little-endian, row by row) and prints
 * "<w> <h> <rc> <fnv1a>".  Built by build.sh against build/sim-ui.
 *
 * Usage: jpegref FILE.jpg W H OUT.rgb565 */
#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lcd.h"
#include "file.h"
#include "jpeg_load.h"

#define DECODE_BYTES (8 << 20)

int main(int argc, char **argv)
{
    struct bitmap bm;
    unsigned char *buf;
    unsigned long n, i;
    uint32_t fnv = 2166136261u;
    int fd, rc;
    FILE *out;

    if (argc != 5)
    {
        fprintf(stderr, "usage: jpegref FILE.jpg W H OUT.rgb565\n");
        return 2;
    }
    buf = malloc(DECODE_BYTES);
    fd = open(argv[1], O_RDONLY);
    if (!buf || fd < 0)
        return 2;
    memset(&bm, 0, sizeof(bm));
    bm.width = atoi(argv[2]);
    bm.height = atoi(argv[3]);
    bm.data = buf;
    rc = clip_jpeg_fd(fd, 0, 0, &bm, DECODE_BYTES,
                      FORMAT_NATIVE | FORMAT_DITHER | FORMAT_RESIZE, NULL);
    close(fd);
    if (rc <= 0)
    {
        printf("0 0 %d 0\n", rc);
        return 1;
    }
    n = (unsigned long)bm.width * bm.height * 2;
    for (i = 0; i < n; i++)
        fnv = (fnv ^ bm.data[i]) * 16777619u;
    out = fopen(argv[4], "wb");
    if (!out || fwrite(bm.data, 1, n, out) != n)
        return 2;
    fclose(out);
    printf("%d %d %d %08x\n", bm.width, bm.height, rc, (unsigned)fnv);
    return 0;
}
