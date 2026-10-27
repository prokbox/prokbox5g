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
 * iPod UI shell
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
#ifndef _IPODUI_INPUT_H_
#define _IPODUI_INPUT_H_

#include <stdbool.h>

enum ui_key { UI_KEY_NONE = 0, UI_KEY_MENU, UI_KEY_SELECT, UI_KEY_PLAY,
              UI_KEY_PREV, UI_KEY_NEXT, UI_KEY_WHEEL };

struct ui_input {
    enum ui_key key;            /* UI_KEY_NONE for chords (see raw) */
    bool release;               /* button released (act on this for SELECT/MENU) */
    bool repeat;                /* held, repeating */
    bool long_press;            /* first repeat past ~HZ/2, fired once. Also
                                   set on the release of a press that already
                                   fired its long press: skip the short action */
    int  wheel_steps;           /* UI_KEY_WHEEL: signed, after acceleration */
    unsigned wheel_velocity;    /* clicks/s (see scroll.h), 0 when idle */
    long raw;                   /* original button code */
};

/* Blocks up to 'timeout' ticks (TIMEOUT_BLOCK allowed). Handles SYS_*
 * events internally via default_event_handler(). Returns false on timeout. */
bool ui_input_get(struct ui_input *in, int timeout);

/* The wheel's speed (clicks/s, as ui_input.wheel_velocity) at its last
 * event, or 0 once the wheel has rested ~250 ms (the driver's own reset):
 * for code that moves on the wheel without seeing the event (the list's
 * fast-scroll row jumps, list.c). */
unsigned ui_input_wheel_speed(void);

#endif /* _IPODUI_INPUT_H_ */
