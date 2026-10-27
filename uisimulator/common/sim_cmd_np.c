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
 * Phase 5 sim script commands; see sim_cmd_np.h.
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
#include <stdlib.h>
#include <string.h>
#include "config.h"
#include "system.h"
#include "kernel.h"
#include "file.h"
#include "audio.h"
#include "playlist.h"
#include "settings.h"
#include "backlight.h"
#include "metadata.h"
#include "powermgmt.h"
#include "sim_db.h"
int sim_mkdir(const char *path);   /* filesystem-sim.c */
#include "sim_cmd_np.h"

/* apps/ipodui/np_playback.c's SIMULATOR-only setter; weak so a stock sim
 * (which never links any apps/ipodui file) still builds. */
void np_pb_sim_pin(long elapsed_ms, int volume_pct) __attribute__((weak));
void np_pb_sim_pin(long elapsed_ms, int volume_pct)
{
    (void)elapsed_ms; (void)volume_pct;
}

/* The audio thread's per-slot seqlock hooks (np_playback.c, P6 Rev 1
 * item 3); weak for the stock sim. -1 = every slot. */
void np_pb_ab_write_begin(int slot) __attribute__((weak));
void np_pb_ab_write_begin(int slot) { (void)slot; }
void np_pb_ab_write_end(int slot) __attribute__((weak));
void np_pb_ab_write_end(int slot) { (void)slot; }
uint32_t np_pb_ab_seq(int slot) __attribute__((weak));
uint32_t np_pb_ab_seq(int slot) { (void)slot; return 0; }

static char *unesc(char *p)
{
    char *r = p, *w = p;

    while (*r)
    {
        if (r[0] == '%' && r[1] == '2' && r[2] == '0')
        {
            *w++ = ' ';
            r += 3;
        }
        else
            *w++ = *r++;
    }
    *w = '\0';
    return p;
}

/* npfix: lib_shuffle.c (weak like the others: a build without it) */
void sim_shuf_refill_force(void) __attribute__((weak));
void sim_shuf_refill_force(void)
{
    fprintf(stderr, "sim_script: shuf_refill_force unavailable\n");
}

/* npfix review L1/L2: shuf_chunk_hold <0|1> (a chunked refill stops
 * after its first chunk while set); shuf_expect_settled (no refill open,
 * the record names the queue) */
void sim_shuf_chunk_hold(bool hold) __attribute__((weak));
void sim_shuf_chunk_hold(bool hold)
{
    (void)hold;
    fprintf(stderr, "sim_script: shuf_chunk_hold unavailable\n");
}

void sim_shuf_expect_settled(int lineno) __attribute__((weak));
void sim_shuf_expect_settled(int lineno)
{
    (void)lineno;
    fprintf(stderr, "sim_script: shuf_expect_settled unavailable\n");
}

bool sim_cmd_np(const char *cmd, char *argv[], int argc, int lineno)
{
    if (!strcmp(cmd, "np_seq_bump"))
    {
        /* a seqlock write with no track change, as a rewrite of the
         * chapter table under the same track makes (the script thread
         * stands in for the audio thread: it writes nothing else) */
        np_pb_ab_write_begin(-1);
        np_pb_ab_write_end(-1);
        fprintf(stderr, "sim_script: np_seq_bump\n");
        return true;
    }
    if (!strcmp(cmd, "np_pin") && argc >= 2)
    {
        long ms = strcmp(argv[1], "off") ? atol(argv[1]) : -1;
        int vol = (argc >= 3 && strcmp(argv[2], "off")) ? atoi(argv[2]) : -1;

        np_pb_sim_pin(ms, vol);
        fprintf(stderr, "sim_script: np_pin %ld %d\n", ms, vol);
        return true;
    }
    if ((!strcmp(cmd, "wait_pos") || !strcmp(cmd, "wait_index"))
        && argc >= 2)
    {
        /* wait_pos <ms> [timeout]: until the current track's position
         * reaches <ms>; wait_index <n> [timeout]: until the queue's display
         * index (1-based) is <n>. Real playback progress instead of a
         * fixed wall-clock wait (the sim's audio runs slower than real
         * time on a loaded host) */
        bool pos = !strcmp(cmd, "wait_pos");
        long want = atol(argv[1]), waited = 0, got = -1;
        long wait = argc >= 3 ? atol(argv[2]) : 60000;
        /* wait_pos <ms> <timeout> <name>: only once the playing file's
         * path contains <name> (a new queue starts with the old track
         * still current) */
        const char *name = pos && argc >= 4 ? unesc(argv[3]) : NULL;

        for (;;)
        {
            struct mp3entry *id3 = audio_current_track();

            if (name && id3 && !strstr(id3->path, name))
                id3 = NULL;
            got = pos ? (id3 ? (long)id3->elapsed : -1)
                      : (long)playlist_get_display_index();
            if ((pos ? got >= want : got == want) || waited >= wait)
                break;
            sleep(HZ / 20);
            waited += 50;
        }
        if (pos ? got >= want : got == want)
            fprintf(stderr, "sim_script: %s ok: %ld after %ld ms\n", cmd,
                    got, waited);
        else
            fprintf(stderr, "sim_script:%d: %s failed: %ld (want %ld) after "
                    "%ld ms\n", lineno, cmd, got, want, waited);
        return true;
    }
    if (!strcmp(cmd, "charger") && argc >= 2)
    {
#if CONFIG_CHARGING
        charger_input_state = strcmp(argv[1], "off") ? CHARGER_PLUGGED
                                                     : CHARGER_UNPLUGGED;
#ifdef HAVE_IPOD_UI
        /* the device's insert interrupt kicks the power thread on the
         * edge (usb_insert_int(), firewire_insert_int()); without it a
         * dark battery playback's 10 s step (P19 19-1a) would see the
         * plug up to 10 s late */
        power_thread_kick();
#endif
#endif
        fprintf(stderr, "sim_script: charger %s\n", argv[1]);
        return true;
    }
    if (!strcmp(cmd, "expect_rating") && argc >= 3)
    {
        long want = atol(argv[2]), alt = want, waited = 0;
        long wait = argc >= 4 ? atol(argv[3]) : 3000;
        long got;
        char *bar = strchr(argv[2], '|');

        if (bar)                /* "a|b": either value passes */
            alt = atol(bar + 1);

        unesc(argv[1]);
        while ((got = sim_db_rating(argv[1])) != want && got != alt
               && waited < wait)
        {
            sleep(HZ / 20);
            waited += 50;
        }
        if (got == want || got == alt)
            fprintf(stderr, "sim_script: expect_rating ok: %s = %ld\n",
                    argv[1], got);
        else
            fprintf(stderr, "sim_script:%d: expect_rating failed: %s is %ld, "
                    "want %ld (-2: not in the database)\n", lineno, argv[1],
                    got, want);
        return true;
    }
    if (!strcmp(cmd, "expect_pos") && argc >= 3)
    {
        struct mp3entry *id3 = audio_current_track();
        long lo = atol(argv[1]), hi = atol(argv[2]);
        long e = id3 ? (long)id3->elapsed : -1;

        if (e >= lo && e <= hi)
            fprintf(stderr, "sim_script: expect_pos ok: %ld ms\n", e);
        else
            fprintf(stderr, "sim_script:%d: expect_pos failed: %ld ms not in "
                    "%ld..%ld\n", lineno, e, lo, hi);
        return true;
    }
    if (!strcmp(cmd, "expect_albums") && argc >= 2)
    {
        /* the next <n> queue entries from the current one: every album
         * forms one contiguous run (an album never reappears after another
         * album began) */
        int n = atoi(argv[1]), idx = 0, i, runs = 0, untagged = 0;
        char seen[64][64], cur[64] = "";
        int nseen = 0;
        bool good = true;
        struct playlist_track_info info;

        playlist_get_resume_info(&idx);
        for (i = idx; i < idx + n && i < playlist_amount(); i++)
        {
            char alb[64];
            int k;

            if (playlist_get_track_info(NULL, i, &info) < 0)
                break;
            if (!sim_db_album(info.filename, alb, sizeof(alb)))
            {
                untagged++;
                continue;
            }
            if (!strcmp(alb, cur))
                continue;
            for (k = 0; k < nseen; k++)
                if (!strcmp(seen[k], alb))
                    good = false;       /* an album that already ended */
            if (nseen < 64)
                strcpy(seen[nseen++], alb);
            strcpy(cur, alb);
            runs++;
        }
        if (argc >= 3 && runs > atoi(argv[2]))
            good = false;               /* not grouped: too many runs */
        if (good && runs > 0)
            fprintf(stderr, "sim_script: expect_albums ok: %d runs in %d "
                    "entries (%d untagged)\n", runs, n, untagged);
        else
            fprintf(stderr, "sim_script:%d: expect_albums failed: %d runs (max %s), "
                    "or an album repeats after another began (queue from %d)\n",
                    lineno, runs, argc >= 3 ? argv[2] : "-", idx);
        return true;
    }
    if (!strcmp(cmd, "play_files") && argc >= 2)
    {
        int i;

        playlist_create(NULL, NULL);
        for (i = 1; i < argc; i++)
            if (playlist_insert_track(NULL, unesc(argv[i]),
                                      PLAYLIST_INSERT_LAST, false, true) < 0)
                fprintf(stderr, "sim_script:%d: play_files: insert failed: %s\n",
                        lineno, argv[i]);
        playlist_start(0, 0, 0);
        fprintf(stderr, "sim_script: play_files %d\n", argc - 1);
        return true;
    }
    if (!strcmp(cmd, "repeat") && argc >= 2)
    {
        global_settings.repeat_mode = !strcmp(argv[1], "all") ? REPEAT_ALL
            : !strcmp(argv[1], "one") ? REPEAT_ONE : REPEAT_OFF;
        fprintf(stderr, "sim_script: repeat %s\n", argv[1]);
        return true;
    }
    if (!strcmp(cmd, "shuf_chunk_hold") && argc >= 2)
    {
        fprintf(stderr, "sim_script: shuf_chunk_hold %s\n", argv[1]);
        sim_shuf_chunk_hold(atoi(argv[1]) != 0);
        return true;
    }
    if (!strcmp(cmd, "shuf_expect_settled"))
    {
        sim_shuf_expect_settled(lineno);
        return true;
    }
    if (!strcmp(cmd, "shuf_refill_force"))
    {
        /* npfix: the next UI pass begins a (chunked) shuffle refill */
        sim_shuf_refill_force();
        fprintf(stderr, "sim_script: shuf_refill_force\n");
        return true;
    }
    if (!strcmp(cmd, "dark") && argc >= 2)
    {
        if (!strcmp(argv[1], "on"))
            backlight_off();
        else
            backlight_on();
        fprintf(stderr, "sim_script: dark %s\n", argv[1]);
        return true;
    }
    if (!strcmp(cmd, "mkdir") && argc >= 2)
    {
        sim_mkdir(unesc(argv[1]));
        fprintf(stderr, "sim_script: mkdir %s\n", argv[1]);
        return true;
    }
    return false;
}
