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
 * iPod UI shell
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
#ifndef IPODUI_VOLCURVE_H
#define IPODUI_VOLCURVE_H

/* One volume table for the wheel, the bar and Volume Limit
 * (docs/phase15-brief.md sections 2.3, 3.4). Pure: no Rockbox headers, so
 * tests/volcurve/ compiles it on the host. */

#define VOL_STEPS 48            /* positions 0 (mute) .. 47 (+6 dB); brief
                                 * section 9 Q2, frozen default */
#define VOL_DB_MUTE (-90)       /* step 0: SOUND_VOLUME's minimum, which the
                                 * WM8758 driver mutes */

int vol_step_to_db(int step);   /* whole dB (SOUND_VOLUME has 0 decimals); step 0 = mute */
int vol_db_to_step(int db);     /* nearest step at or below */
int vol_step_to_bar1024(int step);  /* the bar and Volume Limit: linear in step */
/* The step 'clicks' wheel clicks from volume db, clamped 0..VOL_STEPS-1.
 * db between two steps snaps down first (never louder), and that snap
 * counts as the first click down. */
int vol_step_add(int db, int clicks);

#endif /* IPODUI_VOLCURVE_H */
