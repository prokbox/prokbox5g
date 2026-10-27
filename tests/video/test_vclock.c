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

/* The video clock gate (apps/video/vclock_gate.h): a release build runs
 * the chip at 150 MHz with a verified image (a clock150 token naming its
 * FNV-1a) and at 27 MHz without one; a developer build also with the
 * unit's 4b and 4c flags.  Then this build's own tokens
 * (bcmvideo_clock.h, from bcmfw/verified.txt) against its merged image
 * (bcmvideo_image.h): reported, and a token must name an image FNV. */
#include <stdio.h>
#include "vclock_gate.h"
#define HAVE_BCMFW_MERGED_VIDEO
#include "bcmvideo_image.h"
#include "bcmvideo_clock.h"

static int fails, n;

static void check(int ok, const char *what)
{
    n++;
    if (!ok)
        fails++;
    printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
}

int main(void)
{
    static const uint32_t tok[] = { 0x80bc8611u, 0x12345678u };
    static const uint32_t none[1] = { 0 };
    enum vclock_why g;
    int k, found = 0;

    /* release (no device test): the token alone decides */
    check(vclock_gate(0x80bc8611u, tok, 2, false, false) == VCLOCK_TOKEN,
          "release, verified merged image: 150 MHz");
    check(vclock_gate(0x12345678u, tok, 2, false, false) == VCLOCK_TOKEN,
          "release, the second token: 150 MHz");
    check(vclock_gate(0xdeadbeefu, tok, 2, false, false) == VCLOCK_NONE,
          "release, an image without a token: 27 MHz");
    check(vclock_gate(0x80bc8611u, none, 0, false, false) == VCLOCK_NONE,
          "release, no tokens at all: 27 MHz");
    check(vclock_gate(0xdeadbeefu, tok, 2, false, true) == VCLOCK_NONE,
          "release ignores developer flags: 27 MHz");
    check(vclock_gate(0, none, 1, false, false) == VCLOCK_NONE,
          "release, the empty list's 0 never matches: 27 MHz");
    /* developer builds: the token, or the unit's 4b and 4c flags */
    check(vclock_gate(0xdeadbeefu, tok, 2, true, true) == VCLOCK_DEV_FLAGS,
          "developer, flags, no token: 150 MHz");
    check(vclock_gate(0x80bc8611u, tok, 2, true, false) == VCLOCK_TOKEN,
          "developer, token, no flags: 150 MHz");
    check(vclock_gate(0xdeadbeefu, tok, 2, true, false) == VCLOCK_NONE,
          "developer, neither: 27 MHz");
    /* read per play, nothing cached: flags earned between two plays
       open the clock at the next one */
    g = vclock_gate(0xdeadbeefu, tok, 2, true, false);
    check(g == VCLOCK_NONE && vclock_gate(0xdeadbeefu, tok, 2, true, true) ==
          VCLOCK_DEV_FLAGS, "developer, flags earned between plays: the next play 150 MHz");

    /* this build's tokens */
    for (k = 0; k < BCMVIDEO_N_CLOCK150; k++)
    {
        check(bcmvideo_clock150_fnv[k] != 0, "token is an FNV, not 0");
        found |= bcmvideo_clock150_fnv[k] == BCMVIDEO_MERGED_FNV;
    }
    g = vclock_gate(BCMVIDEO_MERGED_FNV, bcmvideo_clock150_fnv,
                    BCMVIDEO_N_CLOCK150, false, false);
    check((g == VCLOCK_TOKEN) == found, "the gate agrees with the token list");
    printf("info  this build's merged image %08lx: a release plays it at %s\n",
           (unsigned long)BCMVIDEO_MERGED_FNV,
           g == VCLOCK_TOKEN ? "150 MHz (clock150 token)"
                             : "27 MHz until a re-verify writes its clock150 token");
    printf("test_vclock: %d of %d passed\n", n - fails, n);
    return fails != 0;
}
