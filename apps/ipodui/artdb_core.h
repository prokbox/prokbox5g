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
 * iPod UI shell: the stock firmware's cover database, read only
 * (#225 S3; ProkBox never writes under iPod_Control). Pure C over
 * afb_core.h's streamed I/O callbacks, so the host tests (tests/syncart)
 * run it on real files; their fixture writer lives there, never here.
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
#ifndef IPODUI_ARTDB_CORE_H
#define IPODUI_ARTDB_CORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "afb_core.h"

/* Format facts from public documentation only (clean-room; sources in
 * docs/design/ipod-control-support.md §5.7). All integers little-endian.
 *
 *   iPod_Control/Artwork/ArtworkDB   the chunk tree (mhfd > mhsd x3 >
 *                                    mhli > mhii > mhod(2) > mhni >
 *                                    mhod(3) name; mhla; mhlf > mhif)
 *   iPod_Control/Artwork/F<id>_<n>.ithmb   fixed-size RGB565 images back
 *                                    to back (slot = offset / size)
 *
 * Format ids are never trusted: an image is chosen by its size, width
 * and height. ARTDB_FMT_* name the 5G/5.5G's ids for logs and fixtures
 * only ([public-doc]; a device check confirms). */

#define ARTDB_DIR       "/iPod_Control/Artwork"
#define ARTDB_FILE      ARTDB_DIR "/ArtworkDB"
#define ARTDB_NAME_MAX  24              /* "F1029_1.ithmb" + NUL, padded */

/* the cover sizes stock firmware 1.3 on a 5G/5.5G reads [public-doc] */
#define ARTDB_FMT_SMALL 1028            /* 100 x 100, RGB565 LE, 20,000 B */
#define ARTDB_FMT_LARGE 1029            /* 200 x 200, RGB565 LE, 80,000 B */

/* largest side the reader accepts (rejects the 720 x 480 TV-out size) */
#define ARTDB_MAX_SIDE  240

/* -- reading ------------------------------------------------------------ */

/* One stored image (an mhni and its file name), with the mhii it hangs
 * under. Only images that pass artdb_img_ok() reach a callback. */
struct artdb_img {
    uint64_t dbid;              /* the song's id (mhii +20) */
    uint32_t img_id;            /* the mhii's id (mhii +16) */
    uint32_t fmt;               /* mhni format id (informational) */
    uint32_t offset, size;      /* in the image file */
    int16_t  pad_v, pad_h;      /* mhni padding */
    uint16_t w, h;              /* mhni width/height: the picture */
    uint32_t src_size;          /* mhii's source image size (0 unknown) */
    char     file[ARTDB_NAME_MAX];  /* "F1029_1.ithmb": plain name only */
    /* the stored slot (artdb_img_geom(); filled by the reader) */
    uint16_t slot_w, slot_h;    /* w + pad_h, h + pad_v */
    uint16_t stride;            /* pixels per stored row: size / 2 / slot_h */
};

/* The stored slot of an image. Layout [real database, #225 survey of a
 * later model's file; the 5G's own files: hypothesis]: the mhni's w x h
 * is the picture, the padding is the rest of the slot (a 48-wide cover
 * with pad_h 7 in a 55 x 55 slot), and the slot's rows may be wider
 * than the slot (55 pixels in a 56-pixel stride: 6,160 B). So: slot =
 * picture + padding (padding >= 0), size = whole rows of the slot,
 * stride >= slot width. False: inconsistent. */
bool artdb_img_geom(const struct artdb_img *im, uint16_t *slot_w,
                    uint16_t *slot_h, uint16_t *stride);

/* artdb_img_geom() holds, slot sides 1..ARTDB_MAX_SIDE, stride <=
 * ARTDB_MAX_SIDE + 16, a plain F<digits>_<digits>.ithmb name (no
 * separators). The image-file length check is the caller's (it knows
 * the file). Recomputes the geometry; doesn't trust slot_w/h, stride. */
bool artdb_img_ok(const struct artdb_img *im);

/* Errors (any reader): */
enum {
    ARTDB_OK = 0, ARTDB_E_IO = -1, ARTDB_E_MAGIC = -2, ARTDB_E_LEN = -3,
    ARTDB_E_TAG = -4, ARTDB_E_NEST = -5, ARTDB_E_STOP = -6,
};

/* Walks the image list of an ArtworkDB of len bytes. cb runs for every
 * acceptable image; cb < 0 stops the walk with that value (ARTDB_E_STOP
 * by convention). Malformed chunks (lengths that don't nest, a cut file)
 * are errors; a single bad image is skipped and counted in *bad (may be
 * NULL). Returns the number of images passed to cb, or < 0. */
int artdb_read(const struct afb_in *in, uint32_t len,
               int (*cb)(void *ctx, const struct artdb_img *im), void *ctx,
               uint32_t *bad);

/* One song of a song database (the stock one: iPod_Control/iTunes/
 * iTunesDB; mhbd > mhsd type 1 > mhlt > mhit > mhod type 2). */
struct artdb_song {
    uint64_t dbid;              /* mhit +112 */
    uint32_t art_id;            /* mhit +352 when its header is >= 0x184,
                                   else 0: the mhii id it names */
    uint16_t art_count;         /* mhit +124 */
    uint8_t  has_art;           /* mhit +164: 1 yes, 2 no, 0 unknown */
    const char *path;           /* "/iPod_Control/Music/F23/ABCD.mp3":
                                   the location, ':' -> '/', UTF-8;
                                   valid during the callback only */
};

/* Walks the songs; cb as artdb_read(). Songs with no location are
 * skipped. Returns the number of songs passed to cb, or < 0. */
int artdb_songs(const struct afb_in *in, uint32_t len,
                int (*cb)(void *ctx, const struct artdb_song *s), void *ctx);

#endif /* IPODUI_ARTDB_CORE_H */
