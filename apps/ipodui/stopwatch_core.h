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
 * iPod UI shell: Stopwatch, pure half (P12 contract, docs/phase12-brief.md
 * 3.4) Host-tested in tests/stopwatch; owner W1-S
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
#ifndef IPODUI_STOPWATCH_CORE_H
#define IPODUI_STOPWATCH_CORE_H

/* Time = RTC seconds + a tick fraction, so a running stopwatch survives
 * soft sleep (current_tick freezes in P18's sleep) and power-off. Shown
 * in tenths (the 7G's resolution), redrawn at 10 fps while lit. The
 * resident state (W1-S's stopwatch.c) is one struct sw_state. */

#include <stdint.h>
#include <stdbool.h>

#define SW_LAPS_MAX  32
#define SW_LOGS_MAX  20                 /* oldest dropped */
#define SW_TICK_MS   10                 /* HZ = 100 on this target */

struct sw_state {
    uint32_t start_rtc;                 /* RTC s at the last start/resume */
    int32_t  start_tick;                /* current_tick at the same moment
                                           (int32_t, not long: one layout
                                           on the target and the host) */
    uint32_t acc_ms;                    /* time before the last start */
    uint8_t  running, nlaps;
    uint32_t lap_ms[SW_LAPS_MAX];       /* each lap's length; the running
                                           lap is elapsed - sum(lap_ms) */
};
/* 144 B (the brief estimated 152): resident, the stopwatch runs while
 * its screen is closed */
_Static_assert(sizeof(struct sw_state) == 144, "struct sw_state layout");

/* Elapsed ms. While running: acc_ms + the time since the start, whose
 * whole seconds come from the RTC (now_rtc - start_rtc) and whose
 * sub-second part comes from the ticks only while the tick count agrees
 * with the RTC within 1 s ("tick part only within 1 s"): after a sleep
 * froze the tick, the RTC alone counts. Stopped: acc_ms. */
uint32_t sw_elapsed_ms(const struct sw_state *s, uint32_t now_rtc,
                       long now_tick);
/* Records a lap ending at now_ms (an sw_elapsed_ms() value). At
 * SW_LAPS_MAX laps the last slot keeps accumulating (no lap is lost from
 * the total). */
void sw_lap(struct sw_state *s, uint32_t now_ms);
/* Start or resume / stop (pause), and New Timer (all zero, stopped) */
void sw_start(struct sw_state *s, uint32_t now_rtc, long now_tick);
void sw_stop(struct sw_state *s, uint32_t now_rtc, long now_tick);
void sw_reset(struct sw_state *s);

/* stopwatch.dat: SW_LOGS_MAX records, newest first, written at Stop and
 * at New Timer, never per lap (safefile). */
#define SW_LAST_LAPS 3
struct sw_log {
    uint32_t start_rtc;                 /* the date and time listed */
    uint32_t total_ms;
    uint16_t nlaps;
    uint16_t reserved;
    uint32_t shortest, longest, average;    /* ms, over the laps */
    uint32_t last[SW_LAST_LAPS];        /* the last 3 laps, newest first */
};
_Static_assert(sizeof(struct sw_log) == 36, "struct sw_log layout");
#define SW_LOGS_MAGIC   0x57535050u     /* "PPSW" */
#define SW_LOGS_VERSION 1
struct sw_logs_file {
    uint32_t magic; uint16_t version, count;
    uint32_t crc;                       /* crc_32(log, count * 36, ~0) */
    struct sw_log log[SW_LOGS_MAX];
};

/* The log record of a timing (the running lap counts as a lap) */
void sw_log_make(const struct sw_state *s, uint32_t now_ms,
                 struct sw_log *out);
/* Pushes rec at the front of logs[0..n), dropping the oldest at
 * SW_LOGS_MAX; returns the new count */
int  sw_log_push(struct sw_log *logs, int n, const struct sw_log *rec);
/* stopwatch.dat seal/check, as alarms_file_seal/check: 0 or <0 */
void sw_logs_seal(struct sw_logs_file *f, int count);
int  sw_logs_check(const struct sw_logs_file *f, uint32_t bytes);

#endif /* IPODUI_STOPWATCH_CORE_H */
