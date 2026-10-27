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

/* Host round trip for the native USB screen's pixel code (Phase 8a):
 * builds apps/ipodui/usbscreen.c's pure part (USB_PURE_ONLY) with the
 * generated tables, composes the strip exactly as the firmware does and
 * writes each screen (header rows, dithered body, disc box; no text) as
 * raw little-endian RGB565 for check.py to compare with the generator's
 * reference PNGs in docs/design/usb/.
 *
 *   test_usbscreen <outdir>  ->  idle-usbc.565 idle-dock30.565 f00..f05.565
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define USB_PURE_ONLY
#include "usbscreen.c"

#define W 320
#define H 240

static usb_px fb[W * H];

static int dump(const char *dir, const char *name, const usb_px *box)
{
    char path[512];
    FILE *f;
    int x, y;

    for (y = 0; y < H; y++)
    {
        if (y < USB_HDR_H)
            for (x = 0; x < W; x++)
                fb[y * W + x] = usb_hdr_rows[y];
        else
            usb_body_row(fb + y * W, y, W);
    }
    for (y = 0; y < USB_N; y++)
        memcpy(fb + (USB_BOX_Y + y) * W + USB_BOX_X, box + y * USB_N,
               USB_N * sizeof(usb_px));
    snprintf(path, sizeof(path), "%s/%s.565", dir, name);
    f = fopen(path, "wb");
    if (!f)
        return 1;
    for (x = 0; x < W * H; x++)
    {
        fputc(fb[x] & 0xff, f);
        fputc(fb[x] >> 8, f);
    }
    fclose(f);
    return 0;
}

int main(int argc, char **argv)
{
    usb_px *strip = malloc(USB_STRIP_BYTES);
    char name[16];
    int k, rc = 0;

    if (argc < 2 || !strip)
        return 2;
    usb_compose_all(strip, 1);                          /* USB-C */
    rc |= dump(argv[1], "idle-usbc", strip + USB_SLOT_IDLE * USB_SLOT);
    for (k = 0; k < USB_STORED; k++)
    {
        snprintf(name, sizeof(name), "f%02d", k);
        rc |= dump(argv[1], name, strip + (USB_SLOT_FRAME0 + k) * USB_SLOT);
    }
    usb_compose_all(strip, 0);                          /* 30-pin */
    rc |= dump(argv[1], "idle-dock30", strip + USB_SLOT_IDLE * USB_SLOT);
    printf("strip %d B (of the 180000 B strip), %d stored frames\n",
           (int)USB_STRIP_BYTES, USB_STORED);
    free(strip);
    return rc;
}
