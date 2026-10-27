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

/* The first-use video check's token on disk (vcheck_store.c). */
#ifndef VCHECK_STORE_H
#define VCHECK_STORE_H

#include "vcheck.h"

/* the token, its temporary and the replace's old name */
struct vcheck_files
{
    const char *path, *tmp, *prev;
};

#define VCHECK_STORE_NONE       (-1)    /* no file */
#define VCHECK_STORE_CORRUPT    (-2)    /* a file, not a token */

/* A cut replace finished first (safefile_recover), then the token: 0, or
   VCHECK_STORE_NONE / _CORRUPT (both mean: check again) */
int vcheck_store_read(const struct vcheck_files *f, struct vcheck_token *t);
/* Written whole and synced to tmp, then put in place: 0 or -1 (the old
   token, if any, stays) */
int vcheck_store_write(const struct vcheck_files *f,
                       const struct vcheck_token *t);
/* The token gone (live invalidation): 1 removed, 0 none, -1 failed */
int vcheck_store_drop(const struct vcheck_files *f);

#endif /* VCHECK_STORE_H */
