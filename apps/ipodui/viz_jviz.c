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

/* The visualizer's engine (#126): bcmfw/jobs/jviz.c, one definition of the
 * spectrum and the bars for the PP's CPU path and the BCM2722 (the
 * reference executor's SPECTRUM and BARS run the same file).  In a
 * simulator with the job model, also jref_viz.c: the model's SPECTRUM and
 * BARS (JREF_VIZ, apps.make).  Built as the reference executor is
 * (firmware/target/arm/ipod/video/bcmjob-ref.c): the bcmfw/ source
 * included, no copy. */
#include "config.h"
#ifndef SIMULATOR
/* the device's CPU path runs 256 samples only (viz.c N_CPU; the BCM's
 * SPECTRUM takes 512): the 512-sample tables, 3 KB, stay out of the PP
 * build.  The simulator's job model runs 512 through this file's jref */
#define JVIZ_N256_ONLY
#endif
#include "jviz.c"
#if defined(HAVE_BCM_JOBS_MODEL) && defined(JREF_VIZ)
#include "jref_viz.c"
#endif
