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

/* tests/walkcache/artshim (#332): just enough of apps/ for the real
 * apps/recorder/albumart.c (search_albumart_files()) to run over the
 * firmware file layer in walkcache.c art mode, and over tests/artsrc's
 * counted lookups. A test shim only. */
#ifndef ARTSHIM_METADATA_H
#define ARTSHIM_METADATA_H
#include "config.h"
#include "file.h"
#include <sys/types.h>
/* the embedded picture's place (tests/artsrc: artcache.c's source
 * choice reads these) */
struct mp3_albumart
{
    int type;
    int size;
    off_t pos;
};
/* the fields albumart.c reads, then the ones artcache.c's source
 * choice reads (tests/artsrc) */
struct mp3entry
{
    char path[MAX_PATH];
    char *album;
    char *artist;
    char *albumartist;
    bool has_embedded_albumart;
    struct mp3_albumart albumart;
};
#endif
