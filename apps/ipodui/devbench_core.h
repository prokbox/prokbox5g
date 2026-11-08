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

/* Bench rows "Core layout A" (docs/design/core-layout-decision.md section
 * 7, steps 1 and 2; firmware/export/corelayout.h).  Each returns an enum
 * bench_res and logs through devbcm_log(); every exit path puts the
 * default back (the COP pulsed by the tick, the PCM's FIQ on the CPU) and
 * the playback as it was.
 *
 *   k1    hands-off, about 7 min: music plays, screen off, Hold on.  Two
 *         arms of 30 s settle + 150 s, the pulse (today) then K-1 (the
 *         COP parked).  "k1 arm=pulse|park cop=W/s cpu=W/s ticks_sent=N
 *         held=M underruns=U rescues=R".  Pass: in the park arm the tick
 *         sends at most 2 pulses a second (due timeouts only), the COP's
 *         wakes fall below the pulse arm's, and the audio stays clean
 *         (no underrun, no rescue, still playing).
 *   pcop  audible, about 75 s: needs p1 (the PCM's FIQ on the COP, the
 *         raw form).  Playback stopped; pcm-pp.c's own driver with source
 *         26 on the COP (pcm_pp_set_core) plays a ~1 kHz tone for 60 s
 *         through pcm_play_data(), started and locked from the CPU; every
 *         10 ms the CPU takes the PCM lock for 200 us and checks that no
 *         FIQ ran on the COP inside it.  "pcop fiq_cop=N fiq_cpu=0
 *         more=M expect=E late=L in_lock=0 waits=W stuck=0".  Pass: M
 *         within +-5 % of E, no FIQ on the CPU or inside a lock, no late
 *         refill, no stuck wait, and the owner heard a clean tone.
 *         Real music stays on the CPU's FIQ: the refill chain (the mixer's
 *         and pcmbuf's indices) is the CPU's cached memory until step 5
 *         gives the engine its own lines.
 *
 * Wiring (devbench.c's steps[] and a picks table; the BCM session owns
 * them), with devbench.c's BA/BC flags:
 *   { "k1", "Second core parked", ..., devbench_core_k1, NULL, { NULL },
 *     BA | BC | BENCH_F_PICKED, 480, 420 },
 *   { "pcop", "Driver on the second core", ..., devbench_core_pcop,
 *     "<clean tone question>", { "Yes", "Clicks or gaps", "Silent",
 *     "Wrong pitch" }, BC | BENCH_F_PICKED, 0, 67 },
 * picks: { "k1", BENCH_R_NEED, { NULL } },
 *        { "pcop", BENCH_R_NEED, { "p1" } }.
 *
 * Not on this build (a release, the simulator): each row answers "Not
 * run" with a skip; the simulator has no second core, so nothing here is
 * ever a pass there. */

#ifdef DEVBENCH_CORE_ROWS
#undef DEVBENCH_CORE_ROWS
/* The rows, as table initializers (sitting 4gh: the one hook in
 * devbench.c's steps[], as devbench_awake.h and devbench_pwr.h):
 *     #define DEVBENCH_CORE_ROWS
 *     #include "devbench_core.h"
 * In the re-verify run (devbench.h bench_reverify_picks) both are
 * readings (ALSO): pcop right after p1 (it needs p1), k1 its own dark
 * soak after awlg, before duty (duty moves the decode's core, k1 the
 * COP's tick; a shared soak would mix them). */
#ifdef HAVE_IPOD_UI_DEVMODE
    /* ---- Core layout A (devbench_core.h; picked runs only) ---- */
    { "pcop", "Driver on the second core",
      "Headphones on, volume low. The music stops and a steady beep "
      "plays for about 1 min, through the normal sound driver run by the "
      "second core. Normal: one clean, steady beep.",
      "Leave the iPod alone while it plays; the question follows. The "
      "music comes back after.",
      devbench_core_pcop,
      "Did the tone sound clean?\n"
      "Yes: one steady beep, no clicks.\n"
      "Clicks or gaps: it crackled or broke up.\n"
      "Silent: nothing played.\n"
      "Wrong pitch: far higher or lower than a beep.",
      { "Yes", "Clicks or gaps", "Silent", "Wrong pitch" },
      BENCH_F_COP | BENCH_F_PICKED, 0, 67, 0 },
    { "k1", "Second core parked",
      "Hands off, about 7 min, in the dark. The album you played last "
      "plays. The screen goes dark: switch Hold on and leave the iPod "
      "alone until the screen lights again.",
      "Nothing to watch. At the end, switch Hold off; the next screen "
      "follows.",
      devbench_core_k1, NULL, { NULL },
      BENCH_F_AUTO | BENCH_F_COP | BENCH_F_PICKED, 480, 420, 0 },
#endif
#else /* the header proper */

#ifndef _IPODUI_DEVBENCH_CORE_H_
#define _IPODUI_DEVBENCH_CORE_H_

int devbench_core_k1(void);
int devbench_core_pcop(void);

#endif /* _IPODUI_DEVBENCH_CORE_H_ */
#endif /* DEVBENCH_CORE_ROWS */
