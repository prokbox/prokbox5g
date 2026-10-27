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
#ifndef _IPODUI_LITMISS_H_
#define _IPODUI_LITMISS_H_

/* The bench step lit24ui's missed-frame counters (developer builds only;
 * nothing is compiled elsewhere).  A viability check, never a power
 * measurement: while the step runs, the drift and the animation frames
 * (a list scroll) count the frames that were late or skipped, first with
 * the lit clock at 30 MHz (phase A), then at 24 (phase B, the clock
 * switched by the first frame after LITMISS_A_S).  Each hook is a flag
 * test and an increment; with the counters off, the flag test only. */

#include <stdbool.h>
#include <stdint.h>
#include "config.h"
#include "kernel.h"

#ifdef HAVE_IPOD_UI_DEVMODE

#define LITMISS_A_S         60      /* phase A's length; then B */
#define LITMISS_DRIFT_LATE  2       /* ticks past a drift step's due time */

struct litmiss
{
    bool on, b;                     /* counting; in phase B */
    long switch_at;                 /* tick of A's end */
    uint32_t drift_frames[2], drift_miss[2];    /* [0] A, [1] B */
    uint32_t scroll_frames[2], scroll_miss[2];
};

extern struct litmiss litmiss;

void litmiss_begin(void);           /* zero, clock 30, A starts */
void litmiss_end(void);             /* stop counting; the clock stays */
void litmiss_switch(void);          /* A ends: the lit clock to 24 */

static inline void litmiss_poll(void)
{
    if (!litmiss.b && TIME_AFTER(current_tick, litmiss.switch_at))
        litmiss_switch();
}

/* A drift step posted: 'steps' half steps moved since the last one (1
 * is on time, more were skipped), 'late' ticks after its due time */
static inline void litmiss_drift(int steps, int late)
{
    if (!litmiss.on)
        return;
    litmiss_poll();
    litmiss.drift_frames[litmiss.b]++;
    litmiss.drift_miss[litmiss.b] += (steps > 1 ? steps - 1 : 0) +
                                     (late > LITMISS_DRIFT_LATE);
}

/* An animation frame posted ('late': a whole frame slot or more past its
 * due time) */
static inline void litmiss_scroll(bool late)
{
    if (!litmiss.on)
        return;
    litmiss_poll();
    litmiss.scroll_frames[litmiss.b]++;
    litmiss.scroll_miss[litmiss.b] += late;
}

#define LITMISS_DRIFT(steps, late)  litmiss_drift(steps, late)
#define LITMISS_SCROLL(late)        litmiss_scroll(late)
#else
#define LITMISS_DRIFT(steps, late)  ((void)0)
#define LITMISS_SCROLL(late)        ((void)0)
#endif

#endif /* _IPODUI_LITMISS_H_ */
