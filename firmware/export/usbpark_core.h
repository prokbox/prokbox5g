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

/* AW-2 (docs/pp5022-reference/apple-power-catalog.md): USB off again on a
 * charger with no host. Apple 1.3 powers the USB block on a VBUS edge,
 * waits ~100 ms for a host and powers the controller and PHY off when none
 * spoke (osos 0x1022_09FC [apple-RE]). ProkBox waits a wider window (slow
 * hosts), then firmware/usb.c parks the controller (usb_enable(false): the
 * USB clocks off and the PLL hold released) and keeps the state USB_POWERED
 * for the charger. A re-probe brings it back: every VBUS edge, and
 * usb_charger_reprobe(), which the shell calls when the user wakes the
 * screen.
 *
 * The fallback: a host that speaks inside a re-probe's window, with VBUS
 * never dropped since the park, was there all along and missed the
 * window: the window doubles (up to its max); USBPARK_LATE_TRIP of those
 * in a row and the park is given up for the session.
 *
 * Pure logic in ticks, no Rockbox calls (host test: tests/pwrawake/). */
#ifndef USBPARK_CORE_H
#define USBPARK_CORE_H

#include <stdbool.h>
#include <stdint.h>

/* 12 s: past usb_core.c's own no-host decision (10 s), so the charge
 * current the stack chose for a charger (500 mA with USB_CHARGING_FORCE)
 * is the one kept while parked; Apple waits ~100 ms [hypothesis] */
#define USBPARK_WINDOW_MS       12000
#define USBPARK_WINDOW_MAX_MS   48000
#define USBPARK_LATE_TRIP       2

enum usbpark_state
{
    USBPARK_OFF = 0,    /* no VBUS */
    USBPARK_WINDOW,     /* VBUS, the controller on, waiting for a host */
    USBPARK_HOST,       /* a host spoke: never parked until VBUS drops */
    USBPARK_PARKED,     /* a charger: the controller off */
};

struct usbpark
{
    uint8_t state;
    bool reprobed;      /* this window is a re-probe after a park */
    bool gave_up;       /* the fallback: never park again this session */
    uint8_t late_run;   /* late hosts in a row */
    long deadline;      /* the window's end */
    long window;        /* the window now, ticks */
    long window_max;
    uint32_t parks, reprobes, late_hosts;
};

void usbpark_init(struct usbpark *p, long window, long window_max);
/* a VBUS edge: on opens a fresh window */
void usbpark_vbus(struct usbpark *p, bool on, long now);
/* a host spoke (a bus reset, SET_ADDRESS, a transfer). Returns true when
 * it was a late host (the fallback's signal; the window has doubled) */
bool usbpark_host(struct usbpark *p);
/* the window ran out with no host: the caller parks the controller and
 * calls usbpark_parked() */
bool usbpark_due(const struct usbpark *p, long now);
void usbpark_parked(struct usbpark *p);
/* re-probe: true when the caller must enable the controller again (it
 * was parked); a fresh window starts */
bool usbpark_reprobe(struct usbpark *p, long now);

#endif /* USBPARK_CORE_H */
