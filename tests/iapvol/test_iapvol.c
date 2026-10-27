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

/* tests/iapvol: the integer iAP volume maths (apps/iap/iap-volume.h)
 * against upstream's double maths, over every input. */
#include <stdio.h>
#include "iap/iap-volume.h"

int main(void)
{
    int bad = 0;

    /* every volume an int8 setting can hold, far beyond the codec's
     * range, both signs (the double form truncates toward zero too) */
    for (int v = -400; v <= 400; v++)
    {
        int ref = (int)((v + 90) * 2.65625);
        int got = IAP_VOLUME_TO_REMOTE(v);
        if (ref != got || (0xFF & ref) != (0xFF & got))
        {
            printf("to_remote(%d): double %d, int %d\n", v, ref, got);
            bad++;
        }
    }
    /* every byte an accessory can send */
    for (int b = 0; b <= 255; b++)
    {
        unsigned char c = (unsigned char)b;
        int ref = (int)(c / 2.65625) - 90;
        int got = IAP_VOLUME_FROM_REMOTE(c);
        if (ref != got)
        {
            printf("from_remote(%d): double %d, int %d\n", b, ref, got);
            bad++;
        }
    }
    return bad ? 1 : 0;
}
