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

/* The bench session in the simulator (tests/ui/ipodui-p19-bench-*.script).
 *
 * devbench.c's runner, step table, results file, resume and end summary
 * are the device's own code, compiled in the shell simulator with the
 * video player (sim-ui-video: --ipodui --bcmfw-video).  Only the steps'
 * device functions (the BCM rows, the stage plays, the probe image's port
 * reads, the restart-making rows) are replaced, by devbench_sim.c's stubs:
 * devbench_sim_calls.h maps each call in devbench.c to its stub, so no
 * stub carries a device function's name in the simulator binary.
 *
 * The stubs return modelled results and write the log lines the device's
 * rows write (their verdict words, through the bench hook), and use the
 * real resources where the simulator has them: the job model's self-test
 * (bcmjob_selftest), the probe image's pinned block (core_alloc of the
 * image's size, the file read with bcmres_read when it is on the disk),
 * the UI thread switch (fe_be_mode_set_next).
 *
 * The model's state lives in marker files on the sim disk, so it survives
 * the "reboot" parts of a script (DBS_* below; a script sets them with
 * the bench_sim command, uisimulator/common/sim_cmd_bench.c):
 *   - the stage pass flags (default: there),
 *   - the display driver: the boot step's restart brings it (the first
 *     start has none, as sitting 3's: pre is done, boot runs),
 *   - the fault switches: a results-file open that fails once (the lost
 *     line of sitting 4c), and the probe image's block refused while music
 *     plays (sitting 4c's "no probe image" after the visualiser). */
#ifndef _IPODUI_DEVBENCH_SIM_H_
#define _IPODUI_DEVBENCH_SIM_H_

#include "config.h"

#if defined(SIMULATOR) && defined(HAVE_BCMFW_VIDEO) && \
    defined(HAVE_IPOD_UI) && defined(HAVE_IPOD_UI_DEVMODE)
#define DEVBENCH_SIM

#include <stdbool.h>

/* the model's marker files (sim disk; hidden, never named on screen) */
#define DBS_DIR             ROCKBOX_DIR "/.sim_bench"
#define DBS_NOFLAGS         DBS_DIR "/noflags"      /* no stage 4/5 flags */
#define DBS_DRIVER          DBS_DIR "/driver"       /* this start has it */
#define DBS_DRIVER_NEXT     DBS_DIR "/driver_next"  /* the next start will */
#define DBS_COP_NEXT        DBS_DIR "/cop_next"     /* ucop asked for COP */
#define DBS_COP             DBS_DIR "/cop"          /* this start's UI: COP */
#define DBS_UI_MARK         DBS_DIR "/ui_split"     /* the developer switch
                                                       as the device keeps
                                                       it (fe_be.h
                                                       FE_MODE_MARKER):
                                                       "cop"/"cpu"; none:
                                                       the build default */
#define DBS_PUTFAIL         DBS_DIR "/putfail"      /* <id>: that step's
                                                       result line's open
                                                       fails, once */
#define DBS_PUTFAILS        DBS_DIR "/putfails"     /* <k>: the next k
                                                       results-file opens
                                                       all fail */
#define DBS_LOWMEM          DBS_DIR "/lowmem"       /* the probe image's
                                                       block refused while
                                                       music plays */
#define DBS_FAIL            DBS_DIR "/fail_"        /* + <id>: the row fails */

/* The bench_sim script command (sim_cmd_bench.c, on the script thread):
 * true if handled */
bool devbench_sim_cmd(char *argv[], int argc, int lineno);

/* devbench.c's entry points (devbcm.h declares them on the device, under
 * HAVE_BCMFW, which the simulator lacks) */
bool devbench_session(void);
int devbench_ask(const char *title, const char *text,
                 const char *const *opts, int n);

#endif
#endif /* _IPODUI_DEVBENCH_SIM_H_ */
