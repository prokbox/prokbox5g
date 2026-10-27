/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2022 Aidan MacDonald
 * Modified for ProkBox5g (2026); see docs/changes-from-rockbox.md.
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
#ifndef _DSP_RESAMPLE_H
#define _DSP_RESAMPLE_H

/* Conversion quality (P19 19-13). Takes effect at the next rate change.
 *   HERMITE: 4-point Hermite for every ratio (the upstream behaviour);
 *   INTEGER: half-band filters for /2, /4, x2, x4, x3 and x6 (default);
 *   ALL:     INTEGER plus 32 -> 48 kHz (3:2), which costs about three
 *            times Hermite. */
#define RESAMPLE_Q_HERMITE 0
#define RESAMPLE_Q_INTEGER 1
#define RESAMPLE_Q_ALL     2
void dsp_resample_set_quality(unsigned int quality);

void dsp_resample_init(struct dsp_config *dsp, unsigned int dsp_id) INIT_ATTR;

#endif /* _DSP_RESAMPLE_H */
