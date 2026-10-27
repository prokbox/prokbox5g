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

/*
 * The shell's DSP settings, applied on the CPU (P19 ucop-fix2): audio_out.h
 */
#include "config.h"
#include "system.h"
#include "xcore.h"
#include "ipodui/audio_out.h"

void ui_eq_dsp_set(const struct eq_band_setting *bands, int n, int precut,
                   bool enabled)
{
    int i;

    XCORE_ASSERT_CPU("ui_eq_dsp_set");
    for (i = 0; i < n; i++)
        dsp_set_eq_coefs(i, &bands[i]);
    dsp_set_eq_precut(precut);
    dsp_eq_enable(enabled);
}
