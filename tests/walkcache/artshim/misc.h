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
 * firmware file layer in walkcache.c art mode. A test shim only. */
#ifndef ARTSHIM_MISC_H
#define ARTSHIM_MISC_H
#include "config.h"
#include "autoconf.h"
#include "file.h"
#include "strmemccpy.h"
/* apps/misc.c, extracted by run.sh (artmisc.c) */
void fix_path_part(char *path, int offset, int count);
char *strip_extension(char *buffer, int buffer_size, const char *filename);
#endif
