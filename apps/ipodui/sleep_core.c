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

/* Phase 18-2: the pure half of the shell's sleep policy; see
 * sleep_core.h */

#include "ipodui/sleep_core.h"

uint32_t sleep_deep_after_s(uint32_t mah, uint32_t ua, uint32_t stock_s)
{
    uint64_t s;

    if (!ua)
        ua = SLEEP_UA_DEFAULT;
    /* mah * 1000 * pct / 100 uAh at ua uA, in seconds */
    s = (uint64_t)mah * 10u * SLEEP_DEEP_PCT * 3600u / ua;
    if (s > SLEEP_DEEP_AFTER_S)
        s = SLEEP_DEEP_AFTER_S;
    if (stock_s && s > stock_s)
        s = stock_s;
    if (s < SLEEP_DEEP_MIN_S)
        s = SLEEP_DEEP_MIN_S;
    return (uint32_t)s;
}

uint16_t sleep_low_mv(uint16_t disksafe_mv)
{
    return (uint16_t)(disksafe_mv + SLEEP_LOW_MARGIN_MV);
}

bool sleep_battery_low(int mv, int pct, uint16_t low_mv)
{
    return (pct >= 0 && pct < SLEEP_LOW_PCT) || (mv > 0 && mv <= low_mv);
}

enum sleep_pick sleep_tier_pick(const struct sleep_tier_in *in)
{
    if (in->usb)
        return SLEEP_PICK_IGNORE;
    if (in->charger && !in->dev_on_charger)
        return SLEEP_PICK_IGNORE;
    if (!in->supported || in->dev_off)
        return SLEEP_PICK_OFF;
    if (sleep_battery_low(in->mv, in->pct, in->low_mv))
        return SLEEP_PICK_OFF;
    return SLEEP_PICK_SOFT;
}

bool sleep_counts_as_boot(enum sleep_wake why, uint32_t total_slept_s)
{
    switch (why)
    {
    case WAKE_BUTTON:
    case WAKE_VBUS:
    case WAKE_ALARM:
    case WAKE_HOLD:
        return total_slept_s >= SLEEP_BOOT_EQUIV_S;
    default:            /* NONE, DEEP, LOWBATT, BCM_CUT (sleep goes on) */
        return false;
    }
}

const char *sleep_job_name(enum sleep_job j)
{
    static const char *const names[SJ_COUNT] = {
        "commit", "rebuild", "walk", "chapters", "topup", "rotation",
    };
    return (unsigned)j < SJ_COUNT ? names[j] : "?";
}

bool sleep_walk_owed(uint32_t now_rtc, uint32_t last_walk_rtc)
{
    if (!last_walk_rtc)
        return true;
    return now_rtc >= last_walk_rtc
        && now_rtc - last_walk_rtc >= SLEEP_WALK_S;
}

void sleep_swallow_arm(struct sleep_swallow *sw, uint8_t wake_buttons,
                       uint8_t held, uint8_t entry_buttons)
{
    sw->mask = ((wake_buttons & held) | entry_buttons) & WHEEL_BTN_BITS;
}

bool sleep_swallow_event(struct sleep_swallow *sw, uint8_t bit,
                         bool release)
{
    if (!(sw->mask & bit))
        return false;
    if (release)
        sw->mask &= (uint8_t)~bit;
    return true;
}
