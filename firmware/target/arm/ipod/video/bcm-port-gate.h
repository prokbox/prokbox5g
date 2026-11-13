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
 * The host port's power gate (#324): no load or store reaches the BCM2722's
 * host port (0x30000000-0x3007FFFF) while the chip is unpowered.
 *
 * GPO32 bit 14 (0x4000) powers the BCM2722.  A load or store to its host
 * port with the bit clear is believed to stall the PP's bus for good: the
 * core freezes with no tick, no interrupt and no watchdog (bench rows
 * vcdark, b11 and b12; quirks.md).  So every lowest-level port accessor
 * first reads the bit (a PP register: always safe) and, with it clear,
 * touches nothing, counts a refusal and returns its own failure value.
 *
 * bcm_port_on(fn): true when the chip is powered.  Otherwise false, the
 * refusal counted, and the first refusal after each power-up recorded for
 * one log line ("bcm port refused: chip unpowered (n=<count>, from
 * <fn>)", written later in thread context: lcd-video.c).  Any context,
 * the tick and the COP included; fn must be a string literal.
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
#ifndef BCM_PORT_GATE_H
#define BCM_PORT_GATE_H

#include <stdbool.h>
#include <stdint.h>

#define BCM_POWER_BIT 0x4000            /* GPO32_VAL: the BCM2722's power */

/* lcd-video.c: count a refusal; the first after a power-up is logged */
void bcm_port_refuse(const char *fn);
/* refusals since boot (diagnostics, the bench's vcdark row) */
uint32_t bcm_port_refused_count(void);

static inline bool bcm_port_on(const char *fn)
{
    if (__builtin_expect((GPO32_VAL & BCM_POWER_BIT) != 0, 1))
        return true;
    bcm_port_refuse(fn);
    return false;
}

#endif /* BCM_PORT_GATE_H */
