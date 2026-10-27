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

#ifndef _IPODUI_SLEEPLAB_H_
#define _IPODUI_SLEEPLAB_H_

/* Phase 18-1: the developer Sleep lab (sleeplab.c), D2's device steps.
 * Each test runs one soft sleep on the UI thread and logs
 * "sleep wake why= s= polls= spurious= resyncs= buttons= hold_edges= ..."
 * plus, for the poll tests, one "sleep poll i= asked= rtc= us=" line per
 * timer wake.
 *
 * Device order (review, 2026-10-22): MENU+SELECT resets the unit while
 * awake on this build; the poll timer test (Hold off, a USB cable at
 * hand); then the wake rows (each button pressed and released apart, a
 * touch, a turn, USB, a charger; and: hold PLAY to sleep, release, then
 * press PLAY to wake: the wake press must not be eaten); fast polls; the
 * Hold-on-entry and "wheel kept on" rows; the alarm; the deep test in a
 * dark room; the countdown test last. */
enum sleeplab_test {
    SLAB_POLL_TIMER2,   /* Timer 2 polls of 30 s for 10 min */
    SLAB_POLL_WAITCNT,  /* PROC_WAIT_CNT seconds polls of 5 s for 2 min */
    SLAB_WAKE,          /* until a wake (30 min cap), polls of 60 s */
    SLAB_WAKE_FAST,     /* the same with polls of 2 s (10 min cap) */
    SLAB_WAKE_OPTO,     /* SLAB_WAKE with the opto kept powered */
    SLAB_ALARM,         /* an RTC alarm 2 min after entry */
    SLAB_DEEP,          /* 2 min, then the deep tier's dark power-off */
};

/* The device's lab, and the simulator's over its sleep stub (the same
 * entry, wait and restore; sim_sleep_run() in place of power_sleep()) */
#if defined(HAVE_IPOD_UI_DEVMODE) \
    && (defined(HAVE_POWER_SLEEP) || defined(SIMULATOR))
#define IPODUI_SLEEPLAB
/* The entry's wait for quiet (boost, PLL holds, PCM, the drive, the art
 * builder, maint), logged each second its state changes, else every
 * SLEEPLAB_LOG_S; still busy at the end, no sleep */
#define SLEEPLAB_QUIESCE_S  30
#define SLEEPLAB_LOG_S      5
int sleeplab_run(int test);     /* UI thread; always 0 (a menu return) */
/* The last run's outcome, one line for the lab's own row ("Slept 612 s,
 * woke: button", "Didn't sleep: busy (boost/disk)", "Refused: PCM") */
const char *sleeplab_last(void);
#endif

#endif /* _IPODUI_SLEEPLAB_H_ */
