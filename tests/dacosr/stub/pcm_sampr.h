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

/* the ipodvideo HAVE_HIRATE_OUT rate set (the driver's tables use
   designated initialisers, so the order here does not matter) */
#ifndef DACOSR_STUB_PCM_SAMPR_H
#define DACOSR_STUB_PCM_SAMPR_H
enum hw_freq { HW_FREQ_96, HW_FREQ_88, HW_FREQ_48, HW_FREQ_44, HW_FREQ_32,
               HW_FREQ_24, HW_FREQ_22, HW_FREQ_16, HW_FREQ_12, HW_FREQ_11,
               HW_FREQ_8, HW_NUM_FREQ };
extern const unsigned long hw_freq_sampr[HW_NUM_FREQ];
#endif
