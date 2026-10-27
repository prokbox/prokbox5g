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
 * iPod UI shell: the developer power log (tracker #10; docs/phase9-brief.md
 * section 3.6.5, section 4 contract; docs/hardware.md "Rundowns and A/B
 * tests"). Developer builds only (HAVE_IPOD_UI_DEVMODE); the release
 * build gets the no-op inlines below.
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
#ifndef _IPODUI_POWERLOG_H_
#define _IPODUI_POWERLOG_H_

#include <stdbool.h>

#ifdef HAVE_IPOD_UI_DEVMODE
#define POWERLOG_FILE        ROCKBOX_DIR "/power.csv"
#define POWERLOG_CFG         ROCKBOX_DIR "/power.cfg" /* present = on; holds
                                                       * the interval (s) */
/* Then it moves to .1 (one generation). A row is about 120 B: 4 MB is
 * about 4.8 days of 30 s rows, so one file holds a whole 3000 mAh
 * rundown (docs/hardware.md; batcal replays one file). The files are
 * pulled and cleared with each backup, not kept small here. */
#define POWERLOG_MAX         (4L * 1024 * 1024)
#ifdef SIMULATOR
extern long powerlog_max;               /* tests: "powerlog_max <bytes>" */
#endif
#define POWERLOG_PERIOD      30     /* s, the default row interval */
#define POWERLOG_RING_BYTES  7200   /* core_alloc while on; flushed at idle */
/* #114: present = "Drive power: standby only" (the developer switch) */
#define POWERLOG_STOR_STANDBY   ROCKBOX_DIR "/stor_standby"

/* ipodui_main(), once: resumes a log left on (POWERLOG_CFG), with a
 * "boot <date>" mark row. No write at boot. */
void powerlog_boot(void);
/* The developer toggle (UI thread): on writes POWERLOG_CFG and marks
 * "on"; off marks "off", writes the ring out and removes POWERLOG_CFG */
void powerlog_set(bool on);
bool powerlog_is_on(void);
/* Row interval in seconds: 5 (A/B), 30 (rundown) or 60. Persisted with
 * the toggle while on. */
void powerlog_set_interval(int secs);
int powerlog_interval(void);
/* A phase mark for test code: one row now, tagged (commas become '_', at
 * most 21 characters). Any CPU-core thread; a no-op while off. The ring
 * goes out at the next storage idle. */
void powerlog_mark(const char *tag);
/* The shell's main loop (UI thread): a due file rotation while the disk
 * spins, and the one forced write, when the ring is 7/8 full and the
 * disk hasn't gone idle since (it would overflow otherwise). Also the
 * Fit-1 M0 log lines (tap to PCM, disk wakes), log on or off. */
void powerlog_service(void);
#else
static inline void powerlog_boot(void) {}
static inline void powerlog_mark(const char *tag) { (void)tag; }
static inline void powerlog_service(void) {}
#endif

#endif /* _IPODUI_POWERLOG_H_ */
