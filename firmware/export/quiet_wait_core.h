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

/* P19 19-1a, quiet pollers: how long each back-end thread may wait.
 * Each thread waits for the earliest of its own deadlines (18-A's
 * pattern) instead of a fixed poll; an event (a queue post or a kick)
 * still ends the wait at once. Pure logic, no Rockbox calls (host test:
 * tests/quietwait/). Times are in ticks; QW_BLOCK means "no deadline,
 * wait for an event". */
#ifndef QUIET_WAIT_CORE_H
#define QUIET_WAIT_CORE_H

#include <stdbool.h>

#define QW_BLOCK    (-1L)   /* TIMEOUT_BLOCK */

/* Developer fallback: true = every thread keeps its old fixed poll
 * (an A/B against the old cadence). Default false. */
extern bool qw_legacy;

/* the earlier of two waits (QW_BLOCK is "never") */
long qw_min(long a, long b);
/* the wait until the tick after 'deadline' (the callers' TIME_AFTER
 * tests are strict); 0 when it has passed */
long qw_until(long now, long deadline);

/* The audio thread while playback is active (playback.c). */
struct qw_audio_in {
    bool skip_pending;      /* an automatic track change is in progress:
                             * the PCM side's notice is only polled */
    bool paused;            /* playback paused */
    bool dac_pending;       /* a codec standby is still to come */
};
long qw_audio_wait(const struct qw_audio_in *in, long hz);

/* The storage thread (storage.c / ata.c). */
enum qw_stor_state { QW_ST_OFF = 0, QW_ST_SLEEPING, QW_ST_ON };
struct qw_stor_in {
    long now;
    int state;              /* enum qw_stor_state */
    long last_activity;     /* tick of the last access */
    long sleep_timeout;     /* standby this long after it; 0: never */
    bool off_armed;         /* a power cut is due at off_deadline */
    long off_deadline;      /* (the cut comes on the tick after it) */
    bool lit;               /* the hold may shrink when the panel goes
                             * dark, which kicks nobody: capped */
    bool ticked;            /* the previous wake ran the tick and the
                             * state did not move (a failed standby) */
};
#define QW_STOR_RETRY(hz)   ((hz) / 2)  /* the old poll, for retries */
#define QW_STOR_LIT_CAP(hz) (2 * (hz))
long qw_stor_wait(const struct qw_stor_in *in, long hz);

/* The power thread (powermgmt.c): the next step's tick. */
struct qw_power_in {
    long now;
    long last_step;
    bool quiet;             /* 18-A's quiet: no charger, USB, shutdown,
                             * above 15 % */
    bool dark_playing;      /* playing with the backlight off */
    bool sleeptimer;        /* the sleep timer runs */
    long sleep_end;         /* its end tick */
};
#define QW_POWER_STEP(hz)       ((hz) / 2)  /* stock */
#define QW_POWER_QUIET(hz)      (2 * (hz))  /* 18-A */
#define QW_POWER_DARK(hz)       (10 * (hz)) /* 19-1a */
long qw_power_wait(const struct qw_power_in *in, long hz);

/* The tagcache thread (tagcache.c): its old 1 s poll while work is owed
 * or the panel is lit, else the earliest time-based deadline, capped.
 * The wait is computed after a pass, so a deadline that is not in the
 * future already had its pass: its work is blocked by something else (a
 * gate, a closed window) and only a future deadline, a kick or the cap
 * may end the wait. Never under hz (review H1: a past deadline spun at
 * one tick). */
#define QW_TC_CAP(hz)   (30 * (hz))
long qw_tc_wait(bool poll, long now, long deadline, long hz);
/* the earlier of 'd' and 'cand', counting 'cand' only if it is after
 * 'now' (QW_BLOCK: none yet) */
long qw_tc_earliest(long now, long d, long cand);

#endif /* QUIET_WAIT_CORE_H */
