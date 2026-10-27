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
 * Hardware Sound Check arithmetic (docs/phase15-brief.md section 3.5,
 * wave 2b): one channel's volume plus a gain trim, split over the WM8758's
 * headphone amp (OUT1, 1 dB steps, -57..+6 dB) and DAC digital volume
 * (0.5 dB steps, 0 dB down to -127 dB). Pure, no dependencies: used by
 * drivers/audio/wm8758.c, the simulator's sdl.c and tests/schw.
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
#ifndef DAC_GAIN_H
#define DAC_GAIN_H

#define DAC_GAIN_VOL_MAX    60      /* +6 dB: the volume max, amp at 63 */
#define DAC_GAIN_MUTE       (-900)  /* the volume's mute (sound min) */
#define DAC_GAIN_MIN        (-480)  /* ReplayGain's own clamp (-48 dB) */

/* The trim actually applied to a volume: a cut always; a boost only up to
 * ceil_db10 (the Volume Limit, never above the volume max), never below
 * 0 (a boost never turns the volume down). Tenth dB. */
static inline int dac_gain_fit(int vol_db10, int gain_db10, int ceil_db10)
{
    if (ceil_db10 > DAC_GAIN_VOL_MAX)
        ceil_db10 = DAC_GAIN_VOL_MAX;
    if (gain_db10 < DAC_GAIN_MIN)
        gain_db10 = DAC_GAIN_MIN;
    if (gain_db10 > 0)
    {
        int room = ceil_db10 - vol_db10;
        if (room < 0)
            room = 0;
        if (gain_db10 > room)
            gain_db10 = room;
    }
    return gain_db10;
}

/* Register values for one channel: *dac is LDACVOL/RDACVOL's 8-bit volume
 * (255 = 0 dB, 0.5 dB per step, 0 = digital mute), *amp OUT1's 6-bit gain
 * (57 = 0 dB) or 0x40 (mute). The total (volume + fitted trim) is rounded
 * to the nearest 0.5 dB (error <= 0.2 dB, the inputs being whole tenths).
 * The amp carries whole dB down to -57 dB and the DAC the half dB below
 * it; under -57 dB the amp stays at -57 and the DAC attenuates (the
 * upstream split, which a whole-dB volume with no trim reproduces
 * exactly). The volume's mute stays a mute whatever the trim. Returns the
 * fitted trim. */
static inline int dac_gain_split(int vol_db10, int gain_db10, int ceil_db10,
                                 int *dac, int *amp)
{
    int g, t, h;

    if (vol_db10 <= DAC_GAIN_MUTE)
    {
        *dac = 0;
        *amp = 0x40;
        return 0;
    }
    if (vol_db10 > DAC_GAIN_VOL_MAX)
        vol_db10 = DAC_GAIN_VOL_MAX;
    g = dac_gain_fit(vol_db10, gain_db10, ceil_db10);
    t = vol_db10 + g;
    if (t > DAC_GAIN_VOL_MAX)
        t = DAC_GAIN_VOL_MAX;           /* a cut can't be > 0 */
    h = t >= 0 ? (t + 2) / 5 : -((2 - t) / 5); /* half dB, nearest */
    if (h > -114)                       /* above -57 dB: the amp */
    {
        int a = h > 0 ? (h + 1) / 2 : h / 2;    /* ceil(h / 2) */
        *amp = a + 57;
        *dac = 255 + (h - 2 * a);       /* 0 or -0.5 dB */
    }
    else
    {
        *amp = 0;                       /* -57 dB */
        *dac = 255 + (h + 114);
        if (*dac < 1)
            *dac = 1;                   /* 0 is the mute */
    }
    return g;
}

#endif /* DAC_GAIN_H */
