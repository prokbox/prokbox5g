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

/* K-1, the parked COP (docs/design/core-layout-decision.md section 7 step 1,
 * docs/design/p19-placement.md section 3.4): the CPU's tick rule, kept free
 * of every include so the host test (tests/copk1) runs the same code.
 *
 * Upstream pulses the COP on every tick (kernel-pp.c TIMER1) so that it
 * can check its own timeout list.  Everything else that makes a COP thread
 * runnable already wakes it at once: a post, a semaphore release, a thread
 * created there all go through core_schedule_wakeup(), which calls
 * core_wake(COP) when the waker is the other core (firmware/kernel/
 * thread.c).  So with the switch on, the tick wakes the COP only when its
 * next timeout check is due: the COP's core_entry.next_tmo_check, which
 * only the COP writes (prepare_block() and check_tmo_expired_inner() run
 * on the thread's own core) and which lives in IRAM (uncached, shared).
 * With nothing pending the COP's own check sets it a minute ahead, so a
 * parked COP still wakes once a minute; that is upstream's floor, kept. */

#ifndef COP_PARK_H
#define COP_PARK_H

#include <stdbool.h>

/* The tick at `now` wakes the COP?  `park` is the switch; `next_check` the
 * COP's next due timeout check.  The same comparison as the kernel's own
 * check_tmo_expired() (!TIME_BEFORE(now, next_check)), so a timeout the
 * COP armed fires on the first tick at or after its deadline, as with the
 * pulse */
static inline bool cop_park_tick_wakes(bool park, long now, long next_check)
{
    return !park || (long)((unsigned long)now - (unsigned long)next_check) >= 0;
}

#endif /* COP_PARK_H */
