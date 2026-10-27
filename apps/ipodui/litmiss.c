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
#include <string.h>
#include "config.h"
#include "ipodui/litmiss.h"
#include "ipodui/clockpol.h"

struct litmiss litmiss;

void litmiss_begin(void)
{
    memset(&litmiss, 0, sizeof(litmiss));
#ifdef HAVE_PLL_IDLE
    clockpol_lit24_set(false);      /* phase A: today's 30 MHz */
#endif
    litmiss.switch_at = current_tick + LITMISS_A_S * HZ;
    litmiss.on = true;
}

void litmiss_switch(void)
{
    litmiss.b = true;
#ifdef HAVE_PLL_IDLE
    clockpol_lit24_set(true);
#endif
}

void litmiss_end(void)
{
    litmiss.on = false;
}
