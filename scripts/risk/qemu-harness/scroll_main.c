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

/* Bare-metal ARM driver for apps/ipodui/scroll.c under qemu-system-arm +
 * the instruction-count plugin (risk tool 1): the same representative
 * glide workload as scripts/risk/drivers/scroll_drv.c (a 200+ px jump in
 * a ~4000-row offscreen strip, dt=1 per tick: one 30 fps frame each). */
#include "ipodui/scroll.h"

#define GLIDES 500

volatile int sink;

int main(void)
{
    struct ui_scroll s;
    int g;

    for (g = 0; g < GLIDES; g++)
    {
        ui_scroll_init(&s, 4000);
        ui_scroll_set_target(&s, 200 + (g * 7) % 3000);
        while (ui_scroll_step(&s, 1))
            sink++;
    }
    return 0;
}
