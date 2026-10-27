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
 * iPod UI shell: simulator clocks for the trace timings
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
#ifndef _IPODUI_SIMCLOCK_H_
#define _IPODUI_SIMCLOCK_H_

/* Simulator only. The host's clocks in microseconds (differences only):
 * sim_clock_now_us(): monotonic elapsed time; sim_clock_cpu_us(): CPU
 * time of the calling thread (elapsed host time varies a lot with other
 * threads and host scheduling). In a file of their own because the host's
 * <time.h> must not meet the apps headers there (library.c used to
 * declare clock_gettime_nsec_np() by hand, which conflicts with the macOS
 * SDK's own declaration in a UBSan build). */
unsigned long sim_clock_now_us(void);
unsigned long sim_clock_cpu_us(void);
unsigned long sim_clock_proc_cpu_us(void);  /* all threads of the process */
/* the calling thread's USER time only: computing, without the host
 * kernel's time in its system calls (file reads), which is no estimate of
 * the device's (storage is counted apart: cmds=, the storage model).
 * Falls back to sim_clock_cpu_us() where the host has no split */
unsigned long sim_clock_cpu_user_us(void);

#endif /* _IPODUI_SIMCLOCK_H_ */
