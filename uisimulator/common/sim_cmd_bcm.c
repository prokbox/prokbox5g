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


#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "config.h"
#include "sim_cmd_bcm.h"

#ifdef HAVE_BCM_JOBS_MODEL
#include "bcmjob_pp.h"
#include "bcmjob.h"
#include "sim_bcm_job.h"
#include "ipodui/art_bcm.h"
#ifdef HAVE_BCMJOB_INJECT
#include "ipodui/devjobfail.h"
#endif

static unsigned long num(const char *s)
{
    return strtoul(s, NULL, 0);     /* 0xNN or decimal */
}

static void fault_cmd(char *argv[], int argc, int lineno)
{
    static const struct { const char *name; int kind; int args; } k[] = {
        { "clear", SIM_BCM_FAULT_CLEAR, 1 }, { "slow", SIM_BCM_FAULT_SLOW, 1 },
        { "fail", SIM_BCM_FAULT_FAIL, 1 }, { "dead", SIM_BCM_FAULT_DEAD, 0 },
        { "cut", SIM_BCM_FAULT_CUT, 0 }, { "none", SIM_BCM_FAULT_NONE, 0 },
        { "ringfull", SIM_BCM_FAULT_RINGFULL, 1 },
    };
    unsigned i;

    for (i = 0; argc >= 2 && i < sizeof(k) / sizeof(k[0]); i++)
        if (!strcmp(argv[1], k[i].name))
        {
            uint32_t op = 0;

            if (k[i].args)
            {
                if (argc < 3)
                    break;
                /* ringfull: the count is the last argument */
                op = (uint32_t)num(argv[argc - 1]);
            }
            if (!sim_bcm_fault(k[i].kind, op))
                fprintf(stderr, "sim_script:%d: bcm_fault: the model is "
                        "not running\n", lineno);
            return;
        }
    fprintf(stderr, "sim_script:%d: bcm_fault clear|slow|fail <op> | "
            "ringfull [<op>] <n> | dead | cut | none\n", lineno);
}
#endif

bool sim_cmd_bcm(const char *cmd, char *argv[], int argc, int lineno)
{
    if (strcmp(cmd, "bcm_fault") && strcmp(cmd, "bcm_charger")
        && strcmp(cmd, "bcm_selftest") && strcmp(cmd, "bcm_stats")
        && strcmp(cmd, "bcm_advance") && strcmp(cmd, "expect_bcm_dark_ups")
        && strcmp(cmd, "bcm_drift") && strcmp(cmd, "bcm_decode")
        && strcmp(cmd, "bcm_minor") && strcmp(cmd, "bcm_flip")
        && strcmp(cmd, "bcm_viz") && strcmp(cmd, "bcm_suspend")
        && strcmp(cmd, "bcm_jobfail"))
        return false;
#ifndef HAVE_BCM_JOBS_MODEL
    (void)argv; (void)argc;
    fprintf(stderr, "sim_script:%d: %s unavailable\n", lineno, cmd);
    return true;
#else
    struct sim_bcm_stats st;

    if (!strcmp(cmd, "bcm_jobfail"))
    {
        /* the developer rows' hook (Video chip > Fault injection): the
         * next n jobs of a kind fail, then the chip runs them again */
#ifdef HAVE_BCMJOB_INJECT
        int kind = argc >= 2 && !strcmp(argv[1], "viz") ? DEVJF_VIZ :
                   argc >= 2 && !strcmp(argv[1], "decode") ? DEVJF_DECODE : -1;

        if (kind < 0 || argc < 3)
            fprintf(stderr, "sim_script:%d: bcm_jobfail viz|decode <n>\n",
                    lineno);
        else if (devjf_arm(kind, (unsigned)num(argv[2])) < 0)
            fprintf(stderr, "sim_script:%d: bcm_jobfail: the model is not "
                    "running\n", lineno);
#else
        fprintf(stderr, "sim_script:%d: bcm_jobfail unavailable\n", lineno);
#endif
    }
    else if (!strcmp(cmd, "bcm_drift"))
    {
        /* P19 19-2: the drift's backend (the developer row's A/B) */
        static const char *const m[BCMJOB_DRIFT_MODES] = {
            "auto", "on", "off", "still", "parity"
        };
        int i;

        for (i = 0; i < BCMJOB_DRIFT_MODES; i++)
            if (argc >= 2 && !strcmp(argv[1], m[i]))
                break;
        if (i == BCMJOB_DRIFT_MODES)
            fprintf(stderr, "sim_script:%d: bcm_drift auto|on|off|still|parity\n",
                    lineno);
        else
        {
            bcmjob_drift_mode_set((enum bcmjob_drift_mode)i);
            fprintf(stderr, "sim_script: bcm_drift %s\n", m[i]);
        }
    }
    else if (!strcmp(cmd, "bcm_decode") && argc >= 2 && !strcmp(argv[1], "ovl"))
    {
        /* bcm_decode ovl on|off|refuse: the merged image's overlay gate
         * (sim_bcm_job.h; on by default, as the device) */
        static const char *const om[] = { "off", "on", "refuse" };
        int i;

        for (i = 0; i < 3; i++)
            if (argc >= 3 && !strcmp(argv[2], om[i]))
                break;
        if (i == 3 || !sim_bcm_dec_ovl(i))
            fprintf(stderr, "sim_script:%d: bcm_decode ovl on|off|refuse\n",
                    lineno);
        else
            fprintf(stderr, "sim_script: bcm_decode ovl %s\n", om[i]);
    }
    else if (!strcmp(cmd, "bcm_viz"))
    {
        /* bcm_viz ovl on|off|refuse: the visualizer's overlay gate
         * (sim_bcm_job.h; on by default, as the device) */
        static const char *const om[] = { "off", "on", "refuse" };
        int i;

        for (i = 0; i < 3; i++)
            if (argc >= 3 && !strcmp(argv[1], "ovl") && !strcmp(argv[2], om[i]))
                break;
        if (i == 3 || !sim_bcm_viz_ovl(i))
            fprintf(stderr, "sim_script:%d: bcm_viz ovl on|off|refuse\n",
                    lineno);
        else
            fprintf(stderr, "sim_script: bcm_viz ovl %s\n", om[i]);
    }
    else if (!strcmp(cmd, "bcm_decode"))
    {
        /* P19 19-6: bcm_decode on|panel|off [pace N]; "panel" is on and
         * np-sharp's panel too (art_bcm_panel_set(); off by default in
         * the simulator, a release build's default) */
        bool panel = argc >= 2 && !strcmp(argv[1], "panel");
        bool on = panel || (argc >= 2 && !strcmp(argv[1], "on"));
        unsigned pace = argc >= 4 && !strcmp(argv[2], "pace")
                        ? (unsigned)num(argv[3]) : 0;

        art_bcm_panel_set(panel);
        if (argc < 2 || (!on && strcmp(argv[1], "off"))
            || !sim_bcm_decode(on, pace))
            fprintf(stderr, "sim_script:%d: bcm_decode on|panel|off [pace N] "
                    "(model running? a video build?)\n", lineno);
        else
            fprintf(stderr, "sim_script: bcm_decode %s panel=%d pace=%u\n",
                    on ? "on" : "off", (int)art_bcm_panel_on(), pace);
    }
    else if (!strcmp(cmd, "bcm_suspend"))
    {
        /* bcm_suspend on|off: the job driver suspended, as after a
         * device stage play that left the chip on Apple's vmcs: no jobs
         * (bcmjob_have() false, FENCE too) until it resumes */
        bool on = argc >= 2 && !strcmp(argv[1], "on");

        if (argc < 2 || (!on && strcmp(argv[1], "off")))
            fprintf(stderr, "sim_script:%d: bcm_suspend on|off\n", lineno);
        else
        {
            bcmjob_suspend(on);
            fprintf(stderr, "sim_script: bcm_suspend %s\n", argv[1]);
        }
    }
    else if (!strcmp(cmd, "bcm_minor"))
    {
        if (argc < 2)
            fprintf(stderr, "sim_script:%d: bcm_minor <n>\n", lineno);
        else
            sim_bcm_minor((uint32_t)num(argv[1]));
    }
    else if (!strcmp(cmd, "bcm_flip"))
    {
        if (argc < 2)
            fprintf(stderr, "sim_script:%d: bcm_flip 0|1\n", lineno);
        else
            sim_bcm_bars_flip(num(argv[1]) != 0);
    }
    else if (!strcmp(cmd, "bcm_fault"))
        fault_cmd(argv, argc, lineno);
    else if (!strcmp(cmd, "bcm_charger"))
    {
        if (argc < 2 || !sim_bcm_charger(num(argv[1]) != 0))
            fprintf(stderr, "sim_script:%d: bcm_charger 0|1 (model "
                    "running?)\n", lineno);
    }
    else if (!strcmp(cmd, "bcm_advance"))
    {
        if (argc < 2 || !sim_bcm_advance((uint32_t)num(argv[1])))
            fprintf(stderr, "sim_script:%d: bcm_advance <ms> (model "
                    "running?)\n", lineno);
    }
    else if (!strcmp(cmd, "bcm_selftest"))
    {
        int r = bcmjob_selftest();

        printf("bcm selftest rc=%d\n", r);
        fflush(stdout);
    }
    else if (!sim_bcm_stats(&st))
        fprintf(stderr, "sim_script:%d: %s: the model is not running\n",
                lineno, cmd);
    else if (!strcmp(cmd, "bcm_stats"))
    {
        printf("bcm stats c0_ms=%lu c1_ms=%lu c2_ms=%lu cold=%lu done=%lu "
               "epoch=%lu dark_ups=%lu decode=%lu game=%lu video=%lu\n",
               (unsigned long)st.state_ms[0], (unsigned long)st.state_ms[1],
               (unsigned long)st.state_ms[2], (unsigned long)st.cold_starts,
               (unsigned long)st.jobs_done, (unsigned long)st.epoch,
               (unsigned long)st.dark_ups[0], (unsigned long)st.dark_ups[1],
               (unsigned long)st.dark_ups[2], (unsigned long)st.dark_ups[3]);
        fflush(stdout);
    }
    else    /* expect_bcm_dark_ups */
    {
        unsigned long want = argc >= 2 ? num(argv[1]) : 0;

        if (argc < 2)
            fprintf(stderr, "sim_script:%d: expect_bcm_dark_ups <n>\n",
                    lineno);
        else if (st.dark_ups[0] != want)
            fprintf(stderr, "sim_script:%d: expect_bcm failed: dark power-ups "
                    "want %lu, got %lu (decode=%lu game=%lu video=%lu)\n",
                    lineno, want, (unsigned long)st.dark_ups[0],
                    (unsigned long)st.dark_ups[1],
                    (unsigned long)st.dark_ups[2],
                    (unsigned long)st.dark_ups[3]);
        else
            fprintf(stderr, "sim_script: expect_bcm_dark_ups ok: %lu\n", want);
    }
    return true;
#endif
}
