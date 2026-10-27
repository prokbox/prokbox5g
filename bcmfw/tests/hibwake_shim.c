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

/* hibwake_test.py's way into the PP's header-only instant-wake code
 * (firmware/target/arm/ipod/video/bcm-hibwake.h): each static inline
 * exported once, so ctypes can call the exact code the driver would. */
#include "bcm-hibwake.h"

uint32_t t_breakeven_ms(const struct bhw_power *p)
{
    return bhw_breakeven_ms(p);
}

int t_choose(const struct bhw_power *p, const struct bhw_hist *h)
{
    return bhw_choose(p, h);
}

int t_drop_now(const struct bhw_power *p, uint32_t held_ms)
{
    return bhw_drop_now(p, held_ms);
}

uint64_t t_hold_cost(const struct bhw_power *p, uint32_t cap, uint32_t t)
{
    return bhw_hold_cost(p, cap, t);
}

void t_hist_add(struct bhw_hist *h, uint32_t ms)
{
    bhw_hist_add(h, ms);
}

int t_gate(int built, int pw5, int stub)
{
    return bhw_gate(built, pw5, stub);
}

int t_default_regions(int merged, struct bhw_region *r)
{
    return bhw_default_regions(merged, r);
}

int t_prepare(const struct bhw_hal *h, const uint32_t *stub, uint32_t words,
              const struct bhw_region *r, int n, uint32_t wait_ms)
{
    return bhw_prepare(h, stub, words, r, n, wait_ms);
}

int t_park(const struct bhw_hal *h, struct bhw_result *r)
{
    return bhw_park(h, r);
}

int t_wake(const struct bhw_hal *h, struct bhw_result *r)
{
    return bhw_wake(h, r);
}

void t_drop(const struct bhw_hal *h, struct bhw_result *r)
{
    bhw_drop(h, r);
}
