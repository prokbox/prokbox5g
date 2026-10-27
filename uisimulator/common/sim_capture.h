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
 * Simulator: capture the current LCD frame to a PNG (Phase 8a contract,
 * docs/phase8a-brief.md §8, Revision 1 M5). Called by apps/main.c
 * show_logo_boot() under SIMULATOR. Writes $IPODUI_SHOT_DIR/<name>.png
 * when IPODUI_SHOT_DIR is set; a no-op otherwise (sim_capture.c).
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
#ifndef _SIM_CAPTURE_H_
#define _SIM_CAPTURE_H_

#ifdef SIMULATOR
void sim_capture_screen(const char *name);
#endif

#endif /* _SIM_CAPTURE_H_ */
