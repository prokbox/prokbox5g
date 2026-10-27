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
 * The shell's DSP settings, applied on the CPU (P19 ucop-fix2)
 */
#ifndef IPODUI_AUDIO_OUT_H
#define IPODUI_AUDIO_OUT_H

#include <stdbool.h>
#include "eq.h"

/* The software EQ's whole state in one call: bands 0..n-1, the precut and
 * the enable.  The codec's DSP reads these on the CPU, so the writes run
 * there too: with the UI on the COP this is one remote call
 * (febe_rpc.list), served by be_svc with no yield between the bands, so
 * the codec sees the old set or the new one, never half of each (per-band
 * calls, each its own round trip, would let the codec run between them:
 * tests/audioxcore).  Inline or on a CPU thread: a plain call. */
void ui_eq_dsp_set(const struct eq_band_setting *bands, int n, int precut,
                   bool enabled);

#endif /* IPODUI_AUDIO_OUT_H */
