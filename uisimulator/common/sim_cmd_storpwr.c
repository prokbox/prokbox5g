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

/* #114: sim script commands for the drive power model; see
 * sim_cmd_storpwr.h */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "config.h"
#include "kernel.h"
#include "core_alloc.h"
#include "settings.h"
#include "sim_cmd_storpwr.h"

#ifdef HAVE_STORAGE_POWEROFF
#include "stor_pwr.h"
#include "ipodui/devmode.h"

static uint32_t mark_ons;
static int buf_h;                       /* stor_buffer's reservation */
extern size_t audio_get_filebuflen(void);

static void status(const char *what)
{
    const struct stor_pwr *s = storage_power_state();

    fprintf(stderr, "sim_script: %s on=%d ons=%lu offs=%lu wasted=%lu "
            "since_mark=%lu on_s=%lu\n", what, s->on ? 1 : 0,
            (unsigned long)s->ons, (unsigned long)s->offs,
            (unsigned long)s->wasted, (unsigned long)(s->ons - mark_ons),
            (unsigned long)(stor_pwr_on_ticks(s, current_tick) / HZ));
}

bool sim_cmd_storpwr(const char *cmd, char *argv[], int argc, int lineno)
{
    const struct stor_pwr *s;

    if (strncmp(cmd, "stor_", 5) && strcmp(cmd, "drive_ab"))
        return false;
    s = storage_power_state();
    if (!strcmp(cmd, "stor_tune") && argc >= 3)
    {
        sim_stor_tune(atol(argv[1]), atol(argv[2]));
        fprintf(stderr, "sim_script: stor_tune hold=%s min_off=%s\n",
                argv[1], argv[2]);
    }
    else if (!strcmp(cmd, "stor_pwrup") && argc >= 2)
    {
        /* npfix: the modelled power-up wait, ms (default 300) */
        sim_stor_pwrup(atol(argv[1]));
        fprintf(stderr, "sim_script: stor_pwrup %s ms\n", argv[1]);
    }
    else if (!strcmp(cmd, "stor_watch_ui") && argc >= 2)
    {
        /* npfix: trace the UI thread's file calls ("sim io ui") */
        sim_stor_watch_ui(!strcmp(argv[1], "on"));
        fprintf(stderr, "sim_script: stor_watch_ui %s\n", argv[1]);
    }
    else if (!strcmp(cmd, "stor_switch") && argc >= 2)
    {
        storage_set_poweroff(!strcmp(argv[1], "on"));
        fprintf(stderr, "sim_script: stor_switch %s\n", argv[1]);
    }
    else if (!strcmp(cmd, "stor_expect_switch") && argc >= 2)
    {   /* the developer switch: "on" = off when idle (A), "off" = B */
        bool want = !strcmp(argv[1], "on");
        if (storage_poweroff_allowed() != want)
        {
            fprintf(stderr, "sim_script:%d: stor_expect_switch %s failed\n",
                    lineno, argv[1]);
        }
        else
            fprintf(stderr, "sim_script: stor_expect_switch %s ok\n",
                    argv[1]);
    }
#ifdef HAVE_IPOD_UI_DEVMODE
    else if (!strcmp(cmd, "drive_ab") && argc >= 2)
    {   /* #114 H3: the A/B, time-compressed: period/grace <ms>, start, stop */
        if (!strcmp(argv[1], "period") && argc >= 3)
            ipodui_ab_period_ms = atol(argv[2]);
        else if (!strcmp(argv[1], "grace") && argc >= 3)
            ipodui_ab_grace_ms = atol(argv[2]);
        else if (!strcmp(argv[1], "length") && argc >= 3)
            ipodui_devmode_ab_config(atoi(argv[2]), 0);
        else if (!strcmp(argv[1], "step") && argc >= 3)
            ipodui_devmode_ab_config(0, atoi(argv[2]));
        else if (!strcmp(argv[1], "start"))
            ipodui_devmode_ab_start();
        else if (!strcmp(argv[1], "stop"))
            ipodui_devmode_ab_stop();
        fprintf(stderr, "sim_script: drive_ab %s\n", argv[1]);
    }
#endif /* HAVE_IPOD_UI_DEVMODE */
    else if (!strcmp(cmd, "stor_buffer") && argc >= 2)
    {
        /* stor_buffer <bytes>: shrink the audio buffer to about this
         * (a core_alloc of the rest, while stopped), so refills come
         * seconds apart; 0 frees it */
        size_t want = strtoul(argv[1], NULL, 0), have;

        if (buf_h > 0)
            buf_h = core_free(buf_h);
        have = audio_get_filebuflen();
        /* the shrink keeps a reserve: step down until one fits */
        while (want && have > want && buf_h <= 0)
        {
            buf_h = core_alloc(have - want);
            want += 16384;
        }
        fprintf(stderr, "sim_script: stor_buffer %lu -> filebuflen=%lu\n",
                (unsigned long)want,
                (unsigned long)audio_get_filebuflen());
    }
    else if (!strcmp(cmd, "stor_save"))
    {
        /* a settings write, as the shell's own save does */
        long t0 = current_tick;
        settings_save();
        fprintf(stderr, "sim_script: stor_save ms=%ld\n",
                (current_tick - t0) * 1000 / HZ);
    }
    else if (!strcmp(cmd, "stor_mark"))
    {
        mark_ons = s->ons;
        status("stor_mark");
    }
    else if (!strcmp(cmd, "stor_status"))
        status("stor_status");
    else if (!strcmp(cmd, "stor_expect") && argc >= 2)
    {
        bool want = !strcmp(argv[1], "on");
        long max_ms = argc >= 3 ? atol(argv[2]) : 0, t = 0;

        while (s->on != want && t < max_ms)
        {
            sleep(HZ / 50);
            t += 20;
        }
        if (s->on != want)
            fprintf(stderr, "sim_script:%d: stor_expect %s failed after "
                    "%ld ms\n", lineno, argv[1], t);
        else
            fprintf(stderr, "sim_script: stor_expect %s ok at %ld ms\n",
                    argv[1], t);
    }
    else if (!strcmp(cmd, "stor_expect_cycles_ge") && argc >= 2)
    {
        unsigned long n = s->ons - mark_ons;
        if (n < strtoul(argv[1], NULL, 0))
            fprintf(stderr, "sim_script:%d: stor_expect_cycles_ge failed: "
                    "%lu < %s\n", lineno, n, argv[1]);
        else
            status("stor_expect_cycles_ge ok");
    }
    else if (!strcmp(cmd, "stor_expect_cycles_le") && argc >= 2)
    {
        unsigned long n = s->ons - mark_ons;
        if (n > strtoul(argv[1], NULL, 0))
            fprintf(stderr, "sim_script:%d: stor_expect_cycles_le failed: "
                    "%lu > %s\n", lineno, n, argv[1]);
        else
            status("stor_expect_cycles_le ok");
    }
    else
        return false;
    return true;
}
#else
bool sim_cmd_storpwr(const char *cmd, char *argv[], int argc, int lineno)
{
    (void)cmd; (void)argv; (void)argc; (void)lineno;
    return false;
}
#endif
