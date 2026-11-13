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

#ifdef HAVE_IPOD_UI
#include <stdlib.h>
#include "kernel.h"
#include "pcm_mixer.h"
#include "ipodui/header.h"

void sim_audio_load_hold(long ms);      /* apps/playback.c */
int sim_header_play_drawn(void);        /* apps/ipodui/header.c */

/* #345: waits up to ms for done(); returns the ms it took, -1 if never */
static long wait_for(bool (*done)(int), int arg, long ms)
{
    long start = current_tick, t = 0;

    while (!done(arg))
    {
        if (t >= ms)
            return -1;
        sleep(1);
        t = (current_tick - start) * 1000 / HZ;
    }
    return t;
}

static bool shown_is(int want)
{
    return sim_header_play_drawn() == want;
}

/* paused: the channel no longer plays (paused, or stopped by the
 * pause-to-standby path) */
static bool pcm_is(int want)
{
    enum channel_status st = mixer_channel_status(PCM_MIXER_CHAN_PLAYBACK);

    return want == CHANNEL_PLAYING ? st == CHANNEL_PLAYING
                                   : st != CHANNEL_PLAYING;
}

/* "expect_shown|expect_pcm paused|playing <ms>" */
static void expect_state(const char *cmd, char *argv[], int lineno)
{
    bool paused = !strcmp(argv[1], "paused");
    bool shown = !strcmp(cmd, "expect_shown");
    long t = wait_for(shown ? shown_is : pcm_is,
                      shown ? (paused ? UI_PLAY_PAUSED : UI_PLAY_PLAYING)
                            : (paused ? CHANNEL_PAUSED : CHANNEL_PLAYING),
                      atol(argv[2]));

    if (t >= 0)
        fprintf(stderr, "sim_script: %s ok %s after %ld ms\n", cmd, argv[1],
                t);
    else
        fprintf(stderr, "sim_script:%d: %s failed: not %s within %s ms "
                "(%s %d)\n", lineno, cmd, argv[1], argv[2],
                shown ? "drawn" : "pcm",
                shown ? sim_header_play_drawn()
                      : (int)mixer_channel_status(PCM_MIXER_CHAN_PLAYBACK));
}
#endif /* HAVE_IPOD_UI */

bool sim_cmd_audio(const char *cmd, char *argv[], int argc, int lineno)
{
#ifdef HAVE_IPOD_UI
    if (!strcmp(cmd, "load_hold") && argc >= 2)
    {
        sim_audio_load_hold(atol(argv[1]));
        fprintf(stderr, "sim_script: load_hold %s\n", argv[1]);
        return true;
    }
    if ((!strcmp(cmd, "expect_shown") || !strcmp(cmd, "expect_pcm"))
        && argc >= 3)
    {
        expect_state(cmd, argv, lineno);
        return true;
    }
#endif
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
