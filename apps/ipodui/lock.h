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
 * iPod UI shell: Screen Lock gate (P12 contract, docs/phase12-brief.md 3.4,
 * Q10) Owner W1-L; the rules are lock_core.h
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
#ifndef IPODUI_LOCK_H
#define IPODUI_LOCK_H

/* The gate. ui_lock_active() is checked by input.c before any screen's
 * input (W1-L's one hook commit, brief section 6). Locked: only the
 * combination entry (wheel + SELECT), hold PLAY to sleep, the Hold
 * switch, and alarms ring over it (Q10). The state persists as two
 * settings, ipodui_lock_code (-1..9999, -1 = none) and ipodui_locked
 * (0/1), added to settings_list.c / settings.h by W1-L (guarded, ledger
 * row), so a lock survives sleep, power-off and reboot. Separate from
 * Volume Limit's combination (Q11). Resident: 4 B of state. */

#include <stdbool.h>
#include "ipodui/lock_core.h"

bool ui_lock_active(void);
void ui_lock_engage(void);              /* from Screen Lock's Lock row */
bool ui_lock_try(int code);             /* true = unlocked */
void ui_lock_usb_host(void);            /* data host connected: unlock */

/* -- W1-L additions (input.c's hook commit, the art Volume Limit shares) */
struct ui_input;
/* input.c, at the top of ui_input_get(): keeps the locked screen on the
 * stack while locked (a boot, a ring that went away) and takes it off
 * after a data host unlocked; writes a save owed since USB. True if the
 * stack changed (the caller returns, the main loop redraws). */
bool ui_lock_gate(void);
/* input.c, for every key event after the unlit face's filter: true to
 * swallow it. Locked: MENU never reaches the locked screen (no hold-MENU
 * pop past it), a system screen over it (a ring, Charging) gets every key
 * but a held MENU, and without the locked screen only PLAY passes (hold
 * PLAY to sleep). */
bool ui_lock_key(const struct ui_input *in);

/* The frosted combination body and the padlock (owner photo 1201),
 * shared by Screen Lock and Volume Limit (one copy, resident). Rows are a
 * function of y, so a band redraw repeats them exactly. */
#define UI_LOCK_ART_RIGHT 118           /* x right of the padlock */
unsigned ui_lock_body_row(int y);
void ui_lock_body(int x0, int y0, int y1);  /* rows y0..y1-1 from x0 */
void ui_lock_padlock(bool open);

/* The locked screen (resident: unlocking never depends on a file) */
extern const struct ui_screen_ops ui_lock_screen;

#endif /* IPODUI_LOCK_H */
