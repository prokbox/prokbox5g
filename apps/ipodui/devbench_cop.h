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

#ifndef _IPODUI_DEVBENCH_COP_H_
#define _IPODUI_DEVBENCH_COP_H_

/* P19 sitting 4h: the core layout probe (docs/design/core-layout-decision.md
 * section 5; docs/pp5022-reference/interrupts-timers.md "Routing a source
 * to the COP").  The owner chose Apple's layout (A) on one condition: the
 * CPU essentially asleep with the screen off while music plays.  These
 * three bench rows prove or refute what (A) stands on.  Each returns an
 * enum bench_res and logs through devbcm_log() (bcmtest.log and the
 * results file); every exit path puts the interrupt routes, DMA0, the
 * FIQ vector's target, the DAC's standby switch and the playback back.
 *
 *   p2    automatic, silent: Timer 2 routed to the COP at 100 Hz for
 *         10 s.  "p2 cop_t2=N cpu_t2=M expect=1000"; pass: N within
 *         +-2 %, M = 0.  The COP's own deadline source in (A).
 *   p1    audible: playback stopped, PCM DMA source 26 routed to the
 *         COP's FIQ, the COP starts DMA0 on a ~1 kHz tone (64 KB buffer,
 *         8 KB pieces) for 10 s.  "p1 cop_fiq=N cpu_fiq=M expect=E";
 *         pass: N within +-5 % of E (215 at 44.1 kHz), M = 0, and the
 *         owner heard a clean tone.  The gate for (A).
 *   duty  hands-off, about 14 min: music plays, screen off, Hold on;
 *         arms C F C F (decode on the CPU, today's layout; decode on the
 *         COP, codec_cop_set_place's Fit-2 switch), 30 s settle and three
 *         60 s windows each; per window the CPU's wakes by source, both
 *         cores' idle, the clock.  Informational (done).
 *
 * The simulator has no second core: devbench_sim.c answers each row
 * "Not run (sim)" with the route marked [hypothesis], never a pass. */

int devbench_cop_t2(void);
int devbench_cop_fiq(void);
int devbench_cop_duty(void);
/*   heap  hands-off, about 7 min (sitting 4gh-b5's buflib panic): an
 *         album plays, decode on the COP (arm F) then the CPU (arm C);
 *         every 2 s an allocation of a varied size (3 live), the heap
 *         walked (heapchk.h) and the live blocks' bytes checked.  The
 *         verdict: the first bad block and its arm, else PASS */
int devbench_cop_heap(void);

#endif /* _IPODUI_DEVBENCH_COP_H_ */
