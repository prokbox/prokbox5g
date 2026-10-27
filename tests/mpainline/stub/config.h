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

/* tests/mpainline: the host config for lib/rbcodec/codecs/mpa.c and
 * libmad. STUB_CORES (from run.sh) picks the build: 2 compiles mpa.c's
 * second-core synthesis (MPA_SYNTH_ON_COP, on pthreads here) with the
 * shell's runtime switch; 1 compiles upstream's single-core path. */
#ifndef STUB_CONFIG_H
#define STUB_CONFIG_H
#ifndef STUB_CORES
#define STUB_CORES 2
#endif
#define NUM_CORES STUB_CORES
#define HAVE_IPOD_UI
#define DEFAULT_STACK_SIZE 0x1000
#define IBSS_ATTR
#define ICODE_ATTR
#define ICONST_ATTR
#define MEM_ALIGN_ATTR
#endif
