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


#ifndef SIM_BCM_JOB_H
#define SIM_BCM_JOB_H

#include <stdbool.h>
#include <stdint.h>

/* P19 W0-5: the simulator's BCM2722 job model (sim_bcm_job.c wraps
 * bcmfw/jobs/jmodel.c under HAVE_BCM_JOBS_MODEL), as the script commands
 * (sim_cmd_bcm.c) see it.  The bcmjob.h / bcmjob_pp.h API itself is the
 * shell's. */

/* the fault kinds of jmodel_fault() (enum jmodel_fault's values) */
#define SIM_BCM_FAULT_CLEAR     0   /* the image lacks op */
#define SIM_BCM_FAULT_SLOW      1   /* op takes 40 x its deadline */
#define SIM_BCM_FAULT_FAIL      2   /* op fails */
#define SIM_BCM_FAULT_DEAD      3   /* the heartbeat stops */
#define SIM_BCM_FAULT_CUT       4   /* the BCM loses power now */
#define SIM_BCM_FAULT_RINGFULL  5   /* the next 'op' submits answer BUSY */
#define SIM_BCM_FAULT_NONE      6   /* clear every fault */

struct sim_bcm_stats
{
    uint32_t state_ms[3];   /* ms in C0 (cut), C1 (starting), C2 (up) */
    uint32_t cold_starts;
    uint32_t jobs_done;
    uint32_t epoch;
    uint32_t dark_ups[4];   /* by reason: any, decode, game, video */
    int anim_tag;           /* a BCM-timed animation's tag, or -1 */
};

/* false: the model is not running (bcmjob_init failed or not yet called) */
bool sim_bcm_ready(void);
/* the model's clock caught up with the sim's first */
bool sim_bcm_stats(struct sim_bcm_stats *out);
bool sim_bcm_fault(int kind, uint32_t op);
bool sim_bcm_charger(bool on);
/* model time moves on by ms (a scripted jump, on top of the sim's clock) */
bool sim_bcm_advance(uint32_t ms);

/* P19 19-6: the model has DECODE (jdec; offered as the overlay below
 * says) or not (the default: no decoder, NOJOB); pace N: a decode takes
 * real time, N ticks for each of its modelled ms (0: none) */
bool sim_bcm_decode(bool on, unsigned pace);
/* The merged image's DECODE overlay (album-render.md 5.3): ON (the
 * default, the device's merged image) offers DECODE only once the shell
 * has loaded dec-ovl.bin in this epoch, each decode lending a J_CLS_DRIFT
 * work slot; OFF: DECODE resident (the display-only image's way); REFUSE:
 * the load refused (why=nofile) */
#define SIM_BCM_OVL_OFF     0
#define SIM_BCM_OVL_ON      1
#define SIM_BCM_OVL_REFUSE  2
bool sim_bcm_dec_ovl(int mode);
/* The visualizer's overlay (visualizer.md 8.5), the same modes: ON (the
 * default, the device's images) offers SPECTRUM, BARS and the line ops
 * only once the shell has loaded viz-ovl-m.bin (or viz-ovl.bin) in this
 * epoch; OFF: the model's ops resident (the BCMFW_VIZ image's way);
 * REFUSE: the load refused (why=nofile).  The merged image's DECODE and
 * the visualizer share one region: a load evicts the other */
bool sim_bcm_viz_ovl(int mode);
/* J_VERSION's minor the model reports (bcmjob_minor()): an older image */
void sim_bcm_minor(uint32_t minor);
/* BARS honours J_BARS_FLIP (bcmjob_bars_flip(); default on): off stands
 * in an overlay before the flag, so the Mirror takes the CPU fallback */
void sim_bcm_bars_flip(bool on);

/* a shot shows the glass: the BCM-owned rect from the model's panel
 * (on before screen_dump, off after) */
void sim_bcm_glass(bool on);

#endif
