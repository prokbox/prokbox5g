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

/* The display job interface's reference executor (bcmfw/jobs/jref.c) in
 * the PP build (P19 gap G4): every job's CPU path, and the definition the
 * BCM2722's jobs match bit for bit.  Compiled as Thumb, no IRAM, no data
 * (firmware/firmware.make: the bcmfw/ include path and -mthumb for this
 * object); its state is bcmjob-cpu.c's allocation, never a static. */
#include "config.h"
#if defined(HAVE_BCM_JOBS) || defined(HAVE_BCM_JOBS_MODEL)
#include "jref.c"
#endif
