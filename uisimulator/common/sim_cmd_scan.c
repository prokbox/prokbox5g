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
 * Host microsecond clock for the 16-0 scan trace (apps/tagcache.c under
 * HAVE_IPOD_UI_DEVMODE): the sim has no USEC_TIMER. Test-only.
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
#include <stdlib.h>
#include <time.h>

unsigned long sim_scan_us(void);
/* "scan_slow <ticks>" (sim_script.c): the scan sleeps this long at each
 * directory it walks, so a test library's update lasts long enough to
 * see (the header's busy glyph, 3.20); 0: off */
int sim_scan_slow_ticks;
/* p081: "# env: SIM_SCAN_SLOW_MS=n" spreads n ms of sleep over a
 * process's first walked folders (at most 200 ms each), so a boot's own
 * walk (before the script runs) lasts long enough whatever the library's
 * size. Returns the ticks the walk sleeps at this folder. */
static long scan_slow_left = -1;
int sim_scan_slow_now(void);
int sim_scan_slow_now(void)
{
    long ms;

    if (sim_scan_slow_ticks > 0)
        return sim_scan_slow_ticks;
    if (scan_slow_left < 0)
    {
        const char *v = getenv("SIM_SCAN_SLOW_MS");
        scan_slow_left = v ? atol(v) : 0;
    }
    ms = scan_slow_left < 200 ? scan_slow_left : 200;
    scan_slow_left -= ms;
    return (int)(ms / 10);                  /* HZ 100: ticks */
}
/* #276: "scan_begin_delay <ms> [n]" (sim_script.c): each of the next n
 * (default 1) owed scans (an update's read-only probe, then the scan it
 * hands over to) begins that long after the tagcache thread first finds
 * it owed (the device's handover after an unplug: "usb hand last
 * tagcache@180", and a busy CPU later still), so the moments between
 * "owed" and "begun" can be seen; 0: off */
long sim_scan_begin_delay_ms;
int sim_scan_begin_delay_n;
int sim_scan_begin_held(long now_ms);
int sim_scan_begin_held(long now_ms)
{
    static long first = -1;

    if (sim_scan_begin_delay_ms <= 0 || sim_scan_begin_delay_n <= 0)
        return 0;
    if (first < 0)
        first = now_ms;
    if (now_ms - first < sim_scan_begin_delay_ms)
        return 1;
    first = -1;
    if (--sim_scan_begin_delay_n <= 0)
        sim_scan_begin_delay_ms = 0;
    return 0;
}

unsigned long sim_scan_us(void)
{
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return (unsigned long)t.tv_sec * 1000000UL + t.tv_nsec / 1000;
}

/* #245: the calling thread's own CPU time (the commit trace's stages,
 * apps/tagcache.c COMMIT_TRACE): other threads' turns are not counted */
unsigned long sim_thread_cpu_us(void)
{
    struct timespec t;

    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t);
    return (unsigned long)t.tv_sec * 1000000UL + t.tv_nsec / 1000;
}
