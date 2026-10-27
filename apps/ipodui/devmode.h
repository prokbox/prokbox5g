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
 * iPod UI shell: hidden developer mode, MINIMAL scope (Phase 8a contract,
 * docs/phase8a-brief.md §3.4, §4.5; Revision 1 H1, M3, M6, L1, L6, L7)
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
#ifndef _IPODUI_DEVMODE_H_
#define _IPODUI_DEVMODE_H_

#include <stdbool.h>

struct ui_screen;
struct ui_screen_ops;

/* The About stub, .name "about" (screens/stub.c, U2). Devmode arms on
 * top->ops == &ui_about_screen (M6); hook H3 points the About row at it. */
extern const struct ui_screen_ops ui_about_screen;

#ifdef HAVE_IPOD_UI_DEVMODE
#define DEVMODE_CHORD       (BUTTON_SELECT | BUTTON_LEFT)
#define DEVMODE_HOLD_TICKS  (3 * HZ)
/* ui_main.c's global input layer, with in->raw, before the top screen;
 * true = consumed */
bool ipodui_devmode_feed(const struct ui_screen *top, long raw_btn);
/* The Developer list (LANG_IPODUI_DEVELOPER: LANG_DEBUG, LANG_BACK); returns.
 * Wraps do_menu() in exactly one viewportmanager_theme_enable(i, true, NULL)
 * / viewportmanager_theme_undo(i, false) pair per screen and never pushes
 * on return (VPSTACK_DEPTH is 16). On return: scroll_stop(), the shell's
 * fonts and viewport, header cache invalidated, one full redraw. Calls the
 * idempotent ui_log_init(). */
void ipodui_devmode_run(void);
/* main.c, before the shell: button_status() & BUTTON_RIGHT (L7) */
bool ipodui_devmode_boot_requested(void);
/* true only while ipodui_devmode_run() is on the stack (M3): splashes and
 * prompts pass through the funnel then */
bool ipodui_stock_ui_allowed(void);
#ifdef HAVE_STORAGE_POWEROFF
/* #114 H3: the drive-power A/B (devmode.c); service and wait are called
 * by the shell's main loop, start/stop by the row and the sim */
void ipodui_devmode_ab_start(void);
void ipodui_devmode_ab_stop(void);
void ipodui_devmode_ab_config(int hours, int step_min);
void ipodui_devmode_ab_service(void);
int ipodui_devmode_ab_wait(void);
#ifdef SIMULATOR
extern long ipodui_ab_period_ms, ipodui_ab_grace_ms;
#endif
#else
static inline int ipodui_devmode_ab_wait(void) { return -1; }
#endif
#ifdef HAVE_DEVSTATS
/* Fit-3 step 0: the once-a-minute counters line (UI thread, main loop) */
void ipodui_devmode_f3_service(void);
#endif
#else
static inline bool ipodui_stock_ui_allowed(void) { return false; }
static inline int ipodui_devmode_ab_wait(void) { return -1; }
#endif

#endif /* _IPODUI_DEVMODE_H_ */
