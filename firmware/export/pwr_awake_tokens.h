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

/* The awake rows' tokens (pwr_awake.h): one per row whose bench row
 * passed on a real unit. A passed row writes a line to the bench results
 * file,
 *     token pwraw-<row> <step> PASS ... (set PWRAW_<ROW> ...)
 * (apps/ipodui/devbench_awake.h, bench_awake_fmt_token()); set the row's
 * macro below to 1 in the same change that marks the row "default" in
 * apple-power-catalog.md's status column. That makes the row the daily
 * default in every build, the release included: config/ipodvideo.h
 * compiles a row's code into a release only once its token is set
 * (developer builds always carry it, behind the developer mask).
 *   PWRAW_TOKEN_LINGER   bench step awlg (T-LINGER)
 *   PWRAW_TOKEN_USBOFF   awus (T-USBPROBE), with the owner's "Yes"
 *   PWRAW_TOKEN_LCDLDO   awld (T-LCDLDO); also the battery probes' pldo
 *                        row (S-4, branch p19-pwr-probes) passing its
 *                        screen part: either result earns it
 *   PWRAW_TOKEN_I2SGATE  awi2 (T-I2SGATE, variant b)
 * Plain macros only: the config header includes this file.
 * Passed on the owner's 5.5G, sitting 4gh (2026-11-06, build
 * 56cd4e2096-261106):
 *   USBOFF  awus PASS, the owner's "Yes" (parked on the charger, the
 *           charge current kept)
 *   LCDLDO  awld PASS (5 of 5 lit, worst wake 447 ms; pldo's screen part
 *           restored as found)
 * Still off: LINGER (awlg ran with nothing playing), I2SGATE (awi2: 2 late
 * resumes of 6). Evidence: apple-power-catalog.md's status column,
 * clocks-power.md section 12. */
#ifndef PWR_AWAKE_TOKENS_H
#define PWR_AWAKE_TOKENS_H

#define PWRAW_TOKEN_LINGER      0
#define PWRAW_TOKEN_USBOFF      1
#define PWRAW_TOKEN_LCDLDO      1
#define PWRAW_TOKEN_I2SGATE     0

/* the bits, in enum pwraw_row's order (pwr_awake.h) */
#define PWRAW_TOKENS ((PWRAW_TOKEN_LINGER ? 1u : 0u) | \
                      (PWRAW_TOKEN_USBOFF ? 2u : 0u) | \
                      (PWRAW_TOKEN_LCDLDO ? 4u : 0u) | \
                      (PWRAW_TOKEN_I2SGATE ? 8u : 0u))

#endif /* PWR_AWAKE_TOKENS_H */
