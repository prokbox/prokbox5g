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

#ifndef DACOSR_STUB_AUDIOHW_H
#define DACOSR_STUB_AUDIOHW_H
#include "system.h"
#define AUDIOHW_SETTING(...)
#include "wm8758.h"
#include "pcm_sampr.h"
void audiohw_preinit(void);
void audiohw_postinit(void);
void audiohw_close(void);
void audiohw_set_frequency(int fsel);
void audiohw_set_depth(int bits);
void audiohw_set_volume(int vol_l, int vol_r);
void audiohw_set_lineout_volume(int vol_l, int vol_r);
void audiohw_enable_lineout(bool enable);
void audiohw_set_bass(int value);
void audiohw_set_bass_cutoff(int value);
void audiohw_set_treble(int value);
void audiohw_set_treble_cutoff(int value);
void audiohw_enable_recording(bool source_mic);
void audiohw_disable_recording(void);
void audiohw_set_recvol(int left, int right, int type);
void audiohw_set_monitor(bool enable);
#endif
