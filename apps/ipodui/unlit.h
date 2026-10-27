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
 * iPod UI shell: the unlit clock face (#51; Phase 8a contract,
 * docs/phase8a-brief.md §3.5, §4.4; Revision 1 H2, H4, M1, M2, L6).
 * Geometry: the "stock" variant of docs/design/unlit.md, frozen.
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
#ifndef _IPODUI_UNLIT_H_
#define _IPODUI_UNLIT_H_

#include <stdbool.h>
#include <stdint.h>
#include "ipodui/input.h"

#define UNLIT_DIGIT_Y   62
#define UNLIT_DIGIT_H   68
#define UNLIT_CELL_W    30
#define UNLIT_STROKE    9
#define UNLIT_RADIUS    3
#define UNLIT_INK       56              /* grey levels, quantised to RGB565 */
#define UNLIT_RIM       150
#define UNLIT_STATUS_Y  161             /* status band y 161..197 */
#define UNLIT_STATUS_H  37

/* -- pure core: unlit_core.c, host-tested (tests/unlit/) ---------------- */

enum ui_unlit_ev {
    UE_BL_OFF,                          /* SYS_IPODUI_BACKLIGHT_OFF */
    UE_BL_ON_NOKEY, UE_KEY, UE_KEY_RELEASE, UE_WHEEL, UE_BUTTONS_UP,
    UE_HOLD_ON, UE_HOLD_OFF,            /* from SYS_IPODUI_HOLD_CHANGED (H4) */
    UE_USB, UE_SLEEP_NEXT, UE_DEVMODE, UE_MINUTE, UE_STATUS, UE_TICK,
};

enum ui_unlit_act {                     /* bitmask */
    UA_NONE = 0, UA_DRAW_FULL = 1, UA_DRAW_DIGITS = 2, UA_DRAW_STATUS = 4,
    UA_DISMISS = 8, UA_SWALLOW = 16, UA_SKIP = 32,
};

struct ui_unlit_status {                /* identity of the status row */
    unsigned char play;                 /* 0 stopped, 1 playing, 2 paused */
    unsigned char hold, charging;
    unsigned char fill_px;              /* battery fill width drawn, not % */
};

struct ui_unlit_state {
    bool face;                          /* NORMAL / FACE */
    bool swallowing;                    /* drop keys until the dismissing release */
    long swallow_since;                 /* tick of the dismiss (M1 fallbacks) */
    bool skip_next;                     /* the next BL_OFF is the shell's own sleep */
    long skip_stamp;                    /* tick skip_next was set; expires (M2) */
    short shown_min;                    /* identity: minute of day drawn, -1 none */
    bool shown_12h;                     /* identity: format drawn */
    struct ui_unlit_status shown;       /* identity: status drawn */
    bool fb_valid;                      /* validity: framebuffer still holds the face */
};

/* now = current_tick; lit = is_backlight_on(true) when ev arrived.
 * M1: UE_BL_OFF enters FACE only if !lit. In FACE a lone release
 * (UE_KEY_RELEASE with no press seen) also dismisses. Swallowing ends on
 * the dismissing key's UE_KEY_RELEASE, on UE_BUTTONS_UP
 * (button_status() == 0), or UNLIT_SWALLOW_MAX after the dismiss; UE_WHEEL
 * is swallowed for UNLIT_WHEEL_SWALLOW after the dismiss.
 * M2: skip_next expires UNLIT_SKIP_TICKS after it was set. */
#define UNLIT_SWALLOW_MAX    HZ          /* fallback end of swallowing */
#define UNLIT_WHEEL_SWALLOW  (HZ/4)      /* wheel quiet window after dismiss */
#define UNLIT_SKIP_TICKS     HZ          /* skip_next lifetime */
unsigned ui_unlit_step(struct ui_unlit_state *s, enum ui_unlit_ev ev,
                       long now, bool lit);
int ui_unlit_ticks_to_minute(int sec_of_minute);    /* (60 - s) * HZ, min 1 */

/* -- shell side: unlit.c (UI thread only) ------------------------------- */

bool ui_unlit_active(void);
/* input.c: SYS_IPODUI_BACKLIGHT_OFF, SYS_IPODUI_HOLD_CHANGED */
void ui_unlit_sysevent(long ev, intptr_t data);
bool ui_unlit_filter(const struct ui_input *in);    /* true = consumed */
/* P12: the face leaves without a key, for a screen that must show over it
 * (an alarm ring); trace "unlit exit <why>" */
void ui_unlit_dismiss(const char *why);
/* ticks to the next minute boundary; ui_main.c min_waits it with
 * np_pb_idle()'s wake (H2: FACE suspends drawing, never logic) */
int  ui_unlit_timeout(void);
void ui_unlit_tick(void);               /* minute / status redraw + band push */
void ui_unlit_suppress_next(void);      /* right before the sleep path's backlight_off() */
void ui_unlit_foreign_draw(void);       /* someone else drew: fb_valid = false */
/* Drawing contract (L6): digits, colon, play, pause, battery outline and
 * fill, the padlock (Hold) and the charging bolt (XOR over the fill), all
 * procedural. scroll_stop() on FACE enter (no marquee writes under the
 * face). */

#endif /* _IPODUI_UNLIT_H_ */
