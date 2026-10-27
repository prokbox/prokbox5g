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
 * iPod UI shell: automatic battery calibration (tracker #70;
 * docs/batcal-brief.md). Learns the discharge table from each full
 * run-down; nothing is shown outside developer mode. HAVE_BATTERY_LEARN
 * (tools/configure, --ipodui iPod Video builds); otherwise no-op inlines.
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
#ifndef _IPODUI_BATCAL_H_
#define _IPODUI_BATCAL_H_

#include <stdbool.h>
#include <stddef.h>

#ifdef HAVE_BATTERY_LEARN
/* ipodui_main(), once, after settings and battery_levels.cfg: the boot
 * decision on a run file (carry on, fit, discard), the learned table
 * applied, the 1 s sampler started */
void batcal_boot(void);
/* The shell's main loop (UI thread): a fit due at a run's end, and the
 * file writes, only while the disk spins and above disk-safe */
void batcal_service(void);
/* input.c, SYS_POWEROFF, before default_event_handler(): a shutoff marks
 * the run "ending" (written only above disk-safe). system_flush()'s write
 * on this thread marks it "clean" with or without this call. */
void batcal_poweroff(void);
/* Developer rows (devmode.c): the table in use, the run, and a reset
 * (both files removed, the boot-time table restored) */
void batcal_status(char *buf, size_t size);
void batcal_run_status(char *buf, size_t size);
void batcal_reset(void);
#else
static inline void batcal_boot(void) {}
static inline void batcal_service(void) {}
static inline void batcal_poweroff(void) {}
#endif

#endif /* _IPODUI_BATCAL_H_ */
