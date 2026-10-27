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

#ifndef _IAP_VOLUME_H_
#define _IAP_VOLUME_H_

/* The iAP volume byte (0..255) and the player's volume in dB (+90 makes
 * the range start at 0). The factor is 2.65625 = 85/32, exactly, so the
 * integer forms below give the same result as upstream's double maths,
 * (int)((v + 90) * 2.65625) and (int)(b / 2.65625) - 90, for every input
 * (both truncate toward zero; a quotient b*32/85 that is not a whole
 * number is at least 1/85 from one, far beyond a double's rounding).
 * tests/iapvol checks that over the whole range. They keep the
 * soft-double helpers out of the firmware. */
#define IAP_VOLUME_TO_REMOTE(v)    ((((int)(v)) + 90) * 85 / 32)
#define IAP_VOLUME_FROM_REMOTE(b)  (((int)(b)) * 32 / 85 - 90)

#endif /* _IAP_VOLUME_H_ */
