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
 * iPod UI shell: the paused maintenance window (Phase 16-1, #88)
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
#include "config.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "system.h"
#include "kernel.h"
#include "xcore.h"                  /* P19 S0: XCORE_BSS */
#include "core_alloc.h"
#include "audio.h"
#include "playback.h"
#include "pcmbuf.h"
#include "metadata.h"
#include "appevents.h"
#include "ipodui/maint.h"
#include "ipodui/maint_core.h"
#include "tagcache.h"         /* P19 19-1a: tagcache_quiet_kick() */
#include "ipodui/artcache.h"
#include "ipodui/uilog.h"
#include "ipodui/unlit.h"
#include "backlight.h"

typedef char maint_st_ok[(MAINT_ST_PLAY == AUDIO_STATUS_PLAY
                          && MAINT_ST_PAUSE == AUDIO_STATUS_PAUSE) ? 1 : -1];

#define SLOW_TICKS    (5*HZ)    /* a restart longer than this is logged */
#define GIVE_MARGIN   128       /* buflib headers: a hole of exactly
                                   'bytes' is not allocatable (M2) */
#define REGROW_MIN    (256*1024) /* free this much: give it back (M3) */

static volatile long paused_since = -1;
static volatile unsigned char wanted;       /* 1 << job */
static volatile unsigned char active;       /* 1 << job, took the window */
static long active_tick, job_tick[MAINT__N];
static volatile bool taking, play_during, hooked;
/* paused_since, refused (GCC range: every tenant starts at -2, none) */
static long declined[MAINT__N] = { [0 ... MAINT__N - 1] = -2 };
static bool was_open;
static volatile bool sleep_open;    /* P18: the sleep-entry window */
static volatile bool sleep_hold;    /* P18: sleep entry after the window */
static long regrown;            /* paused_since of the pause's one regrow */
/* one taker at a time (tagcache, art and library threads; H2) */
static struct mutex take_mtx XCORE_BSS;     /* P19 S0: either core */
static bool mtx_ready XCORE_BSS;
#ifdef SIMULATOR
static long sim_extra;          /* pause_age: added to the pause's age */
#define AGE_EXTRA sim_extra
#else
#define AGE_EXTRA 0
#endif

static const char * const job_name[MAINT__N] = {
    "commit", "chapters", "panels", "dbq", "lists", "xidx"
};


/* the window's restart reports paused and sends no events (playback.c),
 * so every pause event is the user's: none is ignored (L) */

static void take_lock(void)
{
    int level = disable_irq_save();

    if (!mtx_ready)
    {
        mutex_init(&take_mtx);
        mtx_ready = true;
    }
    restore_irq(level);
    mutex_lock(&take_mtx);
}

/* Playback stopped or paused (audio thread; the window's own silent
 * restart sends none, playback.c): a pause starts the age anew */
static void stop_or_pause(unsigned short id, void *data)
{
    (void)id; (void)data;
    paused_since = (audio_status() & AUDIO_STATUS_PAUSE) ? current_tick : -1;
    if (!audio_status())
        wanted = 0;             /* stopped: the jobs run as today */
#ifdef SIMULATOR
    sim_extra = 0;
#endif
    tagcache_quiet_kick();      /* P19 19-1a: a window or a stopped job */
}

static void track_pause(int st)
{
    if (!(st & AUDIO_STATUS_PAUSE))
        paused_since = -1;
    else if (paused_since < 0)
        paused_since = current_tick;
    else
        return;
#ifdef SIMULATOR
    sim_extra = 0;
#endif
}

static enum maint_why window(void)
{
    struct maint_in in;
    struct mp3entry *id3;

    in.status = audio_status();
    track_pause(in.status);
    if (sleep_hold)
        return MAINT_NO_LIT;    /* P18: no take, no regrow until the wake */
    in.paused_ticks = paused_since < 0 ? -1
                      : current_tick - paused_since + AGE_EXTRA;
    in.user_idle = artcache_user_idle();
    in.dark = ui_unlit_active() || !is_backlight_on(false);
    if (sleep_open && in.paused_ticks >= 0)
    {
        /* P18 Q7: the user has left; the pause age and the idle test
         * are waived for this window only */
        in.paused_ticks = MAINT_PAUSE_TICKS;
        in.user_idle = true;
        in.dark = true;
    }
    in.fading = pcmbuf_fading();
    id3 = in.status ? audio_current_track() : NULL;
    in.video = id3 && id3->is_video;
    return maint_core_window(&in, MAINT_PAUSE_TICKS);
}

bool maint_window_open(void)
{
    return window() == MAINT_OPEN;
}

void maint_want(enum maint_job j)
{
    wanted |= 1u << j;
}

bool maint_wanted(void)
{
    return wanted != 0;
}

void maint_sleep_window(bool open)
{
    sleep_open = open;
}

bool maint_sleep_window_open(void)
{
    return sleep_open;
}

void maint_sleep_hold(bool hold)
{
    sleep_hold = hold;
}

bool maint_wanted_job(enum maint_job j)
{
    return (wanted & (1u << j)) != 0;
}

void maint_pause_restart(void)
{
    /* P18 (Rev 1 M1): the tick was frozen while asleep; the pause age
     * starts again from the wake, so the window does not open at once */
    if (paused_since >= 0)
        paused_since = current_tick;
#ifdef SIMULATOR
    sim_extra = 0;
#endif
}

long maint_paused_since(void)
{
    return paused_since;
}

bool maint_window_due(long *at)
{
    long since = paused_since;

    if (since < 0)
        return false;
    *at = since + MAINT_PAUSE_TICKS - AGE_EXTRA;    /* window()'s age */
    return true;
}

size_t maint_floor(void)
{
#if MEMORYSIZE <= 32
    return MAINT_FLOOR_32MB;
#else
    return MAINT_FLOOR_64MB;
#endif
}

bool maint_fits(size_t bytes)
{
    return maint_core_fits(audio_buffer_size(), bytes, maint_floor());
}

bool maint_take_fits(size_t bytes)
{
    return audio_status() == 0
           || maint_core_take_fits(audio_buffer_size(), core_allocatable(),
                                   bytes, GIVE_MARGIN, maint_floor());
}

bool maint_active(void)
{
    if (active && TIME_AFTER(current_tick, active_tick + MAINT_JOB_TICKS))
    {
        ui_log("maint", "lapsed jobs=%x", (unsigned)active);
        active = 0;
    }
    return active != 0;
}

bool maint_holds(enum maint_job j)
{
    return maint_active() && (active & (1u << j));
}

void maint_done(enum maint_job j, bool ok)
{
    wanted &= ~(1u << j);
    if (!(active & (1u << j)))
        return;
    active &= ~(1u << j);
    if (audio_status() == AUDIO_STATUS_PLAY)
        play_during = true;             /* PLAY came while it ran */
    ui_log("maint", "job=%s ok=%d ms=%ld play_during=%d audio=%lu",
           job_name[j], (int)ok, (current_tick - job_tick[j]) * 1000 / HZ,
           (int)play_during, (unsigned long)audio_buffer_size());
    if (!active)
        play_during = false;
}

static void mark_active(enum maint_job j)
{
    if (!(active & (1u << j)))
        job_tick[j] = current_tick;
    if (!active)
        play_during = false;
    active |= 1u << j;
    active_tick = current_tick;
}

/* One audio-thread give (playback.c audio_on_give(): one event, the
 * song reported paused throughout); bytes 0 grows the buffer back.
 * Called with take_lock() held. */
static int give(size_t bytes, size_t floor, const char *what)
{
    struct audio_give g = { bytes, floor, 0, 0, 0, false };
    size_t before = audio_buffer_size();
    long t0 = current_tick, ms;
    int rc;

    taking = true;
    cpu_boost(true);
    rc = audio_buffer_give(&g);
    cpu_boost(false);
    taking = false;
    ms = (current_tick - t0) * 1000 / HZ;
    ui_log("maint", "take job=%s ok=%d need=%lu audio=%lu->%lu ms=%ld",
           what, rc, (unsigned long)bytes, (unsigned long)before,
           (unsigned long)audio_buffer_size(), ms);
    if (rc < 0)
    {
        /* nothing playable at the spot: stopped (playback.c logged it) */
        ui_log("maint", "restart FAILED job=%s", what);
        play_during = false;
    }
    else if (current_tick - t0 > SLOW_TICKS)
        ui_log("maint", "restart slow job=%s ms=%ld", what, ms);
    return rc;
}

bool maint_take(enum maint_job j, size_t bytes)
{
    enum maint_why why;
    size_t before;
    bool ok;

    if (audio_status() == 0)
        return true;                    /* stopped: callers as today */
    if (taking)                         /* another job's restart runs */
        ui_log("maint", "take queued job=%s", job_name[j]);
    take_lock();
    why = window();
    /* a job that allocates nothing (MAINT_PANELS, #100) never shrinks
     * the paused buffer: it only marks the window's use */
    if (!bytes || core_allocatable() >= bytes + 64)
    {
        if (why == MAINT_OPEN)
            mark_active(j);
        mutex_unlock(&take_mtx);
        return true;
    }
    wanted |= 1u << j;                  /* owed: no regrow meanwhile */
    before = audio_buffer_size();
    if (why != MAINT_OPEN || !maint_fits(bytes + GIVE_MARGIN))
    {
        if (why == MAINT_OPEN)
        {
            /* not this pause: may regrow (every refusal, not only the
             * first: a retry must not leave the job owed, P16) */
            wanted &= ~(1u << j);
            if (declined[j] != paused_since)
            {
                /* Q11: the update waits for a stop or a smaller commit */
                declined[j] = paused_since;
                ui_log("maint", "job=%s wait floor need=%lu audio=%lu "
                       "floor=%lu", job_name[j], (unsigned long)bytes,
                       (unsigned long)before, (unsigned long)maint_floor());
            }
        }
        mutex_unlock(&take_mtx);
        return false;
    }
    /* one front shrink on the audio thread, the floor checked again
     * there (H2); the paused song restarts silently, no regrow */
    ok = give(bytes + GIVE_MARGIN, maint_floor(), job_name[j]) == 1
         && core_allocatable() >= bytes + 64;
    if (ok)
        mark_active(j);
    else
        declined[j] = paused_since;     /* no retry loop this pause */
    mutex_unlock(&take_mtx);
    return ok;
}

bool maint_declined(enum maint_job j)
{
    return audio_status() && paused_since >= 0 && declined[j] == paused_since;
}

void maint_poll(void)
{
    bool open;
    int st = audio_status();

    if (!hooked)
    {
        hooked = true;
        add_event(PLAYBACK_EVENT_STOP_OR_PAUSE, stop_or_pause);
    }
    if (maint_active() && st == AUDIO_STATUS_PLAY && !play_during)
    {
        play_during = true;             /* PLAY: the job goes on */
        ui_log("maint", "play_during=1 jobs=%x audio=%lu",
               (unsigned)active, (unsigned long)audio_buffer_size());
    }
    open = st != 0 && window() == MAINT_OPEN;
    if (st == 0)
        artcache_maint_poll_stopped();  /* #92 M2: the stopped window */
    artcache_rotate_poll();             /* #100: a rotation at a wake */
    /* M3: the jobs are done and their memory is free again: give it
     * back to the audio buffer (the same silent restart), so windows
     * never walk the buffer down */
    /* at most once per pause (M-b): later allocations in the same pause
     * take from the regrown buffer by the same one-event restart */
    if (open && !maint_active() && !wanted && regrown != paused_since
        && core_allocatable() >= REGROW_MIN)
    {
        take_lock();
        if (audio_status() == (AUDIO_STATUS_PLAY | AUDIO_STATUS_PAUSE)
            && !active && !wanted && core_allocatable() >= REGROW_MIN)
        {
            regrown = paused_since;
            give(0, REGROW_MIN, "regrow");
        }
        mutex_unlock(&take_mtx);
    }
    if (open && !was_open)
    {
        ui_log("maint", "window open paused_s=%ld wanted=%x audio=%lu",
               (current_tick - paused_since + AGE_EXTRA) / HZ, (unsigned)wanted,
               (unsigned long)audio_buffer_size());
        if ((wanted & (1u << MAINT_CHAPTERS)) || artcache_maint_owed())
            artcache_maint_kick();
    }
    was_open = open;
}

#ifdef SIMULATOR
/* expect_title: what Now Playing would draw has a title and a length */
int sim_maint_title(char *buf, int len)
{
    struct mp3entry *id3 = audio_current_track();
    const char *t = id3 && id3->title ? id3->title : "";

    snprintf(buf, len, "%s length=%lu elapsed=%lu", t,
             id3 ? id3->length : 0, id3 ? id3->elapsed : 0);
    return id3 && t[0] && id3->length > 0;
}

void sim_maint_try(long bytes)
{
    bool ok = maint_take(MAINT_COMMIT, (size_t)bytes);
    size_t before = audio_buffer_size();
    int h = ok ? core_alloc((size_t)bytes) : 0;

    ui_log("maint", "try ok=%d alloc=%d shrink=%d", (int)ok, (int)(h > 0),
           (int)(audio_buffer_size() != before));
    if (h > 0)
        core_free(h);
    maint_done(MAINT_COMMIT, ok);
}

/* maint_hold <job> <bytes> / maint_drop <job> <ok>: a tenant takes the
 * window and holds an allocation across script commands, then frees it
 * and ends its tenancy (P16 gate fix: one tenant's end never ends
 * another's; the regrow waits for every one). Script thread. */
static int sim_held[MAINT__N];

static int sim_job(const char *name)
{
    for (int j = 0; j < MAINT__N; j++)
        if (!strcmp(name, job_name[j]))
            return j;
    return -1;
}

void sim_maint_hold(const char *job, long bytes)
{
    int j = sim_job(job);
    bool ok = j >= 0 && !sim_held[j] && maint_take(j, (size_t)bytes);
    int h = ok ? core_alloc((size_t)bytes) : 0;

    if (h > 0)
        sim_held[j] = h;
    ui_log("maint", "hold job=%s ok=%d alloc=%d audio=%lu", job, (int)ok,
           (int)(h > 0), (unsigned long)audio_buffer_size());
}

void sim_maint_drop(const char *job, bool ok)
{
    int j = sim_job(job);

    if (j < 0)
        return;
    if (sim_held[j] > 0)
        core_free(sim_held[j]);
    sim_held[j] = 0;
    maint_done(j, ok);
    ui_log("maint", "drop job=%s audio=%lu", job,
           (unsigned long)audio_buffer_size());
}

unsigned long sim_maint_audio_size(void)
{
    return audio_buffer_size();
}

void sim_maint_pause_age(long seconds)
{
    if (audio_status() & AUDIO_STATUS_PAUSE)
    {
        if (paused_since < 0)
            paused_since = current_tick;
        sim_extra = seconds * HZ - (current_tick - paused_since);
        tagcache_quiet_kick();  /* the window's deadline moved */
    }
}
#endif
