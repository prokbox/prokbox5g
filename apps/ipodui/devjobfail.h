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

#ifndef _IPODUI_DEVJOBFAIL_H_
#define _IPODUI_DEVJOBFAIL_H_

#include "config.h"
#include <stdbool.h>

/* Developer builds only (HAVE_IPOD_UI_DEVMODE): make the next N BCM jobs
 * of one kind fail, as if the chip refused them, to prove the loud CPU
 * fallback ("bcmjob fault ... why=failed", then "CPU FALLBACK") and the
 * return to the chip, without a broken chip.  The driver's hook does the
 * failing (bcmjob_dev_inject_fail: the device's real reply path, the
 * simulator's model); the switch expires by itself after its N jobs.
 * Developer > Advanced > Video chip > Fault injection holds the rows;
 * the simulator's script command is "bcm_jobfail viz|decode N". */
#include "system.h"
#include "kernel.h"                /* bcmjob_pp.h wants SYS_EVENT */
#include "bcmjob_pp.h"

#ifdef HAVE_BCMJOB_INJECT
enum devjf_kind
{
    DEVJF_VIZ,          /* the visualiser's frame ops */
    DEVJF_DECODE,       /* DECODE */
};

/* Arm the next n jobs of kind (n = 0 disarms); 0, or -1 (no driver, or
 * not the owner's thread) */
int devjf_arm(int kind, unsigned n);
/* The rows: each press steps 1 -> 2 -> off (one failure is retried and
 * the chip recovers; BCMJOB_FAIL_MARK, 2, marks the op failed for the
 * boot: the CPU path stays) */
bool devjf_row_viz(void);
bool devjf_row_decode(void);
#define DEVJF_ROWS \
        { "BCM jobs: fail visualiser frames (1/2/off)", devjf_row_viz }, \
        { "BCM jobs: fail decodes (1/2/off)", devjf_row_decode },
#else
#define DEVJF_ROWS
#endif

#endif
