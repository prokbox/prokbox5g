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

/* #196: the codec's id3 on lines of its own, uncached for both cores
 * (codec_id3.h). Host test: tests/codecid3/ (a line-granular model of the
 * two caches). */
#include "config.h"
#include <stdint.h>
#include "system.h"
#include "cop_job.h"
#include "codec_id3.h"

void *codec_id3_place(void *slot)
{
    uintptr_t p = ((uintptr_t)slot + CODEC_ID3_LINE - 1)
                  & ~(uintptr_t)(CODEC_ID3_LINE - 1);

#if NUM_CORES > 1 && !defined(SIMULATOR)
    /* The slot's memory had another use before (the ring, a chapter
     * table, the old scratch area): a dirty CPU line of it written back
     * later would land on the entry. Write back and drop them all now;
     * from here the CPU reaches the entry only uncached, so it never
     * caches a line of it again */
    commit_discard_dcache();
#endif
    return cop_uncached((void *)p);
}
