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


#ifndef _IPODUI_SLEEP_H_
#define _IPODUI_SLEEP_H_

/* Phase 18-2 (#86), the shell's sleep policy (docs/phase18-brief.md
 * §3.2, §4.3): hold PLAY (and, once 18-1 routes it, the idle timer)
 * soft-sleeps instead of powering off. Everything below runs on the UI
 * thread; the pure rules are in sleep_core.c. */

#include <stdbool.h>
#include <stdint.h>

#include "kernel.h"     /* enum ui_sleep_reason (queue.h, with
                         * SYS_SLEEP_REQUEST: powermgmt.c posts it) */
enum ui_sleep_tier { TIER_SOFT, TIER_OFF };
enum sleep_quiesce { SQ_SLEEP, SQ_OFF, SQ_USB, SQ_WOKEN };

/* The window caps (Q7, Q11) and the bound on the steps after it */
#define SLEEP_MAINT_MS      30000
#define SLEEP_WALK_MS       60000
#define SLEEP_QUIESCE_MS    5000

/* Records a request; ui_main.c's loop runs it (ui_sleep_run()) at its
 * next pass. UI thread today (hold PLAY, the charging screen); the idle
 * and Sleep Timer paths need 18-1's SYS_SLEEP_REQUEST to reach it from
 * other threads. */
void ui_sleep_request(enum ui_sleep_reason r);
bool ui_sleep_pending(void);
/* The developer "Soft sleep: Off / On" switch. Off (the default, and the
 * state of a build whose platform sleep has not run on silicon yet): every
 * sleep request takes today's power-off. Kept by the marker file
 * SOFT_SLEEP_MARKER. ui_sleep_soft_boot() reads it (ui_main.c, early). */
#define SOFT_SLEEP_MARKER   ROCKBOX_DIR "/soft_sleep"
void ui_sleep_soft_boot(void);
bool ui_sleep_soft_get(void);
void ui_sleep_soft_set(bool on);
/* The whole sleep: tier, quiesce, power_sleep(), wake. True when the
 * unit came back (a wake, SQ_WOKEN or SQ_USB): the caller redraws the
 * top screen in full from the RTC, then lights it (ui_sleep_lit()).
 * False: nothing happened (ignored) or a power-off is under way. */
bool ui_sleep_run(void);
/* input.c, at SYS_USB_CONNECTED before its handler: the entry window, if
 * open, closes and its boost goes (a no-op otherwise) */
void ui_sleep_window_close(void);
/* After the caller's first full frame: the backlight, then
 * SLEEP_EVENT_WAKE's work */
void ui_sleep_lit(void);
/* input.c, every button event: true = swallowed (the waking press, PLAY
 * held into the sleep, the key that ended the entry), up to its
 * release */
bool ui_sleep_swallow(long btn);
enum ui_sleep_tier ui_sleep_tier(int mv, int pct, bool charger,
                                 bool usb, bool supported);
uint32_t ui_sleep_deep_after_s(void);
/* The RTC in seconds since 2000 (sleep_rtc_from_date()), any thread: the
 * Q11 walk's calendar */
uint32_t ui_sleep_rtc_now(void);

/* Review H1: what power_sleep() refuses on, checked before calling it
 * (the boost count, a PLL hold, the clock, PCM): SLEEP_REFUSE_* or 0. */
int sleep_quiet_why(void);
/* Waits until 'deadline' (ticks) for sleep_quiet_why() to clear: a pause's
 * mixer tail (~3 s), the lit screen's PLL hold, the boosts. 'slice' NULL
 * sleeps HZ/10 between looks; otherwise it waits one slice and a result
 * other than SQ_SLEEP (in *q) stops the wait. Returns the refusal still
 * standing (0: quiet). Sleep entry and the Sleep lab share it. */
int sleep_quiesce_wait(long deadline, enum sleep_quiesce (*slice)(void),
                       enum sleep_quiesce *q);

/* Review M1: the storage users held from sleep entry to the wake (the
 * buffer refill, the art builder, maint's paused window); UI thread.
 * Sleep entry and the Sleep lab share it. */
void ui_sleep_storage_hold(bool hold);

/* What still keeps a sleep from starting, each part apart (the Sleep
 * lab's wait and its log): sleep_quiet_why()'s checks plus the drive's
 * last transfer (within HZ/5), an art pass at work and a maint job */
struct sleep_busy {
    int boost;          /* cpu_boost count (sim: 1 while a pll_hold stands) */
    unsigned holds;     /* CPU_PLL_HOLD_* bits */
    bool clock, pcm, disk, art, maint;
};
/* Fills *b; true when any part is busy */
bool sleep_busy_read(struct sleep_busy *b);

#endif /* _IPODUI_SLEEP_H_ */
