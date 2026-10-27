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
 * The vector half of PW3's and PW4's self-check (hostproto.c cmd_pw): one
 * 32-byte row loaded as 16 halfwords and summed through the scalar-result
 * unit.  The three instructions are bench row SUMR's exactly
 * (bench/gen_bench.py), whose result was checked on the device in session
 * 4 (tools/bcmre/device_verified.txt: 48-bit vld16 to d00, vmov ... sum
 * rN): no other vector form.  The plain video image only.
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
#include "config.h"

#if BCMFW_VIDEO && !BCMFW_JOBS && BCMFW_CLOCK
    .syntax vc2
    .text

/* void pw_sumr(const void *row, uint32_t *out): *out = the sum of the
 * low bytes of the 16 halfwords at row (32-byte aligned) */
    .global pw_sumr
pw_sumr:
    vld16 d00, (r0)
    vmov.1 d10, a400, b00 sum r5
    st r5, (r1)
    rts
#endif
