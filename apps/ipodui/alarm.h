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
 * iPod UI shell: the resident alarm engine (P12 contract,
 * docs/phase12-brief.md 3.3, 5) Owner W1-A1; the rules are alarm_core.h
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
#ifndef IPODUI_ALARM_H
#define IPODUI_ALARM_H

/* UI thread only, resident (an armed alarm outlives every screen and the
 * pack: the ring screen never depends on a file loading). One timer
 * source (lean rule 5): the main loop's deadline. Trace lines
 * (UPDATE_TRACE/ui_log): "alarm arm rtc=%lu ok=%d", "alarm ring i=%d",
 * "alarm snooze", "alarm dismiss", "alarm missed i=%d at=%lu". */

#include <stdbool.h>
#include <stdint.h>
#include "ipodui/alarm_core.h"

/* Right after the settings load: reads alarms.dat (one read; a bad
 * magic/version/CRC is "no alarms", logged, rewritten at the next edit),
 * decides an alarm boot (alarm_is_boot() on the persisted armed_rtc),
 * marks misses, and re-arms the PMU. */
void     alarm_init(void);
/* After any edit, a Date & Time or Time Zone change, a ring, a wake: the
 * one place that recomputes the next time, arms the PMU (the full
 * register image, then RTCWAK), READS THE REGISTERS BACK and logs
 * "alarm arm rtc= ok=" (a mismatch retries once, then logs), and writes
 * alarms.dat if it changed (safefile, at once). Inside a USB session it
 * arms the PMU only and owes the file write to the post-USB path (M9).
 * With no alarm set it writes the disabled year once per change. The only
 * code that arms or disarms the PMU alarm (H2). */
void     alarm_rearm(void);
/* The next ring time (RTC s since 2000), 0 = none; for sleep.c's
 * sleep_req.alarm_rtc (wave 2) */
uint32_t alarm_next_rtc(void);
/* Q1(2): true while an alarm is due within ALARM_HOLD_OFF_S and the
 * bootloader alarm window is not on the unit: the shell's power-off
 * requests (hold PLAY, idle, Sleep Timer) take the dark wait instead */
bool     alarm_blocks_poweroff(void);
/* Main-loop deadline: ticks until the next ring while awake,
 * TIMEOUT_BLOCK if none. The loop calls alarm_poll() when it expires,
 * after every wake and at boot. */
long     alarm_timeout(void);
/* Rings if due: pauses playback (Q6), pushes the resident ring screen */
void     alarm_poll(void);
/* The same at a wake, before the first lit frame (ui_main.c): every
 * file write, owed or new, waits for the loop's next alarm_poll(), so a
 * hard drive spins up after the light, not before it */
void     alarm_poll_prelight(void);
/* sleep.c, after every soft-sleep wake (W2-W): the tick stood still, so
 * the deadline is derived again from the RTC; alarm = WAKE_ALARM, the
 * next loop pass rings it (however late the RTC reads) */
void     alarm_wake(bool alarm);
/* M2: the ring's light, owed by the prelight pass (alarm_poll_prelight())
 * until the wake's first frame is drawn; ui_sleep_lit() takes it (true:
 * light now, through Hold) */
bool     alarm_take_light(void);
/* Known limit (developer only): a developer screen that runs its own
 * button loop (the stock Rockbox debug menus behind the developer combo)
 * does not reach ui_main.c's loop, so a due alarm rings only when that
 * screen is left (late, or marked missed past ALARM_BOOT_WIN_S). The
 * shell's own screens, the USB screen (alarm_poll_usb()) and the unlit
 * face all keep the alarm deadline. Documented, not fixed (review). */
/* Q7: the same inside the USB screen's loop (usbscreen.c): the piezo
 * only, no screen change, any button stops it (btn: the event the loop's
 * button_get_w_tmo() returned); the file write deferred */
void     alarm_poll_usb(long btn);
bool     alarm_ringing(void);
/* The resident copies for the Alarms module (W1-A2): read, then edit
 * through alarm_set()/alarm_delete(), which call alarm_rearm() */
int      alarm_count(void);
const struct alarm *alarm_get(int i);
int      alarm_set(int i, const struct alarm *a);   /* i = -1: append; the
                                                       index, or -1 if full */
void     alarm_delete(int i);
/* Q17: the Alarms list was shown; clears every missed mark (one write) */
void     alarm_seen(void);
/* Reset Settings (screens/reset.c): deletes alarms.dat and disables the
 * PMU alarm through alarm_rearm() */
void     alarm_reset(void);

#endif /* IPODUI_ALARM_H */
