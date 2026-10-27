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
 * Host test of the Stopwatch rules, apps/ipodui/stopwatch_core.c (P12, brief
 * 3.4) W0 skeleton: the cases are the contract; W1-S adds more
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
#include <stdio.h>
#include <string.h>
#include "ipodui/stopwatch_core.h"

static int fails;
#define CHECK(c) do { if (!(c)) { fails++; \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
#define EQ(a, b) do { unsigned long _a = (a), _b = (b); if (_a != _b) { \
    fails++; fprintf(stderr, "FAIL %s:%d: %s = %lu, want %lu\n", \
    __FILE__, __LINE__, #a, _a, _b); } } while (0)

/* The firmware's crc_32 (firmware/common/crc32.c), bitwise */
uint32_t crc_32(const void *src, uint32_t len, uint32_t crc);
uint32_t crc_32(const void *src, uint32_t len, uint32_t crc)
{
    const unsigned char *p = src;
    while (len--)
    {
        int b;
        crc ^= (uint32_t)*p++ << 24;
        for (b = 0; b < 8; b++)
            crc = crc & 0x80000000u ? (crc << 1) ^ 0x04c11db7u : crc << 1;
    }
    return crc;
}

static void elapsed(void)
{
    struct sw_state s;

    sw_reset(&s);
    CHECK(!s.running && s.nlaps == 0 && s.acc_ms == 0);
    EQ(sw_elapsed_ms(&s, 1000, 0), 0);
    sw_start(&s, 1000, 0);
    EQ(sw_elapsed_ms(&s, 1000, 50), 500);           /* tenths from ticks */
    EQ(sw_elapsed_ms(&s, 1001, 150), 1500);
    /* a soft sleep: the RTC moves 3,600 s, the tick stays frozen */
    EQ(sw_elapsed_ms(&s, 1000 + 3600, 150), 3600000);
    /* stop, then resume later: only running time counts */
    sw_stop(&s, 1002, 200);
    EQ(s.acc_ms, 2000);
    CHECK(!s.running);
    EQ(sw_elapsed_ms(&s, 5000, 9999), 2000);
    sw_start(&s, 5000, 400);
    EQ(sw_elapsed_ms(&s, 5001, 500), 3000);
}

static void laps(void)
{
    struct sw_state s;
    uint32_t sum = 0;
    int i;

    sw_reset(&s);
    sw_start(&s, 0, 0);
    sw_lap(&s, 1000);
    sw_lap(&s, 2500);
    CHECK(s.nlaps == 2 && s.lap_ms[0] == 1000 && s.lap_ms[1] == 1500);
    /* past SW_LAPS_MAX the last slot accumulates: no time is lost */
    for (i = 3; i <= SW_LAPS_MAX + 5; i++)
        sw_lap(&s, 2500 + (i - 2) * 100u);
    EQ(s.nlaps, SW_LAPS_MAX);
    for (i = 0; i < SW_LAPS_MAX; i++)
        sum += s.lap_ms[i];
    EQ(sum, 2500 + (SW_LAPS_MAX + 3) * 100u);
}

static void logs(void)
{
    static struct sw_logs_file f, g;
    struct sw_state s;
    struct sw_log r;
    int i, n = 0;

    sw_reset(&s);
    sw_start(&s, 846054000u, 0);
    sw_lap(&s, 1000);
    sw_lap(&s, 2500);
    sw_log_make(&s, 3000, &r);          /* the running lap (500) counts */
    CHECK(r.start_rtc == 846054000u && r.total_ms == 3000 && r.nlaps == 3);
    CHECK(r.shortest == 500 && r.longest == 1500 && r.average == 1000);
    CHECK(r.last[0] == 500 && r.last[1] == 1500 && r.last[2] == 1000);

    /* the ring: newest first, the oldest dropped at SW_LOGS_MAX */
    memset(&f, 0, sizeof(f));
    for (i = 0; i < SW_LOGS_MAX + 1; i++)
    {
        r.start_rtc = 1000u + i;
        n = sw_log_push(f.log, n, &r);
    }
    EQ(n, SW_LOGS_MAX);
    EQ(f.log[0].start_rtc, 1000u + SW_LOGS_MAX);
    EQ(f.log[SW_LOGS_MAX - 1].start_rtc, 1001u);

    /* stopwatch.dat round trip */
    sw_logs_seal(&f, n);
    CHECK(f.magic == SW_LOGS_MAGIC && f.version == SW_LOGS_VERSION
          && f.count == n);
    EQ(sw_logs_check(&f, sizeof(f)), 0);
    g = f; g.log[3].total_ms ^= 1;
    CHECK(sw_logs_check(&g, sizeof(g)) < 0);
    g = f; g.magic ^= 1;
    CHECK(sw_logs_check(&g, sizeof(g)) < 0);
    CHECK(sw_logs_check(&f, 11) < 0);
}

int main(void)
{
    elapsed();
    laps();
    logs();
    if (fails)
        return 1;
    printf("ok   stopwatch rules (RTC + tick time across a sleep, stop/"
           "resume, laps, log record, log ring, stopwatch.dat)\n");
    return 0;
}
