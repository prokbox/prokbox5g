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

/* The one port lock's depth logic (firmware/target/arm/ipod/video/
 * bcm-port-lock.h) against a model of lcd_state.blocked and the recursive
 * mutex: lcd_tick may run only while nothing holds the tick block.
 * The laptop's review (2026-11-09): bcm_port_unlock cleared the block
 * unconditionally, so a port lock nested inside a holder that had set it
 * (a push band) would let the tick in mid-band. */
#include <stdio.h>
#include <stdbool.h>

static int fails, cases;
#define CHECK(c, msg) do { cases++; if (!(c)) { fails++; \
    printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, msg); } } while (0)

static bool blocked, mutex_ok = true;
static int mutex_depth;

static bool bpl_mutex(void)
{
    if (!mutex_ok)
        return false;           /* IRQs off: no mutex */
    mutex_depth++;
    return true;
}

static void bpl_mutex_give(bool m)
{
    if (m)
        mutex_depth--;
}

static bool bpl_block(void)
{
    bool was = blocked;

    blocked = true;
    return was;
}

static void bpl_restore(bool was)
{
    blocked = was;
}

#include "bcm-port-lock.h"

/* a holder that sets the block itself (lcd_update_rect's push window:
 * lcd_block_tick() ... lcd_unblock_and_update()) */
static void push_begin(void) { mutex_depth++; blocked = true; }
static void push_end(void)   { blocked = false; mutex_depth--; }

int main(void)
{
    struct bpl_state s = { 0, false, false };

    /* alone: blocked while held, free after, the mutex balanced */
    bpl_lock(&s);
    CHECK(blocked && mutex_depth == 1, "alone: blocked, mutex held");
    bpl_lock(&s);
    bpl_unlock(&s);
    CHECK(blocked && mutex_depth == 1, "nested in itself: still blocked");
    bpl_unlock(&s);
    CHECK(!blocked && mutex_depth == 0 && s.depth == 0, "alone: released");

    /* nested inside a push band: the band's block must survive */
    push_begin();
    bpl_lock(&s);
    CHECK(blocked, "inside a band: blocked");
    bpl_unlock(&s);
    CHECK(blocked, "inside a band: the band's block kept after the unlock");
    push_end();
    CHECK(!blocked && mutex_depth == 0, "band over: released");

    /* no mutex (IRQs off): the block still set and put back as found */
    mutex_ok = false;
    push_begin();
    bpl_lock(&s);
    bpl_unlock(&s);
    CHECK(blocked && mutex_depth == 1, "no mutex, inside a band: kept");
    push_end();
    mutex_ok = true;
    bpl_lock(&s);
    bpl_unlock(&s);
    CHECK(!blocked && mutex_depth == 0, "no mutex: released after");

    printf("bcmportlock: %d cases, %d failed\n", cases, fails);
    return fails != 0;
}
