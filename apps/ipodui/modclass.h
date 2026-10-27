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
 * iPod UI shell: the Class A runtime accountant and boost bursts (P12 contract,
 * docs/phase12-brief.md 2.1, 2.3 item 3); owner W1-F (modclass.c)
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
#ifndef IPODUI_MODCLASS_H
#define IPODUI_MODCLASS_H

/* While a Class A module's screen is on top, the accountant counts its
 * storage reads and writes, core_alloc calls, boosted ticks and pushes,
 * and logs once per period:
 *   "modclass <name> class=A reads=%u writes=%u allocs=%u boost_pct=%u
 *    fps=%u pages=%u"
 * and "modclass viol <name> <rule>" on a breach (2.1's budgets). Trace
 * rule: every ipodui-p12-* script that opens an Extra logs at least one
 * modclass line and no modclass viol line. Sim and developer target
 * builds only (<= 1 KB); a release build compiles the hooks away. */

#include <stdbool.h>
#include <stdint.h>
#include "config.h"

/* Class A boost: one burst of at most ms (capped at 100 ms, the
 * transition's first frame, a deal, a parse step), released by
 * ui_boost_burst_end() or at the cap, whichever comes first; refused
 * (false, nothing boosted) past 10 % boosted time over any 10 s. Every
 * build, the only boost a Class A module may take (cpu_boost is on its
 * deny list). Balanced: one end per granted burst. */
bool ui_boost_burst(int ms);
void ui_boost_burst_end(void);
/* W1-F: the main loop's pass (mod_pass()) ends a burst at its cap, in
 * every build */
void ui_boost_burst_poll(void);

#if defined(SIMULATOR) || defined(HAVE_IPOD_UI_DEVMODE)
#define IPODUI_MODCLASS
/* screen_stack.c: the top screen changed; its module (enum mod_id) or -1
 * for a resident screen */
void modclass_top(int mod_id);
/* a module's entry: before the reads of its root's enter() (extras_open)
 * or of a folder or document opened inside it; opens an entry window
 * (2 storage commands in its first second). Another module: a new visit */
void modclass_entry(int mod_id);
/* the sim's file layer (W1-T): every file call, booked at once; only the
 * UI thread's count */
void modclass_io(bool write, uint32_t bytes);
/* safefile_replace(): its probes and renames between true and false are
 * one save, not the screen's reads */
void modclass_save(bool begin);
/* a page load of a paged list (brief 2.1, the manifest's pages = N):
 * the storage commands between true and false are one page load, booked
 * apart from the screen's reads. A breach: more than 2 commands (one
 * open, one read), or more than N page loads in a second. A module that
 * declares no pages books them as reads (the entry rule). */
void modclass_page(bool begin);
/* a core_alloc while counting (a buflib hook in developer builds) */
void modclass_alloc(void);
/* ui_lcd_push(): one push; frame false: not the module's frame (a
 * slide between screens, an SDK frame's later bands) */
void modclass_push(bool frame);
/* the main loop, once per pass: closes a period and logs */
void modclass_pass(void);
#else
static inline void modclass_top(int mod_id) { (void)mod_id; }
static inline void modclass_entry(int mod_id) { (void)mod_id; }
static inline void modclass_io(bool write, uint32_t bytes)
{ (void)write; (void)bytes; }
static inline void modclass_save(bool begin) { (void)begin; }
static inline void modclass_page(bool begin) { (void)begin; }
static inline void modclass_alloc(void) { }
static inline void modclass_push(bool frame) { (void)frame; }
static inline void modclass_pass(void) { }
#endif

#endif /* IPODUI_MODCLASS_H */
