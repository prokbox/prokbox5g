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
 * iPod UI shell: simulator clocks for the trace timings (simclock.h)
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
/* Host headers only: no config.h, no apps or firmware headers. */
#include <time.h>
#if defined(__APPLE__)
#include <mach/mach.h>
#elif defined(__linux__)
#include <sys/resource.h>
#endif
#include "simclock.h"

static unsigned long clock_us(clockid_t id)
{
    struct timespec ts;

    if (clock_gettime(id, &ts) != 0)
        return 0;
    return (unsigned long)ts.tv_sec * 1000000ul
         + (unsigned long)ts.tv_nsec / 1000ul;
}

unsigned long sim_clock_now_us(void)
{
    return clock_us(CLOCK_MONOTONIC);
}

unsigned long sim_clock_proc_cpu_us(void)
{
#ifdef CLOCK_PROCESS_CPUTIME_ID
    return clock_us(CLOCK_PROCESS_CPUTIME_ID);
#else
    return sim_clock_now_us();
#endif
}

unsigned long sim_clock_cpu_us(void)
{
#ifdef CLOCK_THREAD_CPUTIME_ID
    return clock_us(CLOCK_THREAD_CPUTIME_ID);
#else
    return sim_clock_now_us();
#endif
}

unsigned long sim_clock_cpu_user_us(void)
{
#if defined(__APPLE__)
    thread_basic_info_data_t info;
    mach_msg_type_number_t cnt = THREAD_BASIC_INFO_COUNT;
    mach_port_t self = mach_thread_self();
    kern_return_t kr = thread_info(self, THREAD_BASIC_INFO,
                                   (thread_info_t)&info, &cnt);

    mach_port_deallocate(mach_task_self(), self);
    if (kr != KERN_SUCCESS)
        return sim_clock_cpu_us();
    return (unsigned long)info.user_time.seconds * 1000000ul
         + (unsigned long)info.user_time.microseconds;
#elif defined(__linux__) && defined(RUSAGE_THREAD)
    struct rusage ru;

    if (getrusage(RUSAGE_THREAD, &ru) != 0)
        return sim_clock_cpu_us();
    return (unsigned long)ru.ru_utime.tv_sec * 1000000ul
         + (unsigned long)ru.ru_utime.tv_usec;
#else
    return sim_clock_cpu_us();
#endif
}
