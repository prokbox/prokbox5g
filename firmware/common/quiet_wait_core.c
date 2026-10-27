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

/* P19 19-1a: see firmware/export/quiet_wait_core.h */
#include "quiet_wait_core.h"

bool qw_legacy;

long qw_min(long a, long b)
{
    if (a < 0)
        return b;
    if (b < 0)
        return a;
    return a < b ? a : b;
}

long qw_until(long now, long deadline)
{
    long left = deadline + 1 - now;
    return left > 0 ? left : 0;
}

long qw_audio_wait(const struct qw_audio_in *in, long hz)
{
    if (qw_legacy)
        return in->skip_pending ? hz / 10 : hz / 2;
    if (in->skip_pending)
        return hz / 10;         /* unchanged: the FIQ's notice is polled */
    /* paused: dac_idle_poll() counts the codec's idle time from here */
    if (in->paused && in->dac_pending)
        return hz / 2;
    return QW_BLOCK;
}

long qw_stor_wait(const struct qw_stor_in *in, long hz)
{
    long w;

    if (qw_legacy)
        return hz / 2;
    switch (in->state)
    {
    case QW_ST_ON:
        if (in->sleep_timeout <= 0)
            return QW_BLOCK;    /* never sleeps on its own */
        w = qw_until(in->now, in->last_activity + in->sleep_timeout);
        break;
    case QW_ST_SLEEPING:
        if (!in->off_armed)
            return QW_BLOCK;    /* no cut owed; the next access wakes it */
        w = qw_until(in->now, in->off_deadline);
        if (in->lit)
            w = qw_min(w, QW_STOR_LIT_CAP(hz));
        break;
    default:
        return QW_BLOCK;        /* off: nothing until an access */
    }
    if (w == 0)                 /* due: run the tick now, or retry */
        w = in->ticked ? QW_STOR_RETRY(hz) : 1;
    return w;
}

long qw_power_wait(const struct qw_power_in *in, long hz)
{
    long step = QW_POWER_STEP(hz);
    long w;

    if (in->quiet)
        step = (in->dark_playing && !qw_legacy) ? QW_POWER_DARK(hz)
                                                : QW_POWER_QUIET(hz);
    w = in->last_step + step - in->now;
    if (w < 0)
        w = 0;
    if (in->sleeptimer && !qw_legacy)
        w = qw_min(w, qw_until(in->now, in->sleep_end));
    return w;
}

long qw_tc_earliest(long now, long d, long cand)
{
    if (cand - now <= 0)
        return d;               /* due or past: its pass already ran */
    if (d == QW_BLOCK || cand - d < 0)
        return cand;
    return d;
}

long qw_tc_wait(bool poll, long now, long deadline, long hz)
{
    long w;

    if (poll || qw_legacy)
        return hz;
    w = QW_TC_CAP(hz);
    if (deadline != QW_BLOCK && deadline - now > 0)
        w = qw_min(w, qw_until(now, deadline));
    return w < hz ? hz : w;
}
