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

#ifndef _IPODUI_MQ_JOBS_H_
#define _IPODUI_MQ_JOBS_H_

/* P19 19-5: a marquee as the BCM2722's stepped PAN (docs/research/
 * bcm2722/jobs.md section 5, "Shell recipe: the marquee"; bcmjob.h).
 *
 * The shell renders the label's strip once (F2) and uploads it into a
 * J_CLS_STRIP slot (by content key: the same label again is an ALLOC hit,
 * no upload); then one ANIM PAN in its stepped form draws F2's frames on
 * the BCM's own 50 ms grid: 2 px a step, a 1 s pause at rest, lap after
 * lap.  While it runs the shell has no marquee wakeups and pushes nothing
 * over the window.  Every stop is ANIM_STOP, FENCE and a wait before the
 * CPU draws the row again; the CPU's F2 then goes on from the phase.
 *
 * One marquee at a time (one ANIM runs on the BCM at a time), on the UI
 * thread (the job owner).  Today's user: Now Playing's title. */

#include <stdbool.h>
#include <stdint.h>
#include "config.h"

#if defined(HAVE_BCM_JOBS) || defined(HAVE_BCM_JOBS_MODEL)
#define HAVE_MQ_JOBS

/* F2's rule (list.c, nowplaying.c), in ms: the PAN's words */
#define MQJ_STEP_MS     50
#define MQJ_STEP_PX     2
#define MQJ_PAUSE_MS    1000

/* Whether a marquee may go to the BCM now: the developer switch (the
 * drift's A/B row: auto is the device's verified image, never the
 * simulator's model unless a script asks), ANIM and ANIM_STOP offered,
 * J_VERSION's minor 3 or more, and no DECODE in flight.  'why' (may be
 * NULL): the reason when not */
bool mqj_available(const char **why);

/* Start: the strip px (w x h, pitch w; pinned or static: the upload
 * yields) under 'key', a lap of 'lap' px (the label's width plus its
 * gap; x0 = 0, the rest window the strip's head), shown through a
 * win_w x h window at (dst_x, dst_y), the first frame 'start_ms' into the
 * lap (1000: step 1, sent at F2's first step).  'settle' is called with
 * the phase when something else stops it (ui_fb_settle(): a slide, a
 * takeover), to write the frame the glass shows into the framebuffer.
 * false: F2 for now (traced why). */
bool mqj_start(const uint16_t *px, int w, int h, uint64_t key, int lap,
               int win_w, int dst_x, int dst_y, uint32_t start_ms,
               void (*settle)(long phase));

/* Running in this epoch (an epoch change ends it: the restart showed the
 * framebuffer).  The UI thread's (it may end the PAN's state) */
bool mqj_running(void);
/* A PAN was started and not stopped yet: a plain read, for any thread
 * (art_bcm_ready() on the art thread) */
bool mqj_active(void);

/* ANIM_STOP, FENCE, wait.  The phase (ms into the lap) the glass shows,
 * F2's to go on from; -1 if none was running.  After a stop that timed
 * out the window's band is resynced (bcmjob_resync) once the caller has
 * drawn it: mqj_stop() leaves that to the caller's push, which follows */
long mqj_stop(const char *why);

/* The phase now of the running (or last) PAN, -1 none */
long mqj_phase(void);

/* screen truth: a rect about to be read or written as the screen
 * (ui_fb_settle()) that meets the window stops it, and the owner's
 * settle draws the phase's frame into the framebuffer */
void mqj_settle(int x, int y, int w, int h);

/* F2's offset and its wait from a phase: mq_x, and ms to its next step */
static inline int mqj_phase_x(long u, int *wait_ms)
{
    if (u < MQJ_PAUSE_MS)
    {
        *wait_ms = (int)(MQJ_PAUSE_MS - u);
        return 0;
    }
    u -= MQJ_PAUSE_MS;
    *wait_ms = (int)(MQJ_STEP_MS - u % MQJ_STEP_MS);
    return MQJ_STEP_PX * (int)(u / MQJ_STEP_MS + 1);
}
#endif

#endif
