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
 * iPod UI shell: the alarm engine's inner line (W1-A1 only): the resident
 * ring screen (screens/alarmring.c), the dark wait, the Alarm lab. Not
 * part of the P12 contract; other packages use alarm.h.
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
#ifndef IPODUI_ALARM_RING_H
#define IPODUI_ALARM_RING_H

#include <stdbool.h>
#include <stdint.h>
#include "ipodui/alarm.h"
#include "ipodui/screen_stack.h"

/* How a ring ends (Q5, Q7): a snooze (one left, else a dismiss), a
 * dismiss, the ALARM_RING_S cap (a snooze if one is left, else stopped
 * and marked missed, 5.5), a key inside the USB screen (a dismiss) */
enum alarm_end { ALARM_END_SNOOZE, ALARM_END_DISMISS, ALARM_END_CAP,
                 ALARM_END_USB_KEY };
void alarm_ring_end(enum alarm_end how);
bool alarm_ring_can_snooze(void);
int  alarm_ring_index(void);            /* the first alarm ringing, or -1 */
uint32_t alarm_ring_rtc(void);          /* the occurrence ringing */

/* The beep pattern's burst (5.3), the ring and the lab's beep test: the
 * piezo (and the haptic mod on its line) for 100 ms */
void alarm_beep_burst(unsigned hz);

/* Q1(2): hold PLAY with an alarm due within ALARM_HOLD_OFF_S: playback
 * paused, the screen and the LCD off, awake and unboosted; the main
 * loop's deadline (alarm_timeout()) or a key ends it */
void alarm_dark_wait(void);

/* screens/alarmring.c: the resident ring screen (pushed by alarm_poll(),
 * UI_T_SYSTEM). alarm_ring_close(): pops it if it is on top (hold PLAY's
 * dismiss, before the sleep) */
extern const struct ui_screen_ops ui_alarm_ring_screen;
void alarm_ring_close(void);

#ifdef HAVE_IPOD_UI_DEVMODE
/* alarmlab.c: the developer Alarm lab menu (devmode.c lists it) */
struct menu_item_ex;
extern const struct menu_item_ex ipodui_dev_alarmlab_menu;
#endif

#endif /* IPODUI_ALARM_RING_H */
