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
 * iPod UI shell: performance-trace hooks
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
#ifndef _IPODUI_PTRACE_H_
#define _IPODUI_PTRACE_H_

/* Performance-trace hooks (Phase 8b-0 tooling; rules in
 * scripts/check_trace.py, docs in tests/ui/README.md "perf traces").
 *
 * Compiled in only for the simulator, or a firmware built with
 * -DUI_PTRACE. Otherwise every macro is an empty statement: no code, no
 * data, no timer read (the stock and release firmware are unchanged).
 *
 *   PT_VAR(t)            unsigned long t = <cpu microseconds now>;
 *   PT_LAP(t)            t = <cpu microseconds now> - t;  (t becomes the
 *                        elapsed time of the span opened by PT_VAR)
 *   PT_GAP(g)            g = <elapsed microseconds since the last PT_GAP
 *                        on this g, 0 the first time> (g is a static unsigned long)
 *   PT_TRACE(fmt, ...)   UPDATE_TRACE, only here
 * Simulator microseconds are host CPU time: relative cost only; the
 * device figure needs a UI_PTRACE firmware (target USEC_TIMER). */
#if defined(SIMULATOR)
#include "ipodui/simclock.h"
#define PT_NOW_CPU()    sim_clock_cpu_us()
#define PT_NOW_WALL()   sim_clock_now_us()
#elif defined(UI_PTRACE) && defined(USEC_TIMER)
#define PT_NOW_CPU()    ((unsigned long)USEC_TIMER)
#define PT_NOW_WALL()   ((unsigned long)USEC_TIMER)
#endif

#ifdef PT_NOW_CPU
#define PT_VAR(t)       unsigned long t = PT_NOW_CPU()
#define PT_LAP(t)       ((t) = PT_NOW_CPU() - (t))
#define PT_GAP(g)       do { unsigned long n_ = PT_NOW_WALL(); \
                             (g) = (g##_prev) ? n_ - (g##_prev) : 0; \
                             (g##_prev) = n_ ? n_ : 1; \
                           } while (0)
#define PT_GAPVARS(g)   static unsigned long g, g##_prev
#define PT_TRACE(...)   UPDATE_TRACE(__VA_ARGS__)
#else
#define PT_VAR(t)       ((void)0)
#define PT_LAP(t)       ((void)0)
#define PT_GAP(g)       ((void)0)
#define PT_GAPVARS(g)   ((void)0)
#define PT_TRACE(...)   ((void)0)
#endif

#endif /* _IPODUI_PTRACE_H_ */
