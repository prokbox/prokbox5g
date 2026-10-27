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

/* np-lag: the developer probe for Now Playing's responsiveness while a
 * codec decodes (docs/design/p19-placement.md section 4.4). Developer
 * device builds only (HAVE_DEVSTATS, not the simulator); elsewhere every
 * hook is an empty inline. Each hook is a flag test and a few adds.
 *
 * Three ipodui.log lines per 60 s window, only for a window in
 * which Now Playing was on top, lit, and saw a wheel event or a frame:
 *   uilat np n=N avg=A max=M q=QA/QM o33=X o100=Y
 *       wheel events (n), wheel-to-pixels ms (avg, max: the driver's post
 *       to the end of the next LCD push), the queue wait alone (q: post
 *       to the UI thread's dequeue, avg/max ms), and the events over one
 *       frame slot (33 ms) and over 100 ms
 *   uilat cpu codec=C% main=U% idle=I% s=S
 *       the window's share of the CPU core (S seconds) for the codec and
 *       main (UI) threads, and the core's sleep
 *   uilat fr n=F late=L ib=B/Tms
 *       animation frames drawn and late in Now Playing (a whole slot past
 *       due); input boosts taken and their held time (clockpol.c) */
#ifndef _IPODUI_UILAT_H_
#define _IPODUI_UILAT_H_

#include <stdbool.h>
#include "config.h"

#if defined(HAVE_DEVSTATS) && !defined(SIMULATOR)
void uilat_input(long btn);     /* input.c: a button event dequeued */
void uilat_pixels(void);        /* anim.c: an LCD push done */
void uilat_frame(bool late);    /* anim.c: an animation frame */
void uilat_service(void);       /* the main loop, every pass */
#else
static inline void uilat_input(long btn) { (void)btn; }
static inline void uilat_pixels(void) {}
static inline void uilat_frame(bool late) { (void)late; }
static inline void uilat_service(void) {}
#endif

#endif /* _IPODUI_UILAT_H_ */
