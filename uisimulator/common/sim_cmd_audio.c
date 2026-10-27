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
 * #38 integration fix-up (docs/phase-nav-brief.md) sim script command
 * "audio_stop"; see sim_cmd_audio.h. Links in both the stock and the
 * --ipodui sim (audio.h declares audio_stop() for every build that has
 * playback at all; no apps/ header needed).
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
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "config.h"
#include "audio.h"
#include "sim_cmd_audio.h"
#ifdef HAVE_VIZ_TAP
#include <stdlib.h>
#include "kernel.h"
#include "ipodui/viz_tap.h"

/* viz_tap <ms> [n] [sound|book|quiet]: the visualizer's tap opened from the
 * script thread, read about every 30 ms for ms, closed; one summary line,
 * then "viz_tap ok" or "viz_tap failed" against the expectation */
static void viz_tap_cmd(char *argv[], int argc, int lineno)
{
    long ms = atol(argv[1]), end;
    unsigned n = argc >= 3 ? (unsigned)atoi(argv[2]) : 512;
    const char *want = argc >= 4 ? argv[3] : "sound";
    size_t bytes = viz_tap_bytes(n);
    void *mem = malloc(bytes);
    int16_t *out = malloc(2 * n);
    int rc, reads = 0, fresh = 0, stale = 0, silent = 0, book = 0;
    int torn = 0, under = 0, peak = 0;
    unsigned rate = 0;
    bool ok;

    rc = viz_tap_open(n, mem, bytes);
    if (rc == VIZ_TAP_OK)
    {
        end = current_tick + ms * HZ / 1000;
        while (TIME_BEFORE(current_tick, end))
        {
            struct viz_tap_frame f;
            unsigned fl;

            sleep(HZ * 3 / 100);
            fl = viz_tap_read(out, 0, &f);
            reads++;
            fresh += fl == 0;
            stale += !!(fl & VIZ_TAP_STALE);
            silent += !!(fl & VIZ_TAP_SILENT);
            book += !!(fl & VIZ_TAP_BOOK);
            torn += !!(fl & VIZ_TAP_TORN);
            under += !!(fl & VIZ_TAP_UNDERRUN);
            if (f.peak > peak)
                peak = f.peak;
            rate = f.rate;
        }
        viz_tap_close();
    }
    fprintf(stderr, "sim_script: viz_tap n=%u bytes=%u open=%d reads=%d "
            "fresh=%d stale=%d silent=%d book=%d torn=%d underrun=%d "
            "peak=%d rate=%u\n", n, (unsigned)bytes, rc, reads, fresh, stale,
            silent, book, torn, under, peak, rate);
    if (!strcmp(want, "book"))
        ok = rc == VIZ_TAP_E_BOOK || (reads > 0 && book == reads);
    else if (!strcmp(want, "quiet"))
        ok = rc == VIZ_TAP_OK && reads > 0 && fresh == 0;
    else
        ok = rc == VIZ_TAP_OK && torn == 0 && fresh * 2 >= reads
             && peak >= 100;
    if (ok)
        fprintf(stderr, "sim_script: viz_tap ok (%s)\n", want);
    else
        fprintf(stderr, "sim_script:%d: viz_tap failed (want %s)\n", lineno,
                want);
    free(out);
    free(mem);
}
#endif

bool sim_cmd_audio(const char *cmd, char *argv[], int argc, int lineno)
{
    if (!strcmp(cmd, "audio_stop"))
    {
        audio_stop();
        fprintf(stderr, "sim_script: audio_stop\n");
        return true;
    }
#ifdef HAVE_VIZ_TAP
    if (!strcmp(cmd, "viz_tap") && argc >= 2)
    {
        viz_tap_cmd(argv, argc, lineno);
        return true;
    }
#endif
    (void)argv; (void)argc; (void)lineno;
    return false;
}
