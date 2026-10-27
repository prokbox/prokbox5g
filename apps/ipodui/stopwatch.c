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
 * iPod UI shell: the resident half of the Stopwatch (P12, docs/phase12-brief.md
 * 3.4): the running timing, stopwatch.dat, the panel; owner W1-S
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
#include "config.h"
#include <stdio.h>
#include <stddef.h>
#include <string.h>
#include "system.h"
#include "kernel.h"
#include "file.h"
#include "core_alloc.h"
#include "timefuncs.h"
#include "ipodui/anim.h"
#include "ipodui/safefile.h"
#include "ipodui/uilog.h"
#include "ipodui/stopwatch.h"

#define SW_DAT  ROCKBOX_DIR "/stopwatch.dat"
#define SW_TMP  ROCKBOX_DIR "/stopwatch.tmp"
#define SW_OLD  ROCKBOX_DIR "/stopwatch.old"

/* 144 B of state plus what the log needs and the RTC read cache: ~170 B,
 * the brief's "stopwatch 144 B" line and 25 B */
static struct sw_state sw;
static uint32_t first_rtc;      /* the first start of this timing (the log's
                                   date); sw.start_rtc moves at a resume */
static uint32_t rc_rtc;         /* the RTC at rc_tick */
static long rc_tick;
static bool rc_valid;
static bool begun;              /* a timing was started since New Timer */
static bool has_log;            /* log[0] of stopwatch.dat is this timing's
                                   (a resume then replaces it) */

/* days since 1970-01-01 of a civil date, and back (proleptic Gregorian):
 * the log's date is kept as seconds on this scale, with no time zone in it
 * (the RTC holds local time) */
static long days_from_civil(int y, int m, int d)
{
    long era, yoe, doy, doe;

    y -= m <= 2;
    era = (y >= 0 ? y : y - 399) / 400;
    yoe = y - era * 400;
    doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static uint32_t tm_rtc(const struct tm *tm)
{
    return (uint32_t)(days_from_civil(tm->tm_year + 1900, tm->tm_mon + 1,
                                      tm->tm_mday) * 86400L
                      + tm->tm_hour * 3600L + tm->tm_min * 60L + tm->tm_sec);
}

void ui_sw_split(uint32_t rtc, int *year, int *mon, int *mday, int *hour,
                 int *min)
{
    long z = (long)(rtc / 86400u) + 719468, era, doe, yoe, doy, mp, d, m, y;

    era = z / 146097;
    doe = z - era * 146097;
    yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    y = yoe + era * 400;
    doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    mp = (5 * doy + 2) / 153;
    d = doy - (153 * mp + 2) / 5 + 1;
    m = mp < 10 ? mp + 3 : mp - 9;
    *year = (int)(y + (m <= 2));
    *mon = (int)m;
    *mday = (int)d;
    *hour = (int)(rtc % 86400u / 3600u);
    *min = (int)(rtc % 3600u / 60u);
}

static uint32_t rtc_now(bool fresh)
{
    long t = current_tick;

    if (fresh || !rc_valid || TIME_AFTER(t, rc_tick + HZ)
        || TIME_BEFORE(t, rc_tick))
    {
        rc_rtc = tm_rtc(get_time());
        rc_tick = t;
        rc_valid = true;
        return rc_rtc;
    }
    return rc_rtc + (uint32_t)(t - rc_tick) / HZ;
}

bool ui_sw_running(void)
{
    return sw.running;
}

bool ui_sw_active(void)
{
    return sw.running || sw.acc_ms || sw.nlaps;
}

const struct sw_state *ui_sw_state(void)
{
    return &sw;
}

uint32_t ui_sw_now_ms(bool fresh)
{
    uint32_t rtc = rtc_now(fresh), e;
    long t = current_tick;

    e = sw_elapsed_ms(&sw, rtc, t);
    if (sw.running)
    {
        int32_t dt = (int32_t)((uint32_t)t - (uint32_t)sw.start_tick);
        int32_t dr = (int32_t)(rtc - sw.start_rtc);

        /* the ticks fell out of step with the RTC (a soft sleep froze
         * them): count on from here, tenths from the ticks again */
        if (dt < 0 || dt / HZ - dr > 1 || dr - dt / HZ > 1)
        {
            sw.acc_ms = e;
            sw.start_rtc = rtc;
            sw.start_tick = (int32_t)t;
        }
    }
    return e;
}

/* -- stopwatch.dat -------------------------------------------------------- */

int ui_sw_logs_load(struct sw_logs_file *f)
{
    int fd, n;

    memset(f, 0, sizeof(*f));
    fd = open(SW_DAT, O_RDONLY);
    if (fd < 0)
    {
        safefile_recover(SW_DAT, SW_TMP, SW_OLD);   /* a cut replace */
        fd = open(SW_DAT, O_RDONLY);
    }
    if (fd < 0)
        return 0;
    n = read(fd, f, sizeof(*f));
    close(fd);
    if (n < 0 || sw_logs_check(f, n) < 0)
    {
        UPDATE_TRACE("stopwatch dat bad n=%d", n);
        memset(f, 0, sizeof(*f));
        return 0;
    }
    return f->count;
}

static bool logs_save(struct sw_logs_file *f)
{
    int fd, bytes = offsetof(struct sw_logs_file, log)
                    + f->count * sizeof(struct sw_log);
    bool ok;

    sw_logs_seal(f, f->count);
    fd = open(SW_TMP, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0)
        return false;
    ok = write(fd, f, bytes) == bytes && fsync(fd) >= 0;
    if (close(fd) < 0)
        ok = false;
    if (ok)
        ok = safefile_replace(SW_TMP, SW_DAT, SW_OLD) == 0;
    if (!ok)
    {
        safefile_remove(SW_TMP, SW_DAT);
        UPDATE_TRACE("stopwatch dat write failed n=%d", (int)f->count);
    }
    return ok;
}

/* The log of the timing as it stands: replaces log[0] when this timing
 * was logged before (a resume), else goes in front */
static void commit(struct sw_logs_file *f, uint32_t now_ms)
{
    struct sw_log rec;

    sw_log_make(&sw, now_ms, &rec);
    rec.start_rtc = first_rtc;
    if (has_log && f->count > 0)
        f->log[0] = rec;
    else
        f->count = sw_log_push(f->log, f->count, &rec);
    has_log = true;
    logs_save(f);
    UPDATE_TRACE("stopwatch log total=%lu laps=%d n=%d",
                 (unsigned long)rec.total_ms, (int)rec.nlaps, (int)f->count);
}

void ui_sw_toggle(struct sw_logs_file *f)
{
    uint32_t rtc = rtc_now(true);

    if (sw.running)
    {
        uint32_t e = ui_sw_now_ms(true);

        sw_stop(&sw, rtc, current_tick);
        sw.acc_ms = e;
        if (f)
            commit(f, e);
        UPDATE_TRACE("stopwatch stop ms=%lu", (unsigned long)e);
    }
    else
    {
        if (!begun)
        {
            first_rtc = rtc;
            begun = true;
        }
        sw_start(&sw, rtc, current_tick);
        UPDATE_TRACE("stopwatch start acc=%lu", (unsigned long)sw.acc_ms);
    }
}

void ui_sw_lap(void)
{
    if (!sw.running)
        return;
    sw_lap(&sw, ui_sw_now_ms(false));
    UPDATE_TRACE("stopwatch lap n=%d", (int)sw.nlaps);
}

void ui_sw_new(struct sw_logs_file *f)
{
    if (sw.running)
        ui_sw_toggle(f);            /* stopped and logged */
    sw_reset(&sw);
    begun = has_log = false;
    UPDATE_TRACE("stopwatch new laps=%d", 0);
}

void ui_sw_logs_clear(struct sw_logs_file *f)
{
    f->count = 0;
    has_log = false;                /* a later stop pushes anew */
    logs_save(f);
    UPDATE_TRACE("stopwatch cleared n=%d", 0);
}

void ui_sw_poweroff(void)
{
    struct sw_logs_file *f;
    int h;

    if (!sw.running)
        return;
    h = core_alloc(sizeof(*f));
    if (h <= 0)
    {
        ui_log("stopwatch", "poweroff: no memory");
        return;
    }
    f = core_get_data(h);
    ui_sw_logs_load(f);
    ui_sw_toggle(f);
    core_free(h);
}

/* The panel (ui_preview_x_stopwatch) is the Extras icon set's stopwatch,
 * drawn with the others in screens/panels.c (one scale, owner 2026-10-24). */
