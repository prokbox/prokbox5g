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

/* Plain byte-loop memcpy/memmove/memset for the qemu dynamic-count
 * harness (risk tool 1). firmware/asm/arm/{memcpy,memmove}.S are
 * Rockbox's real, hand-optimized device routines, but they use macros
 * ("ldmpc", the pull/push shift macro) defined elsewhere in Rockbox's
 * assembly build (not resolved by a standalone `as`/`gcc -c` invocation
 * outside the real firmware Makefile), so this harness substitutes a
 * plain C version instead. For the tiny (1-4 byte) copies ui_sortkey()
 * actually does, this is a conservative (slightly pessimistic, i.e. not
 * an undercount) stand-in: Rockbox's real memcpy/memmove for a 1-3 byte
 * copy take about 11 ARM instructions (hand-traced in
 * docs/risk-tooling.md from firmware/asm/arm/memcpy.S's short-copy tail,
 * label "0:"-"8:"); this loop takes a few more per byte. Documented, not
 * hidden: scripts/risk/arm_insn_count.sh reports both the qemu-measured
 * total (using this shim) and the hand-derived total (using the real
 * routine's traced cost) so the gap is visible. */
#include <stddef.h>

void *memcpy(void *dst, const void *src, size_t n)
{
    char *d = dst;
    const char *s = src;
    while (n--)
        *d++ = *s++;
    return dst;
}

void *memmove(void *dst, const void *src, size_t n)
{
    char *d = dst;
    const char *s = src;
    if (d < s)
        while (n--)
            *d++ = *s++;
    else
    {
        d += n; s += n;
        while (n--)
            *--d = *--s;
    }
    return dst;
}

void *memset(void *dst, int c, size_t n)
{
    char *d = dst;
    while (n--)
        *d++ = (char)c;
    return dst;
}
