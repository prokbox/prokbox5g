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
 * Phase 4b sim script commands: "usb_connect"/"usb_disconnect",
 * "art_wait", "power_off_now", "disk_active", "expect_art", and (4b-1
 * end-review fixes) "audio_reload" and "art_poweroff_abort"
 * (docs/phase4b-brief.md). Kept apart
 * from sim_script.c as its own file (docs/process.md: "new sim commands
 * get their own sim_cmd_*.c file where practical"). Registered from
 * sim_script.c's run_line() via sim_cmd_art(); see sim_cmd_art.h.
 *
 * art_wait calls sim_art_builder_idle(), a weak hook: this file (and so
 * the sim it links into) builds cleanly before Agent B's art cache
 * builder (apps/ipodui/artcache.c) exists. The default (weak)
 * definition below always reports "unavailable"; once the builder
 * thread lands, its strong definition of the same symbol overrides this
 * one, and "art_wait" starts blocking on the real idle state with no
 * change needed to this file or to any *.script that already uses it.
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
#include "sim_tasks.h"
#include <limits.h>
#include "kernel.h"
#include "powermgmt.h"
#include "sim_cmd_art.h"
#include "audio.h"
#include "sim_db.h"

/* "Idle" state of the art cache builder thread, for "art_wait":
 *  -1  no builder in this build (the default here: either Agent B's
 *      artcache module hasn't landed yet, or this is a stock sim, which
 *      never builds one)
 *   0  still busy after timeout_ms
 *   1  idle (nothing queued, nothing decoding) within timeout_ms
 *
 * Declared weak so this file links before apps/ipodui/artcache.c
 * exists; B's module provides the strong override, same signature. */
int sim_art_builder_idle(long timeout_ms) __attribute__((weak));
int sim_art_builder_idle(long timeout_ms)
{
    (void)timeout_ms;
    return -1;
}

/* expect_art: the builder's per-album report (path of the album's
 * representative track, state), from apps/ipodui/artcache.c; weak like
 * the idle hook. */
int sim_art_report(void (*fn)(void *ctx, const char *path,
                              const char *state), void *ctx)
    __attribute__((weak));
int sim_art_report(void (*fn)(void *ctx, const char *path,
                              const char *state), void *ctx)
{
    (void)fn; (void)ctx;
    return -1;
}

/* art_request: artcache_build_request(); weak like the others */
void sim_art_request(void) __attribute__((weak));
void sim_art_request(void)
{
    fprintf(stderr, "sim_script: art_request unavailable (no art builder)\n");
}

void sim_art_progress_request(void) __attribute__((weak));
void sim_art_progress_request(void) { }
int sim_art_progress_check(void) __attribute__((weak));
int sim_art_progress_check(void) { return 0; }
int sim_art_persistence(const char *op) __attribute__((weak));
int sim_art_persistence(const char *op) { (void)op; return 0; }

/* art_poweroff_abort: the builder sees a power-off (SYS_POWEROFF on its
 * queue only) that is then aborted (misc.c clean_shutdown() refusing):
 * nothing else shuts down. Weak like the others. */
void sim_art_poweroff_abort(void) __attribute__((weak));
void sim_art_poweroff_abort(void)
{
    fprintf(stderr, "sim_script: art_poweroff_abort unavailable\n");
}

/* art_panel_hold: the builder keeps its next panel block that long
 * (H1 end review: a playback start while it is held) */
void sim_art_panel_hold(long ms) __attribute__((weak));
void sim_art_panel_hold(long ms)
{
    (void)ms;
    fprintf(stderr, "sim_script: art_panel_hold unavailable\n");
}

/* npfix review M1/M2: art_post_flood <n> posts n of each of the
 * builder's own events (raw, past art_post()'s one-of-each rule, and n
 * through it); expect_art_parked <0|1> [ms]: the builder is (not) parked
 * at a decoder yield point; sys_battery_update broadcasts the battery
 * monitor's event to every queue */
void sim_art_post_flood(long n) __attribute__((weak));
void sim_art_post_flood(long n)
{
    (void)n;
    fprintf(stderr, "sim_script: art_post_flood unavailable\n");
}

/* art_npf_hold <0|1> (review M4): the art thread's Now Playing bundle
 * read waits (Now Playing's 5 s fallback then runs) */
void sim_art_npf_hold(bool hold) __attribute__((weak));
void sim_art_npf_hold(bool hold)
{
    (void)hold;
    fprintf(stderr, "sim_script: art_npf_hold unavailable\n");
}

int sim_art_parked(void) __attribute__((weak));
int sim_art_parked(void)
{
    return -1;
}

/* art_decode_spin <us> (npfix): the builder busy-waits that long at each
 * decoder yield point (the ARM's decode cost; ipodui-np-stall) */
void sim_art_decode_spin(long us) __attribute__((weak));
void sim_art_decode_spin(long us)
{
    (void)us;
    fprintf(stderr, "sim_script: art_decode_spin unavailable\n");
}

/* art_panels_v1 / expect_panels_grouped (#92): panels.pak back to the
 * version 1 header; whether the published file is a grouped v2 */
/* #100: panel_rotate_warp <s>, panel_shown_mark, expect_panel_rotated,
 * expect_panel_books_random */
int sim_art_panels_valid(void) __attribute__((weak));
int sim_art_panels_valid(void)
{
    return -2;
}
void sim_art_rotate_warp(long seconds) __attribute__((weak));
void sim_art_rotate_warp(long seconds)
{
    (void)seconds;
    fprintf(stderr, "sim_script: panel_rotate_warp unavailable\n");
}
int sim_art_pool_shown(bool mark, int *nrand, int *brand, int *overlap,
                       uint32_t *rotations, int *bcand) __attribute__((weak));
int sim_art_pool_shown(bool mark, int *nrand, int *brand, int *overlap,
                       uint32_t *rotations, int *bcand)
{
    (void)mark; (void)nrand; (void)brand; (void)overlap; (void)rotations;
    (void)bcand;
    return -2;
}

void sim_art_panels_ungroup(void) __attribute__((weak));
void sim_art_panels_ungroup(void)
{
    fprintf(stderr, "sim_script: art_panels_v1 unavailable\n");
}
int sim_art_panels_grouped(void) __attribute__((weak));
int sim_art_panels_grouped(void)
{
    return -2;
}

/* audio_buf mark|expect: the audio buffer's size (buffering.c's, i.e.
 * audio_get_filebuflen()) now; "expect" fails when it is smaller than
 * at "mark" (memory a transient block took and the buffer never got
 * back) */
size_t audio_get_filebuflen(void);
static size_t audio_buf_marked;

static void audio_buf(const char *what, int lineno)
{
    size_t n = audio_get_filebuflen();

    if (!strcmp(what, "mark"))
    {
        audio_buf_marked = n;
        fprintf(stderr, "sim_script: audio_buf mark %zu\n", n);
    }
    else if (!strcmp(what, "expect") && n >= audio_buf_marked)
        fprintf(stderr, "sim_script: expect_audio_buf ok %zu (marked %zu)\n",
                n, audio_buf_marked);
    else if (!strcmp(what, "expect"))
        fprintf(stderr, "sim_script:%d: expect_audio_buf failed: %zu < "
                "marked %zu (%zu short)\n", lineno, n, audio_buf_marked,
                audio_buf_marked - n);
    else
        fprintf(stderr, "sim_script: audio_buf %zu\n", n);
}

/* disk_active: the art builder's storage_disk_is_active() in the sim
 * (hostfs has no disk state; apps/ipodui/artcache.c disk_active() models
 * the ATA driver around audio fills, and this sets it directly) */
long sim_disk_active_until;
extern const char *sim_script_file;

static void disk_active(long ms)
{
    long t = current_tick + ms * HZ / 1000;

    sim_disk_active_until = t ? t : 1;
    fprintf(stderr, "sim_script: disk_active %ld ms\n", ms);
}

/* --- expect_art <manifest> ------------------------------------------- */
#define MAN_MAX 64
struct man {
    int n;
    char dir[MAN_MAX][256];     /* "Library/Artist/Album" */
    char want[MAN_MAX][8];      /* OK / NOART / FAILED */
    char got[MAN_MAX][8];       /* "" = not reported */
    int albums, pending;
};

static void man_visit(void *ctx, const char *path, const char *state)
{
    struct man *m = ctx;
    const char *slash = strrchr(path, '/');
    int len = slash ? (int)(slash - path) : 0, i;

    m->albums++;
    if (!strcmp(state, "PENDING"))
        m->pending++;
    for (i = 0; i < m->n; i++)
    {
        int dl = strlen(m->dir[i]);

        const char *dot = strrchr(path, '.');
        int bl = dot && dot > slash ? (int)(dot - path) : -1;

        /* the track's directory ends with "/<manifest dir>", or the track
         * without its extension does (a one-file audiobook) */
        if ((len > dl && path[len - dl - 1] == '/'
             && !strncmp(path + len - dl, m->dir[i], dl))
            || (bl > dl && path[bl - dl - 1] == '/'
                && !strncmp(path + bl - dl, m->dir[i], dl)))
            snprintf(m->got[i], sizeof(m->got[i]), "%s", state);
    }
}

static void expect_art(const char *name, int lineno)
{
    char file[PATH_MAX], line[512];
    const char *slash = strrchr(sim_script_file, '/');
    const char *base = getenv("SIM_SCRIPT_DIR");
    struct man *m = calloc(1, sizeof(*m));
    FILE *f;
    int i, bad = 0, n;

    if (!m)
        return;
    if (name[0] == '/')
        snprintf(file, sizeof(file), "%s", name);
    else if (base && *base)
        snprintf(file, sizeof(file), "%s/%s", base, name);
    else
        snprintf(file, sizeof(file), "%.*s%s",
                 slash ? (int)(slash - sim_script_file) + 1 : 0,
                 sim_script_file, name);
    f = fopen(file, "r");
    if (!f)
    {
        fprintf(stderr, "sim_script:%d: expect_art failed: can't open %s\n",
                lineno, file);
        free(m);
        return;
    }
    while (m->n < MAN_MAX && fgets(line, sizeof(line), f))
    {
        char *tab, *tab2;

        if (line[0] == '#' || line[0] == '\n')
            continue;
        tab = strchr(line, '\t');
        if (!tab)
            continue;
        *tab++ = '\0';
        tab2 = strchr(tab, '\t');
        if (tab2)
            *tab2 = '\0';
        tab[strcspn(tab, "\r\n")] = '\0';
        snprintf(m->dir[m->n], sizeof(m->dir[m->n]), "%.255s", line);
        snprintf(m->want[m->n], sizeof(m->want[m->n]), "%.7s", tab);
        m->n++;
    }
    fclose(f);

    n = sim_art_report(man_visit, m);
    if (n < 0)
    {
        fprintf(stderr, "sim_script:%d: expect_art failed: no album table "
                "or no art builder\n", lineno);
        free(m);
        return;
    }
    for (i = 0; i < m->n; i++)
    {
        if (strcmp(m->got[i], m->want[i]))
        {
            fprintf(stderr, "sim_script:%d: expect_art: %s: want %s, got %s\n",
                    lineno, m->dir[i], m->want[i],
                    m->got[i][0] ? m->got[i] : "(no album)");
            bad++;
        }
    }
    if (m->pending)
    {
        fprintf(stderr, "sim_script:%d: expect_art: %d of %d albums still "
                "pending\n", lineno, m->pending, m->albums);
        bad++;
    }
    if (bad)
        fprintf(stderr, "sim_script:%d: expect_art failed (%d)\n", lineno,
                bad);
    else
        fprintf(stderr, "sim_script: expect_art ok (%d manifest albums, %d "
                "albums built)\n", m->n, m->albums);
    free(m);
}

static void art_wait(long timeout_ms, int lineno)
{
    int r = sim_art_builder_idle(timeout_ms);

    if (r < 0)
        fprintf(stderr, "sim_script: art_wait unavailable (no art "
                "builder hook in this build)\n");
    else if (r == 0)
        fprintf(stderr, "sim_script:%d: art_wait timed out after %ld ms\n",
                lineno, timeout_ms);
    else
        fprintf(stderr, "sim_script: art_wait ready after waiting up to "
                "%ld ms\n", timeout_ms);
}

bool sim_cmd_art(const char *cmd, char *argv[], int argc, int lineno)
{
    if (!strcmp(cmd, "usb_connect"))
    {
        fprintf(stderr, "sim_script: usb_connect\n");
        sim_trigger_usb(true);
    }
    else if (!strcmp(cmd, "usb_disconnect"))
    {
        fprintf(stderr, "sim_script: usb_disconnect\n");
        sim_trigger_usb(false);
    }
#ifdef HAVE_HEADPHONE_DETECTION
    else if (!strcmp(cmd, "headphones"))    /* headphones in|out */
    {
        bool in = argc >= 2 && !strcmp(argv[1], "in");

        fprintf(stderr, "sim_script: headphones %s\n", in ? "in" : "out");
        queue_broadcast(in ? SYS_PHONE_PLUGGED : SYS_PHONE_UNPLUGGED, 0);
    }
#endif
    else if (!strcmp(cmd, "art_wait"))
        art_wait(argc >= 2 ? atol(argv[1]) : 10000, lineno);
    else if (!strcmp(cmd, "art_request"))
    {
        fprintf(stderr, "sim_script: art_request\n");
        sim_art_request();
    }
    else if (!strcmp(cmd, "art_progress_request"))
        sim_art_progress_request();
    else if (!strcmp(cmd, "expect_art_progress"))
        fprintf(stderr, "sim_script: expect_art_progress %s\n",
                sim_art_progress_check() ? "ok" : "failed");
    else if (!strcmp(cmd, "snapshot_begin"))
        fprintf(stderr, "sim_script: expect_snapshot_begin %s\n",
                sim_db_snapshot_begin() ? "ok" : "failed");
    else if (!strcmp(cmd, "expect_snapshot_switch"))
        fprintf(stderr, "sim_script: expect_snapshot_switch %s\n",
                sim_db_snapshot_switch(argc >= 2 ? atol(argv[1]) : 30000)
                ? "ok" : "failed");
    else if (!strcmp(cmd, "snapshot_stress_begin"))
        fprintf(stderr, "sim_script: expect_snapshot_stress_begin %s\n",
                sim_db_snapshot_stress_begin() ? "ok" : "failed");
    else if (!strcmp(cmd, "expect_snapshot_stress_switch"))
        fprintf(stderr, "sim_script: expect_snapshot_stress_switch %s\n",
                sim_db_snapshot_stress_switch(argc >= 2 ? atol(argv[1]) : 30000)
                ? "ok" : "failed");
    else if (!strcmp(cmd, "snapshot_stale_begin"))
        fprintf(stderr, "sim_script: expect_snapshot_stale_begin %s\n",
                sim_db_stale_begin() ? "ok" : "failed");
    else if (!strcmp(cmd, "snapshot_stale_restore"))
        fprintf(stderr, "sim_script: expect_snapshot_stale_restore %s\n",
                sim_db_stale_restore() ? "ok" : "failed");
    else if (!strcmp(cmd, "snapshot_play"))
        fprintf(stderr, "sim_script: expect_snapshot_play %s\n",
                sim_db_snapshot_play() ? "ok" : "failed");
    else if (!strcmp(cmd, "snapshot_pause"))
        sim_db_snapshot_pause();
    else if (!strcmp(cmd, "expect_snapshot_deferred"))
        fprintf(stderr, "sim_script: expect_snapshot_deferred %s\n",
                sim_db_snapshot_deferred(argc >= 2 ? atol(argv[1]) : 3000)
                ? "ok" : "failed");
    else if (!strcmp(cmd, "expect_snapshot_after_playback"))
        fprintf(stderr, "sim_script: expect_snapshot_after_playback %s\n",
                sim_db_snapshot_after_playback(argc >= 2 ? atol(argv[1]) : 30000)
                ? "ok" : "failed");
    else if (!strcmp(cmd, "expect_snapshot_stale_finish"))
        fprintf(stderr, "sim_script: expect_snapshot_stale_finish %s\n",
                sim_db_stale_finish(argc >= 2 ? atol(argv[1]) : 30000)
                ? "ok" : "failed");
    else if (!strcmp(cmd, "art_persist") && argc >= 2)
        fprintf(stderr, "sim_script: expect_art_persist %s (%s)\n",
                sim_art_persistence(argv[1]) ? "ok" : "failed", argv[1]);
    else if (!strcmp(cmd, "disk_active") && argc >= 2)
        disk_active(atol(argv[1]));
    else if (!strcmp(cmd, "audio_reload"))
    {
        /* a real refill, without a key (a key pauses the art builder):
         * what a playlist change does; buffering.c ends it with
         * BUFFER_EVENT_FILL_DONE and storage_sleep() */
        fprintf(stderr, "sim_script: audio_reload\n");
        audio_flush_and_reload_tracks();
    }
    else if (!strcmp(cmd, "art_poweroff_abort"))
    {
        fprintf(stderr, "sim_script: art_poweroff_abort\n");
        sim_art_poweroff_abort();
    }
    else if (!strcmp(cmd, "expect_panels_valid"))
    {
        int v = sim_art_panels_valid();

        if (v > 0)
            fprintf(stderr, "sim_script: expect_panels_valid ok covers=%d\n",
                    v);
        else
            fprintf(stderr, "sim_script:%d: expect_panels_valid failed (%d)\n",
                    lineno, v);
    }
    else if (!strcmp(cmd, "panel_rotate_warp") && argc >= 2)
    {
        fprintf(stderr, "sim_script: panel_rotate_warp %s s\n", argv[1]);
        sim_art_rotate_warp(atol(argv[1]));
    }
    else if (!strcmp(cmd, "panel_shown_mark")
             || !strcmp(cmd, "expect_panel_rotated")
             || !strcmp(cmd, "expect_panel_kept")
             || !strcmp(cmd, "expect_panel_books_random"))
    {
        int nr = 0, br = 0, ov = 0, bc = 0, n;
        uint32_t rot = 0;
        bool mark = !strcmp(cmd, "panel_shown_mark");
        bool ok;

        n = sim_art_pool_shown(mark, &nr, &br, &ov, &rot, &bc);
        if (mark)
            ok = n > 0;
        else if (!strcmp(cmd, "expect_panel_rotated"))
            ok = n > 0 && rot > 0 && nr > 0 && ov == 0; /* fresh picks */
        else if (!strcmp(cmd, "expect_panel_kept"))
            ok = n > 0 && rot == 0;
        else                                        /* Q10 */
            ok = n > 0 && br <= 3 && (bc == 0 || br >= (bc < 3 ? bc : 3));
        fprintf(stderr, "sim_script%s%.0d: %s %s shown=%d rand=%d "
                "books_rand=%d books_cand=%d overlap=%d rotations=%lu\n",
                ok ? "" : ":", ok ? 0 : lineno, cmd, ok ? "ok" : "failed", n,
                nr, br, bc, ov, (unsigned long)rot);
    }
    else if (!strcmp(cmd, "art_panels_ungroup"))
        sim_art_panels_ungroup();
    else if (!strcmp(cmd, "expect_panels_grouped"))
    {
        int v = sim_art_panels_grouped();

        if (v >= 1)
            fprintf(stderr, "sim_script: expect_panels_grouped ok v=%d\n", v);
        else
            fprintf(stderr, "sim_script:%d: expect_panels_grouped failed "
                    "(%d)\n", lineno, v);
    }
    else if (!strcmp(cmd, "art_post_flood") && argc >= 2)
    {
        fprintf(stderr, "sim_script: art_post_flood %s\n", argv[1]);
        sim_art_post_flood(atol(argv[1]));
    }
    else if (!strcmp(cmd, "expect_art_parked") && argc >= 2)
    {
        int want = atoi(argv[1]), got;
        long ms = argc >= 3 ? atol(argv[2]) : 2000, waited = 0;

        while ((got = sim_art_parked()) != want && got >= 0 && waited < ms)
        {
            sleep(HZ / 20);
            waited += 50;
        }
        if (got == want)
            fprintf(stderr, "sim_script: expect_art_parked ok: %d after "
                    "%ld ms\n", got, waited);
        else
            fprintf(stderr, "sim_script:%d: expect_art_parked failed: %d "
                    "(want %d) after %ld ms\n", lineno, got, want, waited);
    }
    else if (!strcmp(cmd, "art_npf_hold") && argc >= 2)
    {
        fprintf(stderr, "sim_script: art_npf_hold %s\n", argv[1]);
        sim_art_npf_hold(atoi(argv[1]) != 0);
    }
    else if (!strcmp(cmd, "sys_battery_update"))
    {
        fprintf(stderr, "sim_script: sys_battery_update\n");
        queue_broadcast(SYS_BATTERY_UPDATE, 0);
    }
    else if (!strcmp(cmd, "art_decode_spin") && argc >= 2)
    {
        fprintf(stderr, "sim_script: art_decode_spin %s us\n", argv[1]);
        sim_art_decode_spin(atol(argv[1]));
    }
    else if (!strcmp(cmd, "art_panel_hold") && argc >= 2)
    {
        fprintf(stderr, "sim_script: art_panel_hold %s ms\n", argv[1]);
        sim_art_panel_hold(atol(argv[1]));
    }
    else if (!strcmp(cmd, "audio_buf"))
        audio_buf(argc >= 2 ? argv[1] : "", lineno);
    else if (!strcmp(cmd, "expect_art") && argc >= 2)
        expect_art(argv[1], lineno);
    else if (!strcmp(cmd, "power_off_now"))
    {
        fprintf(stderr, "sim_script: power_off_now\n");
        sys_poweroff();
    }
    else
        return false;

    return true;
}
