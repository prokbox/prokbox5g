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

/* The BCM2722 image files on the disk and the resident image (P19,
   p19-release-noplayer): the one read the boot path's resident rung needs,
   moved out of the video player so a build with the display jobs and no
   player (configure --bcmfw-merged without --bcmfw-video) carries it.

   bcmres_read(): an image file in one bulk read into a pinned core_alloc
   block, checked against the size and FNV-1a this build was made with
   (bcmfw/mkvideo.py's headers).  The player reads its images with it too
   (HAVE_BCMFW_VIDEO, device and simulator).

   bcmres_offer() (HAVE_BCMFW_RESIDENT, the device): at boot, before the
   boot path's swap, the resident image (BCMRES_IMAGE: the merged image in
   a build with HAVE_BCMFW_MERGED_VIDEO, else the plain video image) read
   once into a block kept for good and offered to the boot path
   (bcm_boot_offer_resident); the developer setting can turn it off
   (bcm_boot_resident_setting).  The player then boots nothing and reads
   no image for a play (bcmres_img). */

#ifndef BCMRES_H
#define BCMRES_H

#include <stdbool.h>
#include <stdint.h>

/* the resident image (hidden, like every shell file; never named on
   screen), and the largest image any of these files may be */
#define BCMRES_IMAGE        ROCKBOX_DIR "/bcmvideo.bin"
#define BCMRES_IMAGE_MAX    0x80000

/* An image file and the size and FNV-1a this build expects of it */
struct vimage
{
    const char *path;
    uint32_t bytes, fnv;
};

/* The resident image (BCMRES_IMAGE), also the player's: the size and
   FNV-1a of bcmvideo_image.h */
extern const struct vimage bcmres_image;

/* vi's file in one read into a pinned block of its own: the block's
   handle (*out, *bytes set), or 0, logged (missing, wrong size or not this
   build's image) */
int bcmres_read(const struct vimage *vi, uint8_t **out, uint32_t *bytes);

#ifdef HAVE_BCMFW_RESIDENT
/* The resident image, if bcmres_offer() read it (NULL otherwise): never
   freed, since the display driver may load it again at every wake */
extern uint8_t *bcmres_img;
extern uint32_t bcmres_bytes;
void bcmres_offer(void);
#endif

#endif /* BCMRES_H */
