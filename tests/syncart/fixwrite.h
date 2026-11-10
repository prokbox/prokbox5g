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

/* Test-only writers for the stock firmware's cover database and song
 * database (#225 S3's fixtures). Host code: never linked into the
 * firmware (ProkBox is read-only toward iPod_Control). Layout from public
 * documentation only, as apps/ipodui/artdb_core.c's reader. */
#ifndef SYNCART_FIXWRITE_H
#define SYNCART_FIXWRITE_H

#include <stdint.h>
#include <stdio.h>

/* -- the cover database -------------------------------------------------- */
struct fx_fmt {
    uint32_t id;                /* ARTDB_FMT_* */
    uint16_t w, h;
};

#define FX_MAX_FMTS 4
struct fx_art {
    FILE *f;
    uint32_t n, next_id, list_at, sd1_at;
    int nfmt;
    struct fx_fmt fmt[FX_MAX_FMTS];
    int label;                  /* each mhii also gets a leading type-1
                                   string mhod (a photo-style label) */
};

/* Overrides for the malformed-case tests (0: the normal value). */
struct fx_img_opt {
    uint32_t size;              /* mhni image size */
    uint16_t w, h;              /* mhni width/height */
    const char *name;           /* the name string, as stored (":F..") */
    int16_t pad_v, pad_h;       /* mhni padding (written as given) */
};

void fx_art_begin(struct fx_art *w, FILE *f, const struct fx_fmt *fmts,
                  int nfmt, uint32_t first_id);
/* one mhii for song dbid, one image per format at offsets[i]; opt may be
 * NULL or an array of nfmt overrides. Returns the mhii id. */
uint32_t fx_art_add(struct fx_art *w, uint64_t dbid, uint32_t src_size,
                    const uint32_t *offsets, const struct fx_img_opt *opt);
/* the album list (empty), the file list; the file's length */
long fx_art_end(struct fx_art *w);

/* -- the song database ------------------------------------------------- */
struct fx_song {
    const char *path;           /* "/iPod_Control/Music/F23/ABCD.mp3",
                                   UTF-8; NULL: no location mhod */
    uint64_t dbid;
    uint32_t art_id, art_size;  /* art_id 0: no cover fields */
};

struct fx_db {
    FILE *f;
    uint32_t n, list_at, sd_at;
    uint32_t mhit_hl;           /* 0x184 (default) or shorter */
    int utf8;                   /* locations as UTF-8 (position 2) */
};

void fx_db_begin(struct fx_db *d, FILE *f, uint32_t mhit_hl, int utf8);
void fx_db_add(struct fx_db *d, const struct fx_song *s);
long fx_db_end(struct fx_db *d);

/* the cover pattern of group g at w x h (RGB565 LE, row-major); with
 * fx_mirror set, mirrored left to right (a re-sync's new pictures) */
extern int fx_mirror;
void fx_pattern(uint8_t *p, unsigned g, unsigned w, unsigned h);

#endif
