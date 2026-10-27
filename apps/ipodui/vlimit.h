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
#ifndef IPODUI_VLIMIT_H
#define IPODUI_VLIMIT_H

#include <stdbool.h>
#include "ipodui/screen_stack.h"

/* Phase 11 Volume Limit (docs/phase11-brief.md sections 3.5, 4.6).
 * Contract, committed before fan-out. vlimit.c (U2a) implements all but
 * ui_vol_frac(), which the contract commit implements in np_playback.c.
 * Storage: upstream global_settings.volume_limit (enforced by sound.c
 * and np_playback.c adjust_volume()) and ipodui_vlimit_code (-1 = none,
 * else 0..9999). */

/* 0..1024 along the NP volume bar for a volume in dB (sound_min..
 * sound_max(SOUND_VOLUME), the linear mapping the bar's fill uses,
 * np_pb_volume_pct()); clamped. Pure (Rev 1 L-8). U2a's limit screen and
 * Q's triangle both call it; neither re-derives the mapping. */
int  ui_vol_frac(int vol);   /* P15: via the volume curve (volcurve.h) */
bool ui_vlimit_active(void);             /* volume_limit < max volume */
bool ui_vlimit_locked(void);             /* ipodui_vlimit_code >= 0 */
/* sound_set_volume(global_status.volume): call after EVERY volume_limit
 * write (the limit screen, setting or removing the code, Reset's restore
 * of the kept limit; Rev 1 M-5), so the clamp applies at once.
 * Trace: "vlimit set=<db> code=<0|1> applied=<vol>",
 *        "vlimit unlock ok=<0|1>". */
void ui_vlimit_apply(void);
extern const struct ui_screen_ops ui_vlimit_screen;

#endif /* IPODUI_VLIMIT_H */
