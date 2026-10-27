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
 * iPod UI shell: Stopwatch, the pure rules (P12, docs/phase12-brief.md
 * 3.4): elapsed time from the RTC and the tick, laps, the log record,
 * stopwatch.dat seal/check. Host-tested in tests/stopwatch; owner W1-S
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
#include <stddef.h>
#include <string.h>
#include "ipodui/stopwatch_core.h"
#include "crc32.h"

/* A tick count that does not run with the RTC (frozen by a soft sleep,
 * or never started alongside it) must not be trusted: its sub-second
 * part only counts while it agrees with the RTC within a second */
#define AGREE_MS 1000

uint32_t sw_elapsed_ms(const struct sw_state *s, uint32_t now_rtc,
                       long now_tick)
{
    uint32_t rtc_ms;
    int32_t dt;

    if (!s->running)
        return s->acc_ms;
    rtc_ms = now_rtc > s->start_rtc ? (now_rtc - s->start_rtc) * 1000u : 0;
    dt = (int32_t)((uint32_t)now_tick - (uint32_t)s->start_tick);
    if (dt >= 0)
    {
        uint32_t tick_ms = (uint32_t)dt * SW_TICK_MS;
        uint32_t d = tick_ms > rtc_ms ? tick_ms - rtc_ms : rtc_ms - tick_ms;

        if (d <= AGREE_MS)
            return s->acc_ms + tick_ms;
    }
    return s->acc_ms + rtc_ms;
}

static uint32_t laps_sum(const struct sw_state *s)
{
    uint32_t sum = 0;
    int i;

    for (i = 0; i < s->nlaps; i++)
        sum += s->lap_ms[i];
    return sum;
}

void sw_lap(struct sw_state *s, uint32_t now_ms)
{
    uint32_t sum = laps_sum(s);
    uint32_t len = now_ms > sum ? now_ms - sum : 0;

    if (s->nlaps < SW_LAPS_MAX)
        s->lap_ms[s->nlaps++] = len;
    else
        s->lap_ms[SW_LAPS_MAX - 1] += len;
}

void sw_start(struct sw_state *s, uint32_t now_rtc, long now_tick)
{
    if (s->running)
        return;
    s->start_rtc = now_rtc;
    s->start_tick = (int32_t)now_tick;
    s->running = 1;
}

void sw_stop(struct sw_state *s, uint32_t now_rtc, long now_tick)
{
    if (!s->running)
        return;
    s->acc_ms = sw_elapsed_ms(s, now_rtc, now_tick);
    s->running = 0;
}

void sw_reset(struct sw_state *s)
{
    memset(s, 0, sizeof(*s));
}

void sw_log_make(const struct sw_state *s, uint32_t now_ms,
                 struct sw_log *out)
{
    uint32_t sum = laps_sum(s), run = now_ms > sum ? now_ms - sum : 0;
    uint32_t total = 0, shortest = 0, longest = 0;
    int n = 0, i, k;
    bool more = run > 0 || s->nlaps == 0;    /* the running lap counts */

    memset(out, 0, sizeof(*out));
    out->start_rtc = s->start_rtc;
    for (i = 0; i < s->nlaps + (more ? 1 : 0); i++)
    {
        uint32_t len = i < s->nlaps ? s->lap_ms[i] : run;

        if (n == 0 || len < shortest)
            shortest = len;
        if (n == 0 || len > longest)
            longest = len;
        total += len;
        n++;
    }
    out->total_ms = total;
    out->nlaps = n;
    out->shortest = shortest;
    out->longest = longest;
    out->average = n ? total / n : 0;
    /* the last laps, newest first */
    k = 0;
    if (more && k < SW_LAST_LAPS)
        out->last[k++] = run;
    for (i = s->nlaps - 1; i >= 0 && k < SW_LAST_LAPS; i--)
        out->last[k++] = s->lap_ms[i];
}

int sw_log_push(struct sw_log *logs, int n, const struct sw_log *rec)
{
    if (n >= SW_LOGS_MAX)
        n = SW_LOGS_MAX - 1;                /* the oldest goes */
    memmove(&logs[1], &logs[0], n * sizeof(*logs));
    logs[0] = *rec;
    return n + 1;
}

void sw_logs_seal(struct sw_logs_file *f, int count)
{
    f->magic = SW_LOGS_MAGIC;
    f->version = SW_LOGS_VERSION;
    f->count = (uint16_t)count;
    f->crc = crc_32(f->log, count * sizeof(struct sw_log), 0xffffffff);
}

int sw_logs_check(const struct sw_logs_file *f, uint32_t bytes)
{
    uint32_t need;

    if (bytes < offsetof(struct sw_logs_file, log))
        return -1;
    if (f->magic != SW_LOGS_MAGIC || f->version != SW_LOGS_VERSION)
        return -2;
    if (f->count > SW_LOGS_MAX)
        return -4;
    need = offsetof(struct sw_logs_file, log)
           + f->count * sizeof(struct sw_log);
    if (bytes < need)
        return -1;
    if (f->crc != crc_32(f->log, f->count * sizeof(struct sw_log), 0xffffffff))
        return -3;
    return 0;
}
