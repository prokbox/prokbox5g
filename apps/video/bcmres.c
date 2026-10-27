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

/* The BCM2722 image files and the resident image (see bcmres.h). */

#include "config.h"
#include "system.h"
#include "core_alloc.h"
#include "file.h"
#include "rbpaths.h"
#include "ipodui/uilog.h"
#include "bcmres.h"
#include "bcmvideo_image.h"
#ifdef HAVE_BCMFW_RESIDENT
#include "bcm-devtest.h"
#endif

const struct vimage bcmres_image =
    { BCMRES_IMAGE, BCMVIDEO_IMAGE_BYTES, BCMVIDEO_IMAGE_FNV };

static uint32_t image_fnv(const uint8_t *p, uint32_t n)
{
    uint32_t v = 2166136261u;

    while (n--)
        v = (v ^ *p++) * 16777619u;
    return v;
}

int bcmres_read(const struct vimage *vi, uint8_t **out, uint32_t *bytes)
{
    int fd = open(vi->path, O_RDONLY), h;
    off_t n;

    if (fd < 0)
    {
        ui_log("video", "no image %s", vi->path);
        return 0;
    }
    n = ffilesize(fd);
    if (n < 0x200 || n > BCMRES_IMAGE_MAX || (n & 3))
    {
        close(fd);
        ui_log("video", "image size %ld refused", (long)n);
        return 0;
    }
    h = core_alloc(n);
    if (h <= 0)
    {
        close(fd);
        return 0;
    }
    core_pin(h);
    *out = core_get_data(h);
    /* one bulk read of the whole image (never streamed in pieces) */
    if (read(fd, *out, n) != n)
    {
        close(fd);
        core_unpin(h);
        core_free(h);
        return 0;
    }
    close(fd);
    /* only the image this build was made with (bcmfw/mkvideo.py): an
       older or foreign file would speak another protocol or decode
       wrongly; refused (the player's audio plays without pictures, the
       boot path keeps its other rungs) */
    if ((uint32_t)n != vi->bytes || image_fnv(*out, n) != vi->fnv)
    {
        ui_log("video", "image %s is not this build's (%ld bytes)",
               vi->path, (long)n);
        core_unpin(h);
        core_free(h);
        return 0;
    }
    *bytes = n;
    return h;
}

#ifdef HAVE_BCMFW_RESIDENT
/* p14-resident: the image kept from boot for the boot path's resident
   rung (never freed: the display driver may load it at every wake) */
static int res_h;
uint8_t *bcmres_img;
uint32_t bcmres_bytes;

void bcmres_offer(void)
{
    uint32_t t0 = USEC_TIMER, us;
    bool idle;

    if (res_h || !bcm_boot_resident_setting(&idle))
        return;
    res_h = bcmres_read(&bcmres_image, &bcmres_img, &bcmres_bytes);
    us = USEC_TIMER - t0;
    if (!res_h)
    {
        bcmres_img = NULL;
        return;
    }
    bcm_boot_offer_resident(bcmres_img, bcmres_bytes, BCMVIDEO_IMAGE_FNV, us);
    ui_log("video", "resident image: %lu bytes read in %lu ms",
           (unsigned long)bcmres_bytes, (unsigned long)us / 1000);
}
#endif
